// Drives real Chrome through its DevTools pipe: loads the unpacked extension and checks detection
// on test pages, with the real haul-native-host and a stand-in for Haul's local socket.
// Usage: node tests/browser/e2e.mjs <path to haul-native-host>
import { execFileSync, spawn } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, "../..");
const hostBin = path.resolve(process.argv[2]);
const isWindows = process.platform === "win32";
const work = fs.mkdtempSync(path.join(os.tmpdir(), "haul-e2e-"));
const run = path.join(work, "run");
const profile = path.join(work, "profile");
fs.mkdirSync(run, { recursive: true, mode: 0o700 });

const extId = /kChromeExtensionId = "([a-p]+)"/.exec(fs.readFileSync(path.join(repo, "src/platform/BrowserIntegration.cpp"), "utf8"))[1];
const manifest = JSON.stringify({ name: "io.github.tehsaad.haul", description: "t", path: hostBin, type: "stdio",
                                  allowed_origins: [`chrome-extension://${extId}/`] });
const registryKey = "HKCU\\Software\\Google\\Chrome\\NativeMessagingHosts\\io.github.tehsaad.haul";
if (isWindows) {
  const file = path.join(work, "host.json");
  fs.writeFileSync(file, manifest);
  execFileSync("reg", ["add", registryKey, "/ve", "/d", file, "/f"]);
} else {
  const dir = path.join(profile, "NativeMessagingHosts");
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, "io.github.tehsaad.haul.json"), manifest);
}

const received = [];
const socketPath = isWindows ? `\\\\.\\pipe\\haul-${process.env.USERNAME}` : path.join(run, "haul.sock");
net.createServer((s) => s.on("data", (d) => { received.push(JSON.parse(d.toString())); s.write("ok\n"); })).listen(socketPath);

const server = spawn(process.env.PYTHON || (isWindows ? "python" : "python3"), ["-I", path.join(repo, "tests/server/test_server.py")], { stdio: ["ignore", "pipe", "inherit"] });
const base = await new Promise((resolve) => server.stdout.once("data", (d) => resolve(`http://127.0.0.1:${d.toString().split(" ")[1]}`)));

const chromeBin = process.env.CHROME || (isWindows ? "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe" : "google-chrome");
const chrome = spawn(chromeBin, ["--headless=new", `--user-data-dir=${profile}`, "--no-first-run", ...(process.env.CI && !isWindows ? ["--no-sandbox"] : []),
                                 "--remote-debugging-pipe", "--enable-unsafe-extension-debugging", "about:blank"],
                     { stdio: ["ignore", "ignore", "ignore", "pipe", "pipe"], env: { ...process.env, XDG_RUNTIME_DIR: run } });
const [toChrome, fromChrome] = [chrome.stdio[3], chrome.stdio[4]];
let id = 0;
let buffer = "";
const waiting = new Map();
fromChrome.on("data", (d) => {
  buffer += d.toString();
  let i;
  while ((i = buffer.indexOf("\0")) >= 0) {
    const m = JSON.parse(buffer.slice(0, i));
    buffer = buffer.slice(i + 1);
    if (m.id && waiting.has(m.id)) {
      waiting.get(m.id)(m);
      waiting.delete(m.id);
    }
  }
});
const send = (method, params = {}, sessionId) => new Promise((res, rej) => {
  const n = ++id;
  waiting.set(n, (m) => (m.error ? rej(new Error(`${method}: ${m.error.message}`)) : res(m.result)));
  toChrome.write(`${JSON.stringify({ id: n, method, params, sessionId })}\0`);
});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (name, ok, extra = "") => {
  console.log(`${ok ? "PASS" : "FAIL"}  ${name} ${ok ? "" : extra}`);
  if (!ok) failed++;
};

async function openPage(url) {
  const { targetId } = await send("Target.createTarget", { url });
  const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
  const js = async (expression) => (await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }, sessionId)).result.value;
  js.sessionId = sessionId;
  return js;
}
const until = async (read, test, tries = 60) => {
  let value;
  for (let i = 0; i < tries; i++) {
    value = await read();
    if (test(value)) return value;
    await sleep(250);
  }
  return value;
};
const shadow = `document.getElementById("haul-panel-host")?.shadowRoot`;
const names = `[...(${shadow}?.querySelectorAll(".name") ?? [])].map(n => n.textContent)`;
const noteOf = (name) => `[...${shadow}.querySelectorAll(".row")].find(r => r.querySelector(".name").textContent === ${JSON.stringify(name)})?.querySelector(".note").textContent`;
const metaOf = (name) => `[...${shadow}.querySelectorAll(".row")].find(r => r.querySelector(".name").textContent === ${JSON.stringify(name)})?.querySelector(".meta").textContent`;

try {
  const loaded = await send("Extensions.loadUnpacked", { path: path.join(repo, "browser") });
  check("extension loads, id matches the host", loaded.id === extId, loaded.id);
  await sleep(1500);

  // ------------------------------------------------ one video, downloads taken over
  let js = await openPage(`${base}/page`);
  await until(() => js(names), (n) => n && n.includes("clip.mp4"));
  check("a page's video is found without any click", (await js(names)).includes("clip.mp4"));
  check("the indicator shows and the panel stays closed", (await js(`${shadow}.querySelector(".chip").textContent`)) === "Haul: 1"
        && (await js(`${shadow}.querySelector(".panel").hidden`)) === true);
  await js(`${shadow}.querySelector(".chip").click()`);
  check("clicking the indicator opens the panel", (await js(`${shadow}.querySelector(".panel").hidden`)) === false);
  await js(`${shadow}.querySelector(".get").click()`);
  await until(() => js(`${shadow}.querySelector(".get").textContent`), (t) => t === "Sent");
  const fromPanel = received.find((m) => (m.url || "").includes("clip.mp4"));
  check("Download reaches Haul, which asks first", !!fromPanel && fromPanel.confirm === true, JSON.stringify(fromPanel));
  await js(`document.getElementById("dl").click()`);
  await sleep(3000);
  const captured = received.find((m) => (m.url || "").includes("thing.zip"));
  check("a browser download is taken over", !!captured && captured.confirm === true, JSON.stringify(captured));
  check("only the site's address is sent as referrer", !!captured && captured.referrer === `${new URL(base).origin}/`, captured?.referrer);

  // ------------------------------------------------ many files on one page
  js = await openPage(`${base}/mediapage`);
  await until(() => js(names), (n) => n && n.includes("playerfile.mp4") && n.includes("song.mp3"));
  let list = await js(names);
  check("a playing audio file is found", list.includes("song.mp3"), list.join());
  check("files linked from the page are found", list.includes("album.flac") && list.includes("talk.wma") && list.includes("huge.mkv"), list.join());
  check("a file a player fetches in the background is found, extensionless", list.includes("playerfile.mp4"), list.join());
  check("the same file under a signed link appears once", list.filter((n) => n === "song.mp3").length === 1, list.join());
  check("stream pieces fetched by script are ignored", !list.includes("seg-12.mp4") && !list.includes("piece.ts"), list.join());
  check("a .mp4 address answering with HTML is ignored", !list.includes("notes.mp4"), list.join());
  check("the player's file shows its whole size from a range response", /2\.86 MB/.test(await js(metaOf("playerfile.mp4"))), await js(metaOf("playerfile.mp4")));

  await js(`${shadow}.querySelector(".chip").click()`);
  await until(() => js(noteOf("album.flac")), (t) => t && t.startsWith("Checked"));
  check("opening the panel checks real bytes", /Checked: a real FLAC file\. Can resume\./.test(await js(noteOf("album.flac"))), await js(noteOf("album.flac")));
  check("the real size replaces an unknown one", /FLAC.*391 KB/.test(await js(metaOf("album.flac"))), await js(metaOf("album.flac")));
  await until(() => js(names), (n) => n && !n.includes("fake.mp4"));
  list = await js(names);
  check("a fake .mp4 whose bytes are a web page is removed", !list.includes("fake.mp4"), list.join());
  await until(() => js(noteOf("huge.mkv")), (t) => t && t.startsWith("Checked"));
  check("a large file reports its exact size without downloading it", /5\.59 GB/.test(await js(metaOf("huge.mkv"))), await js(metaOf("huge.mkv")));
  await sleep(1500);
  list = await js(names);
  check("a link that redirects to a listed file doesn't stay as a duplicate", list.filter((n) => /album\.flac|^redirect/.test(n)).length === 1, list.join());
  check("a player's redirected file is listed where it lands", list.includes("moved.mp3"), list.join());
  if (process.env.HAUL_E2E_SHOTS) {
    const shot = await send("Page.captureScreenshot", { format: "png" }, js.sessionId);
    fs.writeFileSync(path.join(process.env.HAUL_E2E_SHOTS, "panel.png"), Buffer.from(shot.data, "base64"));
  }
  const chipCount = await js(`${shadow}.querySelector(".chip").textContent`);
  check("the indicator counts what is listed", chipCount === `Haul: ${list.length}`, `${chipCount} vs ${list.length}`);

  // ------------------------------------------------ streams
  js = await openPage(`${base}/streampage`);
  const labels = await until(() => js(`[...(${shadow}?.querySelectorAll("option") ?? [])].map(o => o.textContent)`), (l) => l && l.length >= 2);
  check("an HLS stream appears with its qualities", labels.join() === "720p,360p", labels.join());
  check("it uses the page title", (await js(`${shadow}.querySelector(".name").textContent`)) === "Stream lesson.mp4");
  check("estimates are marked", /≈/.test(await js(`${shadow}.querySelector(".meta").textContent`)), await js(`${shadow}.querySelector(".meta").textContent`));
  check("resolution comes from the manifest", /1280×720/.test(await js(`${shadow}.querySelector(".meta").textContent`)));
  await js(`fetch("/hls/high.m3u8").then(r=>r.text())`);
  await js(`fetch("/hls/seg/2000.ts?size=3000000").then(r=>r.arrayBuffer())`);
  await sleep(1500);
  check("child playlists and pieces aren't listed", (await js(`${shadow}.querySelectorAll(".row").length`)) === 1);
  await js(`(() => { const s = ${shadow}.querySelector("select"); s.value = "360p-800000"; s.dispatchEvent(new Event("change")); ${shadow}.querySelector(".get").click(); })()`);
  await sleep(2500);
  const fromStream = received.find((m) => (m.url || "").includes("master.m3u8"));
  check("the chosen quality reaches Haul", !!fromStream && fromStream.stream === "hls" && fromStream.variant === "360p-800000", JSON.stringify(fromStream));

  js = await openPage(`${base}/streampage`);
  await sleep(1000);
  await js(`fetch("/hls/high.m3u8?enc=key").then(r=>r.text())`);
  const lockedText = await until(() => js(`${shadow}?.textContent ?? ""`), (t) => t.includes("Protected"), 40);
  check("an encrypted stream is named, with nothing to press", lockedText.includes("Protected video"));

  // Optional look at real sites: HAUL_E2E_SITES="https://a,https://b". Prints what was found.
  for (const site of (process.env.HAUL_E2E_SITES || "").split(",").filter(Boolean)) {
    const page = await openPage(site);
    await sleep(9000);
    await page(`document.querySelectorAll("video, audio").forEach(m => { m.muted = true; m.play().catch(() => {}); })`);
    await sleep(6000);
    await page(`${shadow}?.querySelector(".chip")?.click()`);
    await sleep(5000);
    const rows = await page(`[...(${shadow}?.querySelectorAll(".row") ?? [])].map(r => r.innerText.replace(/\n+/g, " | "))`);
    console.log(`SITE ${site}: ${(rows || []).length} found`);
    for (const row of rows || []) console.log(`  ${row}`);
  }
} catch (e) {
  check("run", false, e.message);
}
chrome.kill();
server.kill();
if (isWindows) {
  try { execFileSync("reg", ["delete", registryKey, "/f"]); } catch {}
}
console.log(failed ? `${failed} check(s) failed` : "All checks passed");
setTimeout(() => process.exit(failed ? 1 : 0), 300);
