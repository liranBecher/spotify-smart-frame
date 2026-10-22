from pathlib import Path
import json
import secrets
import time
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer
import base64

import utils


AUTH_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"
PLAYBACK_URL = "https://api.spotify.com/v1/me/player/currently-playing"
QUEUE_URL = "https://api.spotify.com/v1/me/player/queue"
REDIRECT_URI = "http://127.0.0.1:8888/callback"
TOKEN_FILE = Path(__file__).with_name(".spotify_token.json")


def token_request(client_id, client_secret, fields):
    """Exchange OAuth form fields for a Spotify token response."""
    credentials = base64.b64encode(f"{client_id}:{client_secret}".encode()).decode()
    _, token = utils.request_json(
        TOKEN_URL,
        data=urllib.parse.urlencode(fields).encode(),
        headers={
            "Authorization": f"Basic {credentials}",
            "Content-Type": "application/x-www-form-urlencoded",
        },
    )
    token["expires_at"] = time.time() + token["expires_in"]
    return token


def authorize(client_id, client_secret):
    """Run Spotify's one-time browser authorization-code flow.

    A temporary localhost server receives Spotify's redirect. The random
    ``state`` value prevents an unrelated browser request from supplying a code.
    """
    state = secrets.token_urlsafe(24)
    query = urllib.parse.urlencode(
        {
            "client_id": client_id,
            "response_type": "code",
            "redirect_uri": REDIRECT_URI,
            "scope": "user-read-currently-playing user-read-playback-state user-top-read",
            "state": state,
        }
    )
    result = {}

    class CallbackHandler(BaseHTTPRequestHandler):
        """Handle the single OAuth redirect sent back by Spotify."""

        def do_GET(self):
            """Validate the callback and retain its query parameters."""
            parsed = urllib.parse.urlparse(self.path)
            values = urllib.parse.parse_qs(parsed.query)
            if parsed.path != "/callback" or values.get("state", [None])[0] != state:
                self.send_error(400, "Invalid authorization callback")
                return
            result.update({key: value[0] for key, value in values.items()})
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write(b"Spotify authorized. You can close this tab.")

        def log_message(self, _format, *_args):
            """Silence the temporary server's default access log."""
            pass

    url = f"{AUTH_URL}?{query}"
    print(f"Open this URL if your browser does not open:\n{url}\n")
    webbrowser.open(url)
    server = HTTPServer(("127.0.0.1", 8888), CallbackHandler)
    server.timeout = 180
    server.handle_request()
    server.server_close()

    if "error" in result:
        raise RuntimeError(f"Spotify authorization failed: {result['error']}")
    if "code" not in result:
        raise RuntimeError("Spotify authorization timed out")
    return token_request(
        client_id,
        client_secret,
        {
            "grant_type": "authorization_code",
            "code": result["code"],
            "redirect_uri": REDIRECT_URI,
        },
    )


def get_token(client_id, client_secret):
    """Return a valid access token, refreshing or authorizing when necessary."""
    token = json.loads(TOKEN_FILE.read_text()) if TOKEN_FILE.exists() else None
    if token and "user-top-read" not in token.get("scope", "").split():
        token = None
    if token and token.get("expires_at", 0) > time.time() + 60:
        return token
    if token and token.get("refresh_token"):
        refreshed = token_request(
            client_id,
            client_secret,
            {"grant_type": "refresh_token", "refresh_token": token["refresh_token"]},
        )
        refreshed.setdefault("refresh_token", token["refresh_token"])
        refreshed.setdefault("scope", token.get("scope", ""))
        token = refreshed
    else:
        token = authorize(client_id, client_secret)
    TOKEN_FILE.write_text(json.dumps(token, indent=2))
    return token
