#!/usr/bin/env python3
"""Deterministic HTTP server for Haul's network tests (Python standard library only).

Starts on 127.0.0.1 with a random free port and prints "PORT <n>" on the first line.

Content: byte i of a file with seed s is BLOCK_s[i % 65521], where
BLOCK_s[j] = (j * 31 + s) & 0xFF. The C++ tests generate the same bytes to compare.

Endpoints
  /file/<size>      a file of <size> bytes. Query options:
      ranges=0        ignore Range headers (always answer 200 with the full body)
      rate=<B/s>      throttle the body to roughly this many bytes per second
      drop=<n>        close the connection after sending <n> body bytes of this response
      nolength=1      omit Content-Length (connection closes at the end)
      status=<code>   answer with this status code instead of the file
      retryafter=<s>  add Retry-After to an error status
      badrange=1      answer range requests with a Content-Range that is off by one
      name=<n>        send Content-Disposition: attachment; filename="<n>"
      etag=<tag>      ETag to send (default "fixed-<size>")
  /mutable/<size>   like /file, but content and ETag change after GET /bump
  /bump             changes the content of /mutable
  /flaky/<size>?key=K&fail=N   the first N requests for key K return 503
  /redirect?to=<path>          302 to <path> on this server
  /redirect-file               302 to file:///etc/passwd (must be refused by the client)
"""
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

PRIME = 65521
_blocks = {}
_state_lock = threading.Lock()
_mutable_version = 1
_flaky_counts = {}


def block(seed):
    if seed not in _blocks:
        _blocks[seed] = bytes(((j * 31 + seed) & 0xFF) for j in range(PRIME))
    return _blocks[seed]


def content(seed, start, end):
    """Bytes [start, end) of the file with this seed."""
    b = block(seed)
    out = bytearray()
    pos = start
    while pos < end:
        offset = pos % PRIME
        take = min(PRIME - offset, end - pos)
        out += b[offset:offset + take]
        pos += take
    return bytes(out)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "HaulTestServer/1.0"

    def log_message(self, fmt, *args):  # keep test output quiet
        pass

    def send_simple(self, code, body=b"", headers=None):
        self.send_response(code)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        global _mutable_version
        url = urlparse(self.path)
        q = {k: v[-1] for k, v in parse_qs(url.query).items()}
        path = url.path

        if path == "/bump":
            with _state_lock:
                _mutable_version += 1
            return self.send_simple(200, b"ok")

        if path == "/redirect-file":
            return self.send_simple(302, b"", {"Location": "file:///etc/passwd"})

        if path == "/redirect":
            return self.send_simple(302, b"", {"Location": q.get("to", "/file/10")})

        m = re.match(r"^/(file|mutable|flaky)/(\d+)$", path)
        if not m:
            return self.send_simple(404, b"not found")

        kind, size = m.group(1), int(m.group(2))
        seed = 0
        etag = q.get("etag", '"fixed-%d"' % size)
        if kind == "mutable":
            with _state_lock:
                seed = _mutable_version
            etag = '"v%d"' % seed
        if kind == "flaky":
            key = q.get("key", "default")
            with _state_lock:
                _flaky_counts[key] = _flaky_counts.get(key, 0) + 1
                count = _flaky_counts[key]
            if count <= int(q.get("fail", "1")):
                return self.send_simple(503, b"try later", {"Retry-After": "1"})

        if "status" in q:
            headers = {}
            if "retryafter" in q:
                headers["Retry-After"] = q["retryafter"]
            return self.send_simple(int(q["status"]), b"error page", headers)

        start, end = 0, size  # [start, end)
        partial = False
        range_header = self.headers.get("Range")
        if_range = self.headers.get("If-Range")
        honour_ranges = q.get("ranges", "1") != "0"
        if range_header and honour_ranges and (if_range is None or if_range == etag):
            rm = re.match(r"^bytes=(\d+)-(\d*)$", range_header.strip())
            if rm:
                rs = int(rm.group(1))
                re_ = int(rm.group(2)) if rm.group(2) else size - 1
                if rs >= size:
                    return self.send_simple(416, b"", {"Content-Range": "bytes */%d" % size})
                start, end = rs, min(re_, size - 1) + 1
                partial = True

        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("ETag", etag)
        self.send_header("Last-Modified", "Mon, 05 Oct 2026 10:00:00 GMT")
        if honour_ranges:
            self.send_header("Accept-Ranges", "bytes")
        if "name" in q:
            self.send_header("Content-Disposition", 'attachment; filename="%s"' % q["name"])
        if partial:
            shift = 1 if q.get("badrange") == "1" else 0
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start + shift, end - 1 + shift, size))
        if q.get("nolength") == "1":
            self.send_header("Connection", "close")
            self.close_connection = True
        else:
            self.send_header("Content-Length", str(end - start))
        self.end_headers()
        if self.command == "HEAD":
            return

        rate = int(q.get("rate", "0"))
        drop = int(q.get("drop", "-1"))
        chunk = 16384 if rate == 0 else max(1024, rate // 20)
        sent = 0
        pos = start
        try:
            while pos < end:
                n = min(chunk, end - pos)
                if drop >= 0 and sent + n > drop:
                    n = drop - sent
                    if n > 0:
                        self.wfile.write(content(seed, pos, pos + n))
                    self.wfile.flush()
                    self.close_connection = True
                    self.connection.shutdown(2)
                    return
                self.wfile.write(content(seed, pos, pos + n))
                pos += n
                sent += n
                if rate > 0:
                    time.sleep(n / rate)
        except (BrokenPipeError, ConnectionResetError, OSError):
            self.close_connection = True


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    print("PORT %d" % server.server_address[1], flush=True)
    try:
        server.serve_forever(poll_interval=0.1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
