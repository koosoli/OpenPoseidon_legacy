"""Loopback-only interrupted-download fixture; never contacts an external service."""
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--port-file", required=True)
parser.add_argument("--log", required=True)
args = parser.parse_args()
body = b"fixture-download-resumed-after-disconnect\n"
counts = {}

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args):
        pass
    def do_GET(self):
        route = self.path.strip("/")
        if route not in ("resume", "no-range", "changed", "repeated"):
            self.send_error(404)
            return
        n = counts.get(route, 0) + 1
        counts[route] = n
        start = int(self.headers.get("Range", "bytes=0-").split("=")[1].split("-")[0])
        requested = start
        validator = '"v2"' if route == "changed" and n > 1 else '"v1"'
        if route == "no-range" or (self.headers.get("If-Range") and self.headers["If-Range"] != validator):
            start = 0
        with open(args.log, "a", encoding="utf-8") as log:
            log.write(json.dumps(dict(route=route, request=n, range=requested, served=start)) + "\n")
        payload = body[start:]
        self.send_response(206 if start else 200)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("ETag", validator)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Connection", "close")
        if start:
            self.send_header("Content-Range", f"bytes {start}-{len(body)-1}/{len(body)}")
        self.end_headers()
        fail = n <= (5 if route == "repeated" else 1)
        self.wfile.write(payload[:5 if route == "repeated" else 12] if fail else payload)
        self.wfile.flush()
        self.close_connection = True

server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
Path(args.port_file).write_text(str(server.server_port))
server.serve_forever()
