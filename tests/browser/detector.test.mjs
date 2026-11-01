// Tests for browser/detector.js. Run with: node --test tests/browser/detector.test.mjs
import test from "node:test";
import assert from "node:assert/strict";

await import("../../browser/detector.js");
const { classifyResponse, mediaKey, isMediaLink, fileNameFor } = globalThis.HaulDetector;

function respond(url, { status = 200, type = "media", ct = "", length = 5000000, cd = "", range = "" } = {}) {
  const responseHeaders = [];
  if (ct) responseHeaders.push({ name: "Content-Type", value: ct });
  if (length) responseHeaders.push({ name: "Content-Length", value: String(length) });
  if (cd) responseHeaders.push({ name: "Content-Disposition", value: cd });
  if (range) responseHeaders.push({ name: "Content-Range", value: range });
  return classifyResponse({ url, statusCode: status, type, responseHeaders });
}

const VIDEO = { mp4: "video/mp4", mkv: "video/x-matroska", webm: "video/webm", mov: "video/quicktime", avi: "video/x-msvideo",
                flv: "video/x-flv", ts: "video/mp2t", m4v: "video/x-m4v", mpeg: "video/mpeg", "3gp": "video/3gpp" };
const AUDIO = { mp3: "audio/mpeg", wav: "audio/wav", flac: "audio/flac", aac: "audio/aac", m4a: "audio/mp4",
                ogg: "audio/ogg", opus: "audio/opus", wma: "audio/x-ms-wma" };

test("every format is recognised from its Content-Type alone", () => {
  for (const [format, ct] of Object.entries({ ...VIDEO, ...AUDIO })) {
    const found = respond("https://e.com/get", { ct });
    assert.equal(found.format, format, ct);
    assert.equal(found.kind, format in VIDEO ? "video" : "audio", ct);
    assert.equal(found.name, `get.${format}`, "an extensionless name gets the right extension");
  }
});

test("every format is recognised from its extension with a generic type", () => {
  for (const format of [...Object.keys(VIDEO), ...Object.keys(AUDIO), "mpg"]) {
    if (format === "ts") continue; // checked below
    for (const ct of ["application/octet-stream", "binary/octet-stream", ""]) {
      const found = respond(`https://e.com/file.${format}?x=1`, { ct, type: "other" });
      assert.equal(found && found.format, format, `${format} as "${ct}"`);
    }
  }
});

test("Content-Disposition names media behind any address", () => {
  const found = respond("https://e.com/download.php?id=7", { ct: "application/octet-stream", type: "main_frame", cd: 'attachment; filename="talk.mp3"' });
  assert.equal(found.format, "mp3");
  assert.equal(found.name, "talk.mp3");
  const encoded = respond("https://e.com/d", { ct: "application/force-download", type: "other", cd: "attachment; filename*=UTF-8''my%20song.flac" });
  assert.equal(encoded.name, "my song.flac");
});

test("a player's generic response counts as media; anyone else's does not", () => {
  assert.equal(respond("https://e.com/stream?id=9", { ct: "application/octet-stream", type: "media" }).kind, "video");
  assert.equal(respond("https://e.com/stream?id=9", { ct: "application/octet-stream", type: "xmlhttprequest" }), null);
  assert.equal(respond("https://e.com/stream?id=9", { ct: "application/octet-stream", type: "other" }), null);
});

test("a media-looking URL with a non-media response is rejected", () => {
  for (const ct of ["text/html", "text/html; charset=utf-8", "application/json", "image/png", "text/plain", "application/zip"]) {
    assert.equal(respond("https://e.com/movie.mp4", { ct }), null, ct);
    assert.equal(respond("https://e.com/song.mp3", { ct, type: "other" }), null, ct);
  }
});

test("only successful responses count, so a redirect reports only where it lands", () => {
  for (const status of [301, 302, 304, 401, 403, 404, 500])
    assert.equal(respond("https://e.com/a.mp4", { ct: "video/mp4", status }), null, String(status));
  assert.ok(respond("https://e.com/a.mp4", { ct: "video/mp4", status: 206, range: "bytes 0-999/5000000" }));
});

test("ranged responses report the size of the whole file", () => {
  const found = respond("https://e.com/a.mp4", { ct: "video/mp4", status: 206, length: 1024, range: "bytes 0-1023/734003200" });
  assert.equal(found.size, 734003200);
});

test("chunked responses with no length still count when a player asks", () => {
  const found = respond("https://e.com/a.mp4", { ct: "video/mp4", length: 0 });
  assert.equal(found.size, 0);
  assert.equal(respond("https://e.com/a.mp4", { ct: "video/mp4", length: 0, type: "xmlhttprequest" }), null);
});

test("large sizes are kept exactly", () => {
  assert.equal(respond("https://e.com/a.mkv", { ct: "video/x-matroska", length: 6000000000 }).size, 6000000000);
});

test("background requests: whole files yes, stream pieces no", () => {
  const type = "xmlhttprequest";
  assert.ok(respond("https://e.com/lecture.mp4", { ct: "video/mp4", type }));
  assert.ok(respond("https://e.com/lecture", { ct: "video/mp4", type, status: 206, length: 2000, range: "bytes 0-1999/90000000" }));
  assert.equal(respond("https://e.com/seg-12.mp4", { ct: "video/mp4", type }), null);
  assert.equal(respond("https://e.com/v/chunk_004.mp4", { ct: "video/mp4", type }), null);
  assert.equal(respond("https://e.com/a.ts", { ct: "video/mp2t", type }), null);
  assert.equal(respond("https://e.com/v.mp4?range=0-1000", { ct: "video/mp4", type }), null);
  assert.equal(respond("https://e.com/small.mp4", { ct: "video/mp4", type, length: 200000 }), null);
  assert.equal(respond("https://e.com/x.m4s", { ct: "video/mp4", type: "media" }), null);
});

test("a .ts file opened directly is a file; fetched by script it is a piece", () => {
  assert.equal(respond("https://e.com/movie.ts", { ct: "video/mp2t", type: "main_frame" }).format, "ts");
  assert.equal(respond("https://e.com/movie.ts", { ct: "video/mp2t", type: "xmlhttprequest" }), null);
});

test("tiny interface sounds are ignored", () => {
  assert.equal(respond("https://e.com/click.mp3", { ct: "audio/mpeg", length: 8000 }), null);
});

test("manifests are recognised by type and by name", () => {
  assert.equal(respond("https://e.com/x", { ct: "application/vnd.apple.mpegurl", type: "xmlhttprequest", length: 300 }).stream, true);
  assert.equal(respond("https://e.com/x", { ct: "application/dash+xml", type: "xmlhttprequest", length: 300 }).stream, true);
  assert.equal(respond("https://e.com/x", { ct: "audio/mpegurl", type: "xmlhttprequest", length: 300 }).stream, true);
  assert.equal(respond("https://e.com/a.m3u8", { ct: "application/octet-stream", type: "xmlhttprequest", length: 300 }).stream, true);
  assert.equal(respond("https://e.com/a.mpd", { ct: "", type: "xmlhttprequest", length: 300 }).stream, true);
  assert.equal(respond("https://e.com/a.m3u8", { ct: "text/html", type: "xmlhttprequest" }), null);
});

test("YouTube's hosts are left alone, whatever the response looks like", () => {
  for (const host of ["www.youtube.com", "r3---sn-abc.googlevideo.com", "i.ytimg.com", "www.youtube-nocookie.com"]) {
    assert.equal(respond(`https://${host}/videoplayback`, { ct: "video/mp4" }), null, host);
    assert.equal(respond(`https://${host}/a.m3u8`, { ct: "application/x-mpegurl" }), null, host);
  }
});

test("signed links to the same file share a key; different files do not", () => {
  const key = mediaKey("https://cdn.e.com/a.mp4?token=abc&expires=1&id=7");
  assert.equal(mediaKey("https://cdn.e.com/a.mp4?id=7&token=xyz&expires=2"), key);
  assert.equal(mediaKey("https://cdn.e.com/a.mp4?X-Amz-Signature=1&X-Amz-Date=2&id=7"), key);
  assert.notEqual(mediaKey("https://cdn.e.com/a.mp4?id=8"), key);
  assert.notEqual(mediaKey("https://cdn.e.com/b.mp4?id=7"), key);
});

test("links are matched on the path, not anywhere in the address", () => {
  assert.equal(isMediaLink("https://e.com/song.mp3?x=1"), true);
  assert.equal(isMediaLink("https://e.com/page.html?file=song.mp3"), false);
  assert.equal(isMediaLink("https://e.com/song.mp3.html"), false);
  assert.equal(isMediaLink("not a url"), false);
});

test("file names only gain an extension when they need one", () => {
  assert.equal(fileNameFor("stream", "mp4", "e.com"), "stream.mp4");
  assert.equal(fileNameFor("download.php", "mp3", "e.com"), "download.php.mp3");
  assert.equal(fileNameFor("song.mp3", "mp3", "e.com"), "song.mp3");
  assert.equal(fileNameFor("", "webm", "e.com"), "e.com.webm");
  assert.equal(fileNameFor("thing", "", "e.com"), "thing");
});
