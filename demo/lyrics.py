

from concurrent.futures import ThreadPoolExecutor
import logging
import re
import time
import urllib.error
import urllib.parse

from spotify_api import QUEUE_URL
from utils import request_json


LYRICS_URL = "https://lrclib.net/api/get"
LYRICS_CACHED_URL = "https://lrclib.net/api/get-cached"
LYRICS_SEARCH_URL = "https://lrclib.net/api/search"
LRC_TIMESTAMP = re.compile(r"\[(\d+):(\d+(?:\.\d+)?)\]")
# Completed lookups, active background jobs, and tracks already prefetched.
LYRICS_CACHE = {}
LYRICS_FUTURES = {}
PREFETCHED_TRACKS = set()
WORKERS = ThreadPoolExecutor(max_workers=2, thread_name_prefix="spotify-frame")


def parse_lrc(lyrics):
    """Convert LRC text into sorted ``(timestamp_ms, lyric_text)`` pairs.

    One LRC line may contain multiple timestamps. Empty lyric text is retained
    because it intentionally clears the previous line during instrumental gaps.
    """
    lines = []
    for raw_line in lyrics.splitlines():
        timestamps = list(LRC_TIMESTAMP.finditer(raw_line))
        if not timestamps:
            continue
        text = LRC_TIMESTAMP.sub("", raw_line).strip()
        for timestamp in timestamps:
            minutes, seconds = timestamp.groups()
            lines.append((round((int(minutes) * 60 + float(seconds)) * 1000), text))
    return sorted(lines)


def has_usable_lyrics(lines):
    """Ignore LRC files made entirely of music markers and blank lines."""
    return any(text and text.strip().casefold() != "[music]" for _, text in lines)


def has_usable_synced(lyrics):
    return bool(lyrics and lyrics.get("syncedLyrics") and
                has_usable_lyrics(parse_lrc(lyrics["syncedLyrics"])))


def normalized_title(value):
    return re.sub(r"[\W_]+", " ", value.casefold()).strip()


def base_title(value):
    return re.split(r" \(| - ", value, maxsplit=1)[0]


def matching_synced(results, track_name, artist_name, album_name, duration):
    """Pick a timed version of the same recording, even on another album."""
    primary_artist = artist_name.split(", ")[0].casefold()
    full_title = normalized_title(track_name)
    short_title = normalized_title(base_title(track_name))
    matches = [item for item in results if
               (normalized_title(item.get("trackName", "")) == full_title or
                (item.get("albumName", "").casefold() == album_name.casefold() and
                 normalized_title(item.get("trackName", "")) == short_title))
               and primary_artist in item.get("artistName", "").casefold()
               and abs(item.get("duration", 0) - duration) <= 3
               and has_usable_synced(item)]
    return min(matches, key=lambda item: (
        normalized_title(item["trackName"]) != full_title,
        item.get("albumName", "").casefold() != album_name.casefold(),
        abs(item["duration"] - duration)), default=None)


def current_lyric(lines, progress_ms):
    """Return the most recent lyric at ``progress_ms``, or ``None`` before it."""
    current = None
    for timestamp, text in lines:
        if timestamp > progress_ms:
            break
        current = text
    return current

def next_lyric(lines, progress_ms):
    """Return the next non-empty, distinct lyric after ``progress_ms``."""
    current = current_lyric(lines, progress_ms)
    for timestamp, text in lines:
        if timestamp > progress_ms and text and text != current:
            return text
    return None


def track_signature(track):
    """Build the exact title/artist/album/duration tuple LRCLIB expects."""
    artists = ", ".join(artist["name"] for artist in track["artists"])
    album = track.get("album", {})
    return (
        track["name"],
        artists,
        album.get("name", ""),
        round(track["duration_ms"] / 1000),
    )


def get_lyrics(track_name, artist_name, album_name, duration):
    """Fetch and cache one LRCLIB result.

    The internal-only endpoint is tried first because it is fast. A cache miss
    falls back to LRCLIB's slower endpoint, which may query external sources.
    Confirmed misses are cached; temporary network failures are not, allowing a
    later Spotify poll to retry them.
    """
    signature = (track_name, artist_name, album_name, duration)
    if signature in LYRICS_CACHE:
        return LYRICS_CACHE[signature]
    query = urllib.parse.urlencode(
        {
            "track_name": track_name,
            "artist_name": artist_name,
            "album_name": album_name,
            "duration": duration,
        }
    )
    final_not_found = False
    fallback = None
    for endpoint, timeout in ((LYRICS_CACHED_URL, 5), (LYRICS_URL, 30)):
        url = f"{endpoint}?{query}"
        started = time.perf_counter()
        try:
            status, lyrics = request_json(
                url,
                headers={"User-Agent": "spotify-smart-frame/0.1"},
                missing_ok=True,
                timeout=timeout,
            )
        except (TimeoutError, urllib.error.URLError, RuntimeError) as error:
            logging.debug(
                "LRCLIB failed after %.2fs: %s (%s)",
                time.perf_counter() - started,
                url,
                error,
            )
            continue
        logging.debug(
            "LRCLIB HTTP %s in %.2fs: %s",
            status,
            time.perf_counter() - started,
            url,
        )
        if lyrics is not None:
            logging.debug(
                "LRCLIB match id=%s synced=%s instrumental=%s",
                lyrics.get("id"),
                bool(lyrics.get("syncedLyrics")),
                lyrics.get("instrumental", False),
            )
            if has_usable_synced(lyrics):
                LYRICS_CACHE[signature] = lyrics
                return lyrics
            fallback = lyrics
            continue
        final_not_found = endpoint == LYRICS_URL and status == 404

    if fallback and fallback.get("instrumental"):
        LYRICS_CACHE[signature] = fallback
        return fallback

    try:
        search_query = urllib.parse.urlencode({
            "track_name": base_title(track_name),
            "artist_name": artist_name.split(", ")[0],
        })
        _, results = request_json(
            f"{LYRICS_SEARCH_URL}?{search_query}",
            headers={"User-Agent": "spotify-smart-frame/0.1"},
            timeout=10,
        )
        match = matching_synced(results or [], track_name, artist_name, album_name, duration)
        if match:
            logging.debug("LRCLIB alternate synced match id=%s", match.get("id"))
            LYRICS_CACHE[signature] = match
            return match
    except (TimeoutError, urllib.error.URLError, RuntimeError) as error:
        logging.debug("LRCLIB search failed: %s", error)

    if fallback is not None or final_not_found:
        LYRICS_CACHE[signature] = fallback
    return fallback


def schedule_lyrics(track):
    """Start a non-blocking lyric lookup unless it is cached or already running."""
    signature = track_signature(track)
    future = LYRICS_FUTURES.get(signature)
    if signature not in LYRICS_CACHE and (not future or future.done()):
        logging.debug("Scheduling lyrics: %s - %s", signature[0], signature[1])
        LYRICS_FUTURES[signature] = WORKERS.submit(get_lyrics, *signature)
    return signature


def prefetch_next(access_token):
    """Fetch the first queued track and warm its lyric cache in the background."""
    try:
        _, queue = request_json(
            QUEUE_URL, headers={"Authorization": f"Bearer {access_token}"}
        )
        next_track = next(
            (item for item in queue.get("queue", []) if item.get("type") == "track"),
            None,
        )
        if next_track:
            signature = track_signature(next_track)
            logging.debug("Prefetching queued track: %s - %s", signature[0], signature[1])
            get_lyrics(*signature)
        else:
            logging.debug("Spotify queue has no next track")
    except (TimeoutError, urllib.error.URLError, RuntimeError) as error:
        logging.debug("Queue prefetch failed: %s", error)


def local_progress(playback):
    """Estimate current progress from the last Spotify value and elapsed time.

    Paused playback does not advance. Every Spotify poll replaces this estimate,
    correcting natural clock drift and user seeks.
    """
    progress = playback.get("progress_ms", 0)
    if playback.get("is_playing"):
        progress += round((time.monotonic() - playback["polled_at"]) * 1000)
    return min(progress, playback["item"].get("duration_ms", progress))


def lyric_status(playback):
    """Return the current lyric or a short loading/unavailable status."""
    signature = track_signature(playback["item"])
    future = LYRICS_FUTURES.get(signature)
    if future and not future.done():
        return "Loading..."
    if future:
        try:
            future.result()
        except Exception as error:
            logging.debug("Lyrics worker failed: %s", error)
        LYRICS_FUTURES.pop(signature, None)
    if signature not in LYRICS_CACHE or not LYRICS_CACHE[signature]:
        return "Unavailable"
    lyrics = LYRICS_CACHE[signature]
    if lyrics.get("instrumental"):
        return "Instrumental"
    if not lyrics.get("syncedLyrics"):
        return "Unavailable (not synchronized)"
    lines = parse_lrc(lyrics["syncedLyrics"])
    if not has_usable_lyrics(lines):
        return "Unavailable (not synchronized)" if lyrics.get("plainLyrics") else "Unavailable"
    line = current_lyric(lines, local_progress(playback))
    return line or "♪"
