

import json
from pathlib import Path
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

import lyrics


WEB_STATE = {"playing": False}

def update_web_state(playback, lyric=None, lyric_next=None):
    """Publish a browser-safe snapshot without exposing Spotify credentials."""
    global WEB_STATE
    if not playback:
        WEB_STATE = {"playing": False}
        return
    track = playback["item"]
    album = track.get("album", {})
    images = album.get("images", [])
    WEB_STATE = {
        "playing": True,
        "title": track["name"],
        "artist": ", ".join(artist["name"] for artist in track["artists"]),
        "artwork": images[0]["url"] if images else None,
        "progress_ms": lyrics.local_progress(playback),
        "duration_ms": track.get("duration_ms", 0),
        "is_playing": playback.get("is_playing", False),
        "lyric": lyric,
        "lyric_next": lyric_next,
    }


def start_web_server(port=8000):
    """Serve the mockup and current state on localhost in a background thread."""

    class WebHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            path = urllib.parse.urlparse(self.path).path
            if path == "/api/state":
                body = json.dumps(WEB_STATE).encode()
                content_type = "application/json; charset=utf-8"
            elif path in ("/", "/index.html"):
                body = Path(__file__).with_name("index.html").read_bytes()
                content_type = "text/html; charset=utf-8"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, _format, *_args):
            pass

    server = HTTPServer(("127.0.0.1", port), WebHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"Display: http://127.0.0.1:{port}")
