"""Diagnostics Viewer: view profiling sessions in a browser or terminal."""

import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import sys
import tempfile
import time
from urllib.parse import parse_qs, urlparse
import webbrowser


DEFAULT_DIRECTORY = Path(tempfile.gettempdir()) / "Diagnostics"


class TailReader:
    """Read complete JSON lines, retaining a partially written last line."""

    def __init__(self, path):
        self.path = Path(path)
        self.stream = self.path.open("rb")
        self.pending = b""
        self.header = self._decode(self.stream.readline(65536)) or {}
        # Limit startup work/memory for a session that has been running for hours.
        size = self.path.stat().st_size
        if size > 262144:
            self.stream.seek(size - 262144)
            self.stream.readline()

    @staticmethod
    def _decode(line):
        try:
            item = json.loads(line.decode("utf-8", errors="replace"))
            return item if isinstance(item, dict) else None
        except (ValueError, UnicodeError):
            return None

    def poll(self):
        if self.path.stat().st_size < self.stream.tell():
            self.stream.seek(0)
            self.pending = b""
        self.pending += self.stream.read(65536)
        lines = self.pending.split(b"\n")
        self.pending = lines.pop()
        if len(self.pending) > 262144:
            self.pending = b""
        return [item for line in lines if (item := self._decode(line)) is not None]

    def close(self):
        self.stream.close()


def session_paths(directory):
    try:
        return sorted(directory.glob("session-*.jsonl"),
                      key=lambda path: path.stem.rsplit("-", 1)[-1], reverse=True)[:100]
    except OSError:
        return []


def terminal(directory, fixed_file):
    reader = None
    waiting = False
    try:
        while True:
            if reader is None:
                paths = [fixed_file] if fixed_file else session_paths(directory)
                if paths and paths[0].exists():
                    reader = TailReader(paths[0])
                    print(f"Following {reader.path}", flush=True)
                elif not waiting:
                    print(f"Waiting for a diagnostics session in {directory}", flush=True)
                    waiting = True
            if reader is not None:
                for item in reader.poll():
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
                        reader.close()
                        if fixed_file:
                            return
                        old_path = reader.path
                        reader = None
                        # Wait for a new instance rather than reopening the old file.
                        while not (paths := session_paths(directory)) or paths[0] == old_path:
                            time.sleep(0.25)
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        if reader is not None:
            reader.close()


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
        end = data.rfind(b"\n") + 1
        records = [item for line in data[:end].splitlines()
                   if (item := TailReader._decode(line)) is not None]
        return {"records": records, "offset": offset + end, "reset": reset}


def make_server(directory, fixed_file=None, port=0):
    page = Path(__file__).with_suffix(".html").read_bytes()
    directory = directory.resolve()

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
                            header = TailReader._decode(stream.readline(65536)) or {}
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
