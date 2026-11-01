#!/usr/bin/env python3
"""Deterministic HTTP server for Haul's network tests (Python standard library only).

Starts on 127.0.0.1 with a random free port and prints "PORT <http> <ftp>" on the first line.

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
  /auth/<size>      needs HTTP Basic login test:secret
  /cookie/<size>    needs the cookie session=abc
  /m/<name>         a media file with real signature bytes (fmt=, size=, ct=, cd=, body=html, chunked=1)
  /mediapage        one page with several media files, duplicates and fakes
  /redirect-file               302 to file:///etc/passwd (must be refused by the client)
"""
import os
import re
import socket
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

PRIME = 65521

# Real signatures, followed by padding, for every supported format.
MEDIA_FIXTURES = {
    "mp4": ("video/mp4", b"\x00\x00\x00\x18ftypisom\x00\x00\x02\x00isom"),
    "m4v": ("video/x-m4v", b"\x00\x00\x00\x18ftypM4V \x00\x00\x02\x00M4V "),
    "mov": ("video/quicktime", b"\x00\x00\x00\x14ftypqt  \x00\x00\x02\x00qt  "),
    "3gp": ("video/3gpp", b"\x00\x00\x00\x18ftyp3gp4\x00\x00\x02\x003gp4"),
    "mkv": ("video/x-matroska", b"\x1a\x45\xdf\xa3\x01\x00\x00\x00\x00\x00\x00\x1fmatroska"),
    "webm": ("video/webm", b"\x1a\x45\xdf\xa3\x01\x00\x00\x00\x00\x00\x00\x1fwebm"),
    "avi": ("video/x-msvideo", b"RIFF\x00\x00\x00\x00AVI LIST"),
    "flv": ("video/x-flv", b"FLV\x01\x05\x00\x00\x00\x09"),
    "ts": ("video/mp2t", (b"\x47" + b"\x00" * 187) * 8),
    "mpeg": ("video/mpeg", b"\x00\x00\x01\xba\x44\x00\x04"),
    "mpg": ("video/mpeg", b"\x00\x00\x01\xb3\x14\x00\xf0"),
    "mp3": ("audio/mpeg", b"ID3\x04\x00\x00\x00\x00\x00\x00"),
    "wav": ("audio/wav", b"RIFF\x00\x00\x00\x00WAVEfmt "),
    "flac": ("audio/flac", b"fLaC\x00\x00\x00\x22"),
    "aac": ("audio/aac", b"\xff\xf1\x50\x80\x00\x1f\xfc"),
    "m4a": ("audio/mp4", b"\x00\x00\x00\x18ftypM4A \x00\x00\x02\x00M4A "),
    "ogg": ("audio/ogg", b"OggS\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x01\x1e\x01vorbis"),
    "opus": ("audio/opus", b"OggS\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00OpusHead"),
    "wma": ("audio/x-ms-wma", bytes.fromhex("3026B2758E66CF11A6D900AA0062CE6C")),
}
_blocks = {}
_state_lock = threading.Lock()
_mutable_version = 1
_flaky_counts = {}
STATIC_DIR = os.environ.get("HAUL_STATIC_DIR", "")


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
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                self.close_connection = True

    def serve_media(self, path, q):
        """A file with a real signature and any size; options set what its headers say."""
        if q.get("deny") == "1":
            return self.send_simple(403, b"denied")
        name = path[3:]
        extension = name.rsplit(".", 1)[-1].lower() if "." in name else ""
        fmt = q.get("fmt", extension)
        mime, magic = MEDIA_FIXTURES.get(fmt, ("application/octet-stream", b""))
        if q.get("body") == "html":
            magic = b"<!doctype html><html><body>Please log in</body></html>"
        size = int(q.get("size", "2000000"))
        mime = q.get("ct", mime)

        start, end = 0, size
        partial = False
        rm = re.match(r"^bytes=(\d+)-(\d*)$", (self.headers.get("Range") or "").strip())
        if rm and q.get("ranges", "1") != "0" and int(rm.group(1)) < size:
            start = int(rm.group(1))
            end = min(int(rm.group(2)) + 1 if rm.group(2) else size, size)
            partial = True

        self.send_response(206 if partial else 200)
        if mime:
            self.send_header("Content-Type", mime)
        if "cd" in q:
            self.send_header("Content-Disposition", q["cd"])
        if partial:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end - 1, size))
        if q.get("chunked") == "1":
            self.send_header("Connection", "close")
            self.close_connection = True
        else:
            self.send_header("Content-Length", str(end - start))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        if self.command == "HEAD":
            return
        pos = start
        padding = b"\0" * 65536
        try:
            while pos < end:
                if pos < len(magic):
                    data = magic[pos:min(len(magic), end)]
                else:
                    data = padding[:min(len(padding), end - pos)]
                self.wfile.write(data)
                pos += len(data)
        except (BrokenPipeError, ConnectionResetError, OSError):
            self.close_connection = True

    def serve_stream(self, path, q, query):
        """Synthetic streams. Options travel in the query string so every fixture is deterministic."""
        count = int(q.get("count", "6"))
        size = int(q.get("size", "20000"))
        suffix = ("?" + query) if query else ""
        hls = {"Content-Type": "application/vnd.apple.mpegurl"}
        dash = {"Content-Type": "application/dash+xml"}

        if path == "/streampage":
            html = ('<!doctype html><title>Stream lesson</title><script>'
                    'fetch("/hls/master.m3u8").then(r=>r.text());</script>').encode()
            return self.send_simple(200, html, {"Content-Type": "text/html"})

        if path == "/hls/master.m3u8":
            lines = ["#EXTM3U"]
            if q.get("enc") == "session":
                lines.append('#EXT-X-SESSION-KEY:METHOD=AES-128,URI="k"')
            if q.get("audio") == "1":
                lines.append('#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="en",DEFAULT=YES,URI="audio.m3u8%s"' % suffix)
            group = ',AUDIO="a"' if q.get("audio") == "1" else ""
            lines += ['#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS="avc1.4d401e,mp4a.40.2"%s' % group,
                      "low.m3u8" + suffix,
                      '#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720,CODECS="avc1.4d401f,mp4a.40.2"%s' % group,
                      "high.m3u8" + suffix,
                      '#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=100000,URI="iframe.m3u8"']
            return self.send_simple(200, "\n".join(lines).encode(), hls)

        m = re.match(r"^/hls/(low|high|audio|single)\.m3u8$", path)
        if m:
            base = {"low": 1000, "high": 2000, "audio": 3000, "single": 4000}[m.group(1)]
            lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2"]
            if q.get("enc") == "key":
                lines.append('#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/key"')
            if q.get("fmp4") == "1":
                lines.append('#EXT-X-MAP:URI="seg/%d.bin?size=100"' % (base + 999))
            for i in range(count):
                if q.get("enc") == "late" and i == count // 2:
                    lines.append('#EXT-X-KEY:METHOD=SAMPLE-AES,URI="k"')
                lines += ["#EXTINF:2.000,", "seg/%d.ts%s" % (base + i, suffix)]
            if q.get("live") != "1":
                lines.append("#EXT-X-ENDLIST")
            return self.send_simple(200, "\n".join(lines).encode(), hls)

        m = re.match(r"^/(?:hls|dash)/seg/(?:[a-z0-9]+/)?(\d+)\.[a-z0-9]+$", path)
        if m:
            n = int(m.group(1))
            index = n % 1000
            if str(index) == q.get("deny"):
                return self.send_simple(403, b"denied")
            if str(index) == q.get("html"):
                return self.send_simple(200, b"<html>log in</html>", {"Content-Type": "text/html"})
            if str(index) == q.get("flaky"):
                key = "seg%s-%d" % (q.get("key", ""), n)
                with _state_lock:
                    _flaky_counts[key] = _flaky_counts.get(key, 0) + 1
                    first = _flaky_counts[key] == 1
                if first:
                    return self.send_simple(503, b"busy")
            if q.get("slow"):
                time.sleep(float(q["slow"]))
            body = content(n, 0, size)
            if q.get("encv") == "1" and index == 999:
                body = b"....encv...." + body
            return self.send_simple(200, body, {"Content-Type": "video/mp2t"})

        if path == "/dash/manifest.mpd":
            protection = '<ContentProtection schemeIdUri="urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed"/>' if q.get("enc") == "1" else ""
            kind = 'dynamic' if q.get("live") == "1" else 'static'
            periods = 2 if q.get("periods") == "2" else 1
            video_timeline = ""
            if q.get("timeline") == "1":
                video_timeline = '<SegmentTimeline><S t="0" d="2000" r="%d"/></SegmentTimeline>' % (count - 1)
                timing = ''
            else:
                timing = 'duration="2000"'
            body = ('<?xml version="1.0"?><MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="%s" '
                    'mediaPresentationDuration="PT%dS">' % (kind, count * 2))
            for _ in range(periods):
                body += ('<Period><AdaptationSet contentType="video" mimeType="video/mp4">%s'
                         '<SegmentTemplate timescale="1000" %s startNumber="0" initialization="seg/$RepresentationID$/999.bin%s" '
                         'media="seg/$RepresentationID$/$Number%%03d$.m4s%s">%s</SegmentTemplate>'
                         '<Representation id="v360" bandwidth="800000" width="640" height="360" codecs="avc1.4d401e"/>'
                         '<Representation id="v720" bandwidth="2500000" width="1280" height="720" codecs="avc1.4d401f"/>'
                         '</AdaptationSet>' % (protection, timing, suffix.replace("&", "&amp;"), suffix.replace("&", "&amp;"), video_timeline))
                body += ('<AdaptationSet contentType="audio" mimeType="audio/mp4">%s'
                         '<SegmentTemplate timescale="1000" %s startNumber="0" initialization="seg/$RepresentationID$/999.bin" '
                         'media="seg/$RepresentationID$/$Number%%03d$.m4s">%s</SegmentTemplate>'
                         '<Representation id="a128" bandwidth="128000" codecs="mp4a.40.2"/></AdaptationSet></Period>'
                         % (protection, timing, video_timeline))
            body += "</MPD>"
            return self.send_simple(200, body.encode(), dash)

        if path == "/hls/garbage.m3u8":
            return self.send_simple(200, b"<html>not a playlist</html>", {"Content-Type": "text/html"})
        return self.send_simple(404, b"not found")

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

        if path == "/page":
            html = (b'<!doctype html><title>t</title><video src="/media/clip.mp4"></video>'
                    b'<a id="dl" href="/file/5000?name=thing.zip" download>get</a>')
            return self.send_simple(200, html, {"Content-Type": "text/html"})

        if path.startswith("/media/"):
            body = content(0, 0, 3000000)
            return self.send_simple(200, body, {"Content-Type": "video/mp4"})

        if path.startswith("/hls/") or path.startswith("/dash/") or path.startswith("/streampage"):
            return self.serve_stream(path, q, url.query)

        if path.startswith("/static/") and STATIC_DIR:
            target = os.path.realpath(os.path.join(STATIC_DIR, path[len("/static/"):]))
            if not target.startswith(os.path.realpath(STATIC_DIR)) or not os.path.isfile(target):
                return self.send_simple(404, b"not found")
            with open(target, "rb") as f:
                return self.send_simple(200, f.read())

        if path == "/mediapage":
            html = """<!doctype html><title>Media page</title>
<audio src="/m/song.mp3?size=300000"></audio>
<audio src="/redirect?to=/m/moved.mp3%3Fsize%3D200000"></audio>
<a id="a1" href="/m/album.flac?size=400000">flac</a>
<a id="a2" href="/m/song.mp3?size=300000&token=abc123">the same song, signed</a>
<a id="a3" href="/m/talk.wma?size=500000">wma</a>
<a id="a4" href="/m/fake.mp4?ct=application/octet-stream&body=html&size=300000">a page pretending to be a video</a>
<a id="a5" href="/m/huge.mkv?size=6000000000">large</a>
<a id="a6" href="/redirect?to=/m/album.flac?size=400000">redirects to the flac</a>
<script>
fetch("/m/playerfile?fmt=mp4&size=3000000", {headers: {Range: "bytes=0-1023"}});
fetch("/m/seg-12.mp4?size=3000000");
fetch("/m/piece.ts?size=3000000");
fetch("/m/notes.mp4?ct=text/html&size=3000000");
</script>"""
            return self.send_simple(200, html.encode(), {"Content-Type": "text/html"})

        if path.startswith("/m/"):
            return self.serve_media(path, q)

        if path == "/redirect-file":
            return self.send_simple(302, b"", {"Location": "file:///etc/passwd"})

        if path == "/redirect":
            return self.send_simple(302, b"", {"Location": q.get("to", "/file/10")})

        m = re.match(r"^/(file|mutable|flaky|auth|cookie|referer)/(\d+)$", path)
        if not m:
            return self.send_simple(404, b"not found")

        kind, size = m.group(1), int(m.group(2))
        seed = 0
        etag = q.get("etag", '"fixed-%d"' % size)
        if kind == "mutable":
            with _state_lock:
                seed = _mutable_version
            etag = '"v%d"' % seed
        if kind == "auth" and self.headers.get("Authorization") != "Basic dGVzdDpzZWNyZXQ=":  # test:secret
            return self.send_simple(401, b"login", {"WWW-Authenticate": 'Basic realm="haul"'})
        if kind == "cookie" and "session=abc" not in self.headers.get("Cookie", ""):
            return self.send_simple(403, b"no cookie")
        if kind == "referer" and "example.org" not in self.headers.get("Referer", ""):
            return self.send_simple(403, b"no referer")
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


class FtpHandler(socketserver.StreamRequestHandler):
    """Just enough FTP for libcurl: login test/secret or anonymous, SIZE, MDTM, REST and RETR /file/<n>."""

    def say(self, line):
        self.wfile.write((line + "\r\n").encode())
        self.wfile.flush()

    def handle(self):
        self.say("220 ready")
        user, logged_in, rest, passive = "", False, 0, None
        while True:
            raw = self.rfile.readline()
            if not raw:
                return
            cmd, _, arg = raw.decode("latin-1").strip().partition(" ")
            cmd = cmd.upper()
            if cmd == "USER":
                user = arg
                self.say("331 password")
            elif cmd == "PASS":
                logged_in = user == "anonymous" or (user == "test" and arg == "secret")
                self.say("230 ok" if logged_in else "530 wrong login")
            elif not logged_in:
                self.say("530 log in first")
            elif cmd == "PWD":
                self.say('257 "/"')
            elif cmd in ("TYPE", "CWD"):
                self.say("200 ok" if cmd == "TYPE" else "250 ok")
            elif cmd in ("SIZE", "MDTM", "RETR"):
                m = re.match(r"^/?(?:file/)?(\d+)$", arg)
                if not m:
                    self.say("550 not found")
                elif cmd == "SIZE":
                    self.say("213 %s" % m.group(1))
                elif cmd == "MDTM":
                    self.say("213 20261005100000")
                else:
                    self.say("150 sending")
                    data, _ = passive.accept()
                    data.sendall(content(0, rest, int(m.group(1))))
                    data.close()
                    passive.close()
                    rest = 0
                    self.say("226 done")
            elif cmd == "REST":
                rest = int(arg)
                self.say("350 ok")
            elif cmd == "EPSV":
                passive = socket.socket()
                passive.bind(("127.0.0.1", 0))
                passive.listen(1)
                self.say("229 passive (|||%d|)" % passive.getsockname()[1])
            elif cmd == "QUIT":
                self.say("221 bye")
                return
            else:
                self.say("502 not implemented")


class FtpServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ftp = FtpServer(("127.0.0.1", 0), FtpHandler)
    threading.Thread(target=ftp.serve_forever, daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    print("PORT %d %d" % (server.server_address[1], ftp.server_address[1]), flush=True)
    try:
        server.serve_forever(poll_interval=0.1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
