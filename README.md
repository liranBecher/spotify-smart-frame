# Spotify Smart Frame

A standalone Spotify now-playing display built for the **JC3248W535C ESP32-S3**. The device connects to Spotify and [LRCLIB](https://lrclib.net/) over Wi-Fi and shows album art, track details, progress, and synchronized lyrics when available. A computer is needed for the first Spotify authorization and for flashing; it does not need to stay on afterward.

| Directory | Purpose |
| --- | --- |
| [`firmware/spotify_frame/`](firmware/spotify_frame/) | Main product: ESP32-S3 Arduino sketch, font assets, and configuration template |
| [`firmware/display_test/`](firmware/display_test/) | Display hardware test sketch |
| [`demo/`](demo/) | Optional desktop validator and live browser demo; also obtains the initial Spotify refresh token |

## Set up the frame

1. Create an app in the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard) and add `http://127.0.0.1:8888/callback` as a redirect URI.
2. Follow the [desktop authorization steps](demo/README.md#authorize-spotify) once to create `demo/.spotify_token.json`.
3. In Arduino IDE, install **ArduinoJson**, **JPEGDEC**, **GFX Library for Arduino**, and the [JC3248W535 driver](https://github.com/me-processware/JC3248W535-Driver).
4. Copy `firmware/spotify_frame/config.h.example` to `firmware/spotify_frame/config.h`. Fill in your Wi-Fi name and password, your Spotify client ID and secret, and the `refresh_token` from `demo/.spotify_token.json`.
5. Open [`firmware/spotify_frame/spotify_frame.ino`](firmware/spotify_frame/spotify_frame.ino) in Arduino IDE. Select **ESP32S3 Dev Module**, **16MB flash**, and **OPI PSRAM**. Verify and upload.

The refresh token is stored in ESP32 NVS if Spotify rotates it. If Spotify revokes it, run the desktop authorization again, update `config.h`, and upload the sketch. The idle screen shows an Israel-time clock and three short-term top artists; this requires the `user-top-read` scope requested by the desktop authorization flow.

## Operation

The frame checks playback every 10 seconds while music is playing and every 30 seconds while paused or idle. Song position and lyrics advance locally between checks. A tap requests an immediate check, subject to throttling and Spotify's quota cooldown. A manual skip may take up to 10 seconds to appear; playback starting from idle may take up to 30 seconds. The frame also prefetches the next track's lyrics near the end of a song.

Spotify HTTP 429 responses pause further Spotify requests for the returned `Retry-After` period. These poll intervals are calibration settings, not a verified safe quota. Running the desktop demo at the same time adds requests. The sketch logs endpoints, request counts, status, and uptime to Serial Monitor so you can measure your own usage before changing `PLAYING_POLL_MS` or `IDLE_POLL_MS`.

The backlight dims from 23:00 to 06:00 Israel time. Adjust `NIGHT_BACKLIGHT` in the sketch (0–255; currently 40) and re-upload to tune it.

## Privacy and project notes

`demo/.env`, `demo/.spotify_token.json`, and `firmware/spotify_frame/config.h` contain secrets and are ignored by Git. Commit only their `.example` templates. See [the project plan](PLAN.md) for the original vision and longer-term ideas.
