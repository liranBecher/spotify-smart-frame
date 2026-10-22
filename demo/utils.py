import json
import urllib.error
import urllib.parse
import urllib.request


def request_json(url, *, data=None, headers=None, missing_ok=False, timeout=15):
    """Send an HTTP request and return ``(status_code, decoded_json)``.

    ``missing_ok`` turns an expected 404 into ``(404, None)``. Other HTTP
    failures keep their response details so ``--debug`` can explain them.
    """
    request = urllib.request.Request(url, data=data, headers=headers or {})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, json.load(response) if response.status != 204 else None
    except urllib.error.HTTPError as error:
        if missing_ok and error.code == 404:
            return error.code, None
        detail = error.read().decode("utf-8", errors="replace")
        host = urllib.parse.urlparse(url).netloc
        raise RuntimeError(f"{host} returned HTTP {error.code}: {detail}") from error

def format_ms(milliseconds):
    """Format milliseconds as ``minutes:seconds`` for terminal output."""
    seconds = max(0, milliseconds // 1000)
    return f"{seconds // 60}:{seconds % 60:02d}"
