"""Only synthetic loopback data; no public service or external asset requests."""
import collections
import http.server
import pathlib
import subprocess
import sys
import tempfile
import threading
import time

requests = collections.Counter()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        requests[self.path] += 1
        lon = int(pathlib.PurePosixPath(self.path).stem.split("_")[1])
        if lon == 4:
            time.sleep(2.5)
        if lon == 3:
            self.send_response(302)
            self.send_header("Location", "/redirect-must-not-be-followed")
            self.end_headers()
            return
        body = {1: b"not json", 2: b" " * (4 * 1024 * 1024 + 1),
                5: b'{"elements":[],"remark":"truncated"}'}.get(lon, b'{"elements":[]}')
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix="op-earth-content-") as cache:
        subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}", cache],
                       check=True, timeout=30)
    assert requests["/features/0_0.json"] == 2, requests
    for lon in (1, 2, 3, 4, 5, 6):
        assert requests[f"/features/0_{lon}.json"] == 1, requests
    assert requests["/buildings/0_8.json"] == 1, requests
    assert requests["/features/0_7.json"] == 0, requests
    assert requests["/redirect-must-not-be-followed"] == 0, requests
    print("Synthetic HTTP server request-budget checks PASS")
finally:
    server.shutdown()
    server.server_close()
