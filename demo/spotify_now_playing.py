#!/usr/bin/env python3
"""Track Spotify playback and print synchronized lyrics.

The program has two different clocks:

1. Spotify is polled every minute to correct track, pause, and seek state.
2. Between polls, ``time.monotonic()`` advances the lyric position locally every
   250 ms. This avoids excessive API calls and prevents short lyric lines from
   being skipped.

Lyrics are loaded in background threads because an LRCLIB cache miss may take
several seconds. During a track's final 65 seconds, the next queued track is
looked up once so its lyrics are usually ready before it starts.
"""

import argparse
import logging
import os
from pathlib import Path
import time
import urllib.error

from dotenv import load_dotenv

from utils import format_ms, request_json

import spotify_api
import lyrics
import web_server


def print_playback(access_token):
    """Poll Spotify, print track metadata, and schedule background preparation."""
    _, playback = request_json(
        spotify_api.PLAYBACK_URL, headers={"Authorization": f"Bearer {access_token}"}
    )
    if not playback or not playback.get("item"):
        print("Nothing is currently playing.")
        return None
    playback["polled_at"] = time.monotonic()
    track = playback["item"]
    artists = ", ".join(artist["name"] for artist in track["artists"])
    progress = format_ms(playback.get("progress_ms", 0))
    duration = format_ms(track.get("duration_ms", 0))
    album = track.get("album", {})
    images = album.get("images", [])
    print(f"{track['name']} - {artists} [{progress}/{duration}]")
    print(f"Artwork: {images[0]['url'] if images else 'unavailable'}")
    lyrics.schedule_lyrics(track)
    remaining = track.get("duration_ms", 0) - playback.get("progress_ms", 0)
    if (
        playback.get("is_playing")
        and remaining <= 65_000
        and track.get("id") not in lyrics.PREFETCHED_TRACKS
    ):
        logging.debug("Scheduling queue prefetch with %.1fs remaining", remaining / 1000)
        lyrics.PREFETCHED_TRACKS.add(track.get("id"))
        lyrics.WORKERS.submit(lyrics.prefetch_next, access_token)
    return playback


def self_test():
    """Check pure formatting, LRC boundary, and paused-clock behavior offline."""
    assert format_ms(0) == "0:00"
    assert format_ms(61_999) == "1:01"
    assert format_ms(-1) == "0:00"
    lines = lyrics.parse_lrc(
        "[00:01.00]First line\n[00:02.00]\n[00:03.50]Second line\n[00:05.00]"
    )
    assert lyrics.current_lyric(lines, 999) is None
    assert lyrics.current_lyric(lines, 1_000) == "First line"
    assert lyrics.current_lyric(lines, 3_500) == "Second line"
    assert lyrics.current_lyric(lines, 5_000) == ""
    assert lyrics.next_lyric(lines, 1_000) == "Second line"
    assert lyrics.next_lyric(lines, 3_500) is None
    assert not lyrics.has_usable_lyrics(lyrics.parse_lrc("[00:00.00][Music]\n[02:00.00]"))
    assert lyrics.has_usable_lyrics(lyrics.parse_lrc("[00:00.00][Music]\n[00:10.00]Verse"))
    candidates = [
        {"trackName": "Dance Me to the End of Love", "artistName": "Leonard Cohen",
         "albumName": "Other Album", "duration": 280,
         "syncedLyrics": "[00:00.00][Music]"},
        {"trackName": "Dance Me to the End of Love", "artistName": "Leonard Cohen",
         "albumName": "Other Album", "duration": 281,
         "syncedLyrics": "[00:10.00]Verse"},
    ]
    assert lyrics.matching_synced(candidates, "Dance Me to the End of Love",
                                  "Leonard Cohen", "Various Positions", 280) is candidates[1]
    live = {"trackName": "No More Tears (live from the let go letters tour)",
            "artistName": "Avery Anna", "albumName": "No More Tears - Single",
            "duration": 231, "syncedLyrics": "[00:10.00]Verse"}
    assert lyrics.base_title("No More Tears - live from the let go letters tour") == "No More Tears"
    assert lyrics.matching_synced([live], "No More Tears - live from the let go letters tour",
                                  "Avery Anna", "No More Tears - Single", 231) is live
    paused = {"progress_ms": 1_234, "is_playing": False, "item": {"duration_ms": 5_000}}
    assert lyrics.local_progress(paused) == 1_234
    print("Self-test passed.")


def main():
    """Parse options and run the Spotify/local-timing event loop."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--watch", action="store_true", help="poll every minute")
    parser.add_argument("--web", action="store_true", help="serve the live browser display")
    parser.add_argument("--debug", action="store_true", help="show API timing and status")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.debug else logging.WARNING,
        format="%(levelname)s: %(message)s",
    )
    if args.self_test:
        self_test()
        return

    load_dotenv(Path(__file__).with_name(".env"))
    client_id = os.environ.get("SPOTIFY_CLIENT_ID")
    client_secret = os.environ.get("SPOTIFY_CLIENT_SECRET")
    if not client_id or not client_secret:
        raise SystemExit("Set SPOTIFY_CLIENT_ID and SPOTIFY_CLIENT_SECRET in .env.")
    if args.web:
        web_server.start_web_server()

    playback = None
    last_track = None
    last_lyric = None
    next_line = None
    next_poll = 0
    while True:
        # Network state is corrected every minute.
        if time.monotonic() >= next_poll:
            token = spotify_api.get_token(client_id, client_secret)
            try:
                playback = print_playback(token["access_token"])
            except RuntimeError as error:
                response = error.__cause__
                if not isinstance(response, urllib.error.HTTPError) or response.code != 429:
                    raise
                try:
                    delay = max(60, int(response.headers.get("Retry-After", "60")))
                except ValueError:
                    delay = 60
                print(f"{error}; retrying in {delay} seconds")
                next_poll = time.monotonic() + delay
            else:
                next_poll = time.monotonic() + 60
            track_id = playback["item"].get("id") if playback else None
            if track_id != last_track:
                last_track = track_id
                last_lyric = None
                next_line = None

        # Lyric selection is local, cheap, and intentionally much more frequent.
        lyric = None
        if playback:
            lyric = lyrics.lyric_status(playback)
            signature = lyrics.track_signature(playback["item"])
            payload = lyrics.LYRICS_CACHE.get(signature)
            if payload and payload.get("syncedLyrics"):
                lines = lyrics.parse_lrc(payload["syncedLyrics"])
                next_line = lyrics.next_lyric(
                    lines,
                    lyrics.local_progress(playback),
                ) if lyrics.has_usable_lyrics(lines) else None
            else:
                next_line = None
            if lyric != last_lyric:
                print(f"Lyrics: {lyric}")
                last_lyric = lyric
        web_server.update_web_state(playback, lyric, next_line)

        if not (args.watch or args.web) and not any(
            not future.done() for future in lyrics.LYRICS_FUTURES.values()
        ):
            break
        time.sleep(0.25)


if __name__ == "__main__":
    main()
