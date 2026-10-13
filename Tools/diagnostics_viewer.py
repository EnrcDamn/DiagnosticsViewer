"""Diagnostics Viewer: view profiling sessions in a browser or terminal."""

import argparse
import json
from html import escape
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import sys
import tempfile
import time
from urllib.parse import parse_qs, urlparse
import webbrowser


DEFAULT_DIRECTORY = Path(tempfile.gettempdir()) / "Diagnostics"


def decode_record(line):
    try:
        item = json.loads(line.decode("utf-8", errors="replace"))
        return item if isinstance(item, dict) else None
    except (ValueError, UnicodeError):
        return None


def session_paths(directory):
    try:
        # The date and time prefix makes filename order chronological.
        return sorted(directory.glob("session-*-pid-*-instance-*.jsonl"),
                      key=lambda path: path.name, reverse=True)[:100]
    except OSError:
        return []


def terminal(directory, fixed_file):
    current_path = None
    offset = 0
    waiting = False
    try:
        while True:
            if current_path is None:
                paths = [fixed_file] if fixed_file else session_paths(directory)
                if paths and paths[0].exists():
                    current_path = paths[0]
                    offset = 0
                    print(f"Following {current_path}", flush=True)
                elif not waiting:
                    print(f"Waiting for a diagnostics session in {directory}", flush=True)
                    waiting = True
            if current_path is not None:
                chunk = read_chunk(current_path, offset)
                offset = chunk["offset"]
                for item in chunk["records"]:
                    kind = item.get("type")
                    if kind == "metrics":
                        print(f"{item['time']:8.2f}s  load {item['load']:6.1f}% "
                              f"peak {item['peak_load']:6.1f}%  "
                              f"mean/p99/max {item['mean_ms']:.3f}/"
                              f"{item['p99_ms']:.3f}/{item['peak_ms']:.3f} ms  "
                              f"over budget {item['over_budget']}  "
                              f"dropped {item['dropped_blocks']}", flush=True)
                    elif kind == "log":
                        print(f"{item['time']:8.2f}s [{item['level']}] {item['text']}", flush=True)
                    elif kind == "closed":
                        print("Session closed.", flush=True)
                        if fixed_file:
                            return
                        old_path = current_path
                        current_path = None
                        # Wait for a new instance rather than reopening the old file.
                        while not (paths := session_paths(directory)) or paths[0] == old_path:
                            time.sleep(0.25)
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass


def read_chunk(path, offset):
    """Stateless, bounded reads shared by browser clients; no open file is retained."""
    with path.open("rb") as stream:
        size = path.stat().st_size
        reset = offset > size
        if reset:
            offset = 0
        if offset == 0 and size > 262144:
            stream.seek(size - 262144)
            stream.readline(65536)
            offset = stream.tell()
        stream.seek(offset)
        data = stream.read(65536)
        # Finish a long record without retaining a separate partial-line buffer.
        if b"\n" not in data and len(data) == 65536:
            data += stream.readline(262145 - len(data))
        end = data.rfind(b"\n") + 1
        if end == 0 and len(data) > 262144:
            end = len(data)  # Discard an oversized incomplete record and keep polling.
            data = b""
        records = [item for line in data[:end].splitlines()
                   if (item := decode_record(line)) is not None]
        return {"records": records, "offset": offset + end, "reset": reset}


def make_server(directory, fixed_file=None, port=0):
    directory = directory.resolve()
    saved_directory = fixed_file.resolve().parent if fixed_file else directory
    label = ("%TEMP%/Diagnostics" if sys.platform == "win32" and
             saved_directory == DEFAULT_DIRECTORY.resolve() else str(saved_directory))
    page = Path(__file__).with_suffix(".html").read_bytes()
    page = page.replace(b"__SAVE_DIRECTORY_FULL__", escape(str(saved_directory)).encode("utf-8"))
    page = page.replace(b"__SAVE_DIRECTORY__", escape(label).encode("utf-8"))

    def allowed_paths():
        if fixed_file:
            return [fixed_file] if fixed_file.is_file() else []
        return [path for path in session_paths(directory) if path.resolve().parent == directory]

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            request = urlparse(self.path)
            try:
                if request.path == "/":
                    self.respond(page, "text/html; charset=utf-8")
                elif request.path == "/api/sessions":
                    sessions = []
                    for path in allowed_paths():
                        with path.open("rb") as stream:
                            header = decode_record(stream.readline(65536)) or {}
                        sessions.append({"name": path.name, "application": header.get("application"),
                                         "pid": header.get("pid"), "instance": header.get("instance")})
                    self.respond(json.dumps(sessions).encode(), "application/json")
                elif request.path == "/api/poll":
                    params = parse_qs(request.query)
                    name = params.get("session", [""])[0]
                    offset = int(params.get("offset", ["0"])[0])
                    matches = [path for path in allowed_paths() if path.name == name]
                    if not matches or offset < 0:
                        self.send_error(400, "Unknown session or invalid offset")
                        return
                    result = read_chunk(matches[0], offset)
                    self.respond(json.dumps(result).encode(), "application/json")
                else:
                    self.send_error(404)
            except (OSError, ValueError):
                self.send_error(400, "Session unavailable")

        def respond(self, data, content_type):
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *_):
            pass

    return ThreadingHTTPServer(("127.0.0.1", port), Handler)


def browser(directory, fixed_file, port, no_browser):
    with make_server(directory, fixed_file, port) as server:
        url = f"http://127.0.0.1:{server.server_port}/"
        print(f"Diagnostics Viewer: {url}\nSessions: {directory}\nCtrl+C stops the viewer.", flush=True)
        if not no_browser:
            webbrowser.open(url, new=1)
        try:
            server.serve_forever(poll_interval=0.2)
        except KeyboardInterrupt:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=DEFAULT_DIRECTORY)
    parser.add_argument("--file", type=Path, help="Follow a particular instance/session")
    parser.add_argument("--terminal", action="store_true", help="Print live metrics and logs")
    parser.add_argument("--port", type=int, default=0, help="Browser server port (default: automatic)")
    parser.add_argument("--no-browser", action="store_true", help="Print the URL without opening it")
    args = parser.parse_args()
    if args.terminal:
        terminal(args.directory, args.file)
    else:
        try:
            browser(args.directory, args.file, args.port, args.no_browser)
        except (OSError, ValueError) as error:
            print(f"Cannot open the viewer: {error}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
