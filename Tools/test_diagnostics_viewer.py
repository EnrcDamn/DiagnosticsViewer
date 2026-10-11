import json
from pathlib import Path
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import urlopen

from diagnostics_viewer import TailReader, make_server, read_chunk, session_paths


SESSION_NAME = "session-2026-10-11_10-00-00.000000-pid-1-instance-2.jsonl"


class ReaderTests(unittest.TestCase):
    def directory(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        return Path(directory.name)

    def test_partial_line_and_multiple_writes(self):
        path = self.directory() / SESSION_NAME
        path.write_bytes(b'{"type":"session"}\n{"type":"log","text":"caf')
        reader = TailReader(path)
        self.addCleanup(reader.close)
        self.assertEqual(reader.header["type"], "session")
        self.assertEqual(reader.poll(), [])
        with path.open("ab") as output:
            output.write('è"}\ninvalid line\n{"type":"closed"}\n'.encode())
        self.assertEqual(reader.poll(), [{"type": "log", "text": "cafè"},
                                         {"type": "closed"}])

    def test_truncated_file_recovers(self):
        path = self.directory() / SESSION_NAME
        path.write_text('{"type":"session","application":"a long name"}\n'
                        '{"type":"closed"}\n', encoding="utf-8")
        reader = TailReader(path)
        self.addCleanup(reader.close)
        reader.poll()
        path.write_text('{"type":"log"}\n', encoding="utf-8")
        self.assertEqual(reader.poll(), [{"type": "log"}])

    def test_large_session_reads_recent_history(self):
        path = self.directory() / SESSION_NAME
        with path.open("wb") as output:
            output.write(b'{"type":"session"}\n')
            for i in range(20000):
                output.write((json.dumps({"type": "metrics", "sequence": i}) + "\n").encode())
        reader = TailReader(path)
        self.addCleanup(reader.close)
        records = []
        for _ in range(6):
            records.extend(reader.poll())
        self.assertGreater(records[0]["sequence"], 0)
        self.assertEqual(records[-1]["sequence"], 19999)

    def test_missing_directory(self):
        self.assertEqual(session_paths(Path("does-not-exist")), [])

    def test_session_list_orders_filenames_by_date_and_time(self):
        directory = self.directory()
        names = ["session-2026-10-12_00-00-00.000000-pid-1-instance-1.jsonl",
                 "session-2026-10-11_10-00-02.000002-pid-2-instance-1.jsonl",
                 "session-2026-10-11_10-00-02.000001-pid-9000-instance-20.jsonl",
                 "session-2026-10-10_23-59-59.999999-pid-9999-instance-99.jsonl"]
        for name in reversed(names):
            (directory / name).touch()
        self.assertEqual([path.name for path in session_paths(directory)], names)

    def test_readable_session_list_keeps_the_100_newest(self):
        directory = self.directory()
        names = [f"session-2026-10-11_12-00-00.{index:06}-pid-{200-index}-instance-1.jsonl"
                 for index in range(120)]
        for name in names:
            (directory / name).touch()
        self.assertEqual([path.name for path in session_paths(directory)], names[-1:19:-1])

    def test_browser_partial_line_offsets(self):
        path = self.directory() / SESSION_NAME
        first = b'{"type":"session"}\n'
        path.write_bytes(first + b'{"type":"log","text":"pending')
        chunk = read_chunk(path, 0)
        self.assertEqual(chunk["records"], [{"type": "session"}])
        self.assertEqual(chunk["offset"], len(first))
        with path.open("ab") as output:
            output.write(b'"}\n')
        result = read_chunk(path, chunk["offset"])
        self.assertEqual(result["records"], [{"type": "log", "text": "pending"}])
        self.assertEqual(result["offset"], path.stat().st_size)
        path.write_bytes(first)
        self.assertTrue(read_chunk(path, result["offset"])["reset"])

    def test_browser_server_routes(self):
        directory = self.directory()
        path = directory / SESSION_NAME
        path.write_text('{"type":"session","application":"Test","pid":1,"instance":2}\n'
                        '{"type":"closed"}\n', encoding="utf-8")
        server = make_server(directory)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}"
            with urlopen(url + "/") as response:
                self.assertIn(b"Block load", response.read())
            with urlopen(url + "/api/sessions") as response:
                sessions = json.load(response)
                self.assertEqual(sessions[0]["application"], "Test")
            with urlopen(url + f"/api/poll?session={SESSION_NAME}&offset=0") as response:
                self.assertEqual(json.load(response)["records"][-1], {"type": "closed"})
            for invalid in ("../README.md", f"{SESSION_NAME}&offset=-1",
                            f"{SESSION_NAME}&offset=invalid"):
                with self.assertRaises(HTTPError) as error:
                    urlopen(url + "/api/poll?session=" + invalid)
                self.assertEqual(error.exception.code, 400)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    unittest.main()
