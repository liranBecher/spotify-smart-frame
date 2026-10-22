# Desktop and web demo

This optional Python stack validates Spotify playback and lyrics on a computer. With `--web`, it serves a live browser version of the display at `http://127.0.0.1:8000`. The **ESP32 firmware is the main product** and runs without this demo after setup.

## Authorize Spotify

From the repository root, install the dependency and copy the template once if `demo/.env` does not already exist:

```powershell
python -m pip install -r demo/requirements.txt
Copy-Item demo/.env.example demo/.env
```

Create an app in the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard) and register `http://127.0.0.1:8888/callback` as a redirect URI. Enter its client ID and secret in `demo/.env`, then run:

```powershell
python demo/spotify_now_playing.py
```

Authorize in the browser. The resulting `demo/.spotify_token.json` contains a refresh token for the ESP32 setup. Keep both files private.

## Run the browser demo

```powershell
python demo/spotify_now_playing.py --watch --web
```

Open `http://127.0.0.1:8000`. The demo polls Spotify roughly once a minute and advances song position and lyrics locally between polls. `--watch` alone prints playback and lyrics in the terminal. Add `--debug` for API request timing and match details.

An offline check is available without credentials:

```powershell
python demo/spotify_now_playing.py --self-test
```

Running this demo alongside the frame increases Spotify API usage.
