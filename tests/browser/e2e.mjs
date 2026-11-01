// Usage: ID_FILE=<file holding the extension id> node e2e.mjs <work dir> <browser dir> <haul-native-host> <test server URL>
// Drives real Chrome through its DevTools pipe: loads the unpacked extension, opens a test page,
// and records what reaches a stand-in for Haul's local socket through the real native host.
import { spawn } from "node:child_process";
import net from "node:net";
import fs from "node:fs";
import path from "node:path";
const [work, extension, hostBin, base] = process.argv.slice(2);
const run = path.join(work, "run"), profile = path.join(work, "profile");
fs.mkdirSync(run, { recursive: true, mode: 0o700 });
const nmDir = path.join(profile, "NativeMessagingHosts");
fs.mkdirSync(nmDir, { recursive: true });
const extId = fs.readFileSync(process.env.ID_FILE, "utf8").trim();
fs.writeFileSync(path.join(nmDir, "io.github.tehsaad.haul.json"), JSON.stringify({
  name: "io.github.tehsaad.haul", description: "t", path: hostBin, type: "stdio",
  allowed_origins: [`chrome-extension://${extId}/`] }));
const received = [];
net.createServer((s) => s.on("data", (d) => { received.push(JSON.parse(d.toString())); s.write("ok\n"); }))
   .listen(path.join(run, "haul.sock"));
const chrome = spawn("google-chrome", ["--headless=new", `--user-data-dir=${profile}`, "--no-first-run",
  "--remote-debugging-pipe", "--enable-unsafe-extension-debugging", "about:blank"],
  { stdio: ["ignore", "ignore", "ignore", "pipe", "pipe"], env: { ...process.env, XDG_RUNTIME_DIR: run } });
const [toChrome, fromChrome] = [chrome.stdio[3], chrome.stdio[4]];
let id = 0, buf = "";
const waiting = new Map();
fromChrome.on("data", (d) => {
  buf += d.toString();
  let i;
  while ((i = buf.indexOf("\0")) >= 0) {
    const m = JSON.parse(buf.slice(0, i)); buf = buf.slice(i + 1);
    if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
  }
});
const send = (method, params = {}, sessionId) => new Promise((res, rej) => {
  const n = ++id; waiting.set(n, (m) => m.error ? rej(new Error(method + ": " + m.error.message)) : res(m.result));
  toChrome.write(JSON.stringify({ id: n, method, params, sessionId }) + "\0");
});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const check = (name, ok, extra = "") => { console.log(`${ok ? "PASS" : "FAIL"}  ${name} ${extra}`); if (!ok) process.exitCode = 1; };
try {
  const loaded = await send("Extensions.loadUnpacked", { path: extension });
  check("extension loads, id matches the host", loaded.id === extId, loaded.id);
  await sleep(1500);
  const { targetId } = await send("Target.createTarget", { url: `${base}/page` });
  const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
  const js = async (expression) => (await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }, sessionId)).result.value;
  let text = "";
  for (let i = 0; i < 40 && !text.includes("clip.mp4"); i++) {
    await sleep(250);
    text = (await js(`document.getElementById("haul-panel-host")?.shadowRoot.textContent ?? ""`)) || "";
  }
  check("panel pops up by itself on a page with a video", text.includes("clip.mp4"));
  await sleep(1000);
  const size = await js(`document.getElementById("haul-panel-host").shadowRoot.querySelector(".size").textContent`);
  console.log("      size shown:", JSON.stringify(size));
  await js(`document.getElementById("haul-panel-host").shadowRoot.querySelector(".get").click()`);
  await sleep(2500);
  const fromPanel = received.find((m) => (m.url || "").includes("clip.mp4"));
  check("panel button reaches Haul, asks first", !!fromPanel && fromPanel.confirm === true, JSON.stringify(fromPanel));
  check("panel button shows Sent", (await js(`document.getElementById("haul-panel-host").shadowRoot.querySelector(".get").textContent`)) === "Sent");
  await js(`document.getElementById("dl").click()`);
  await sleep(3000);
  const captured = received.find((m) => (m.url || "").includes("thing.zip"));
  check("browser download is taken over automatically", !!captured && captured.confirm === true, JSON.stringify(captured));
  check("referrer is only the site's address", !!captured && captured.referrer === new URL(base).origin + "/", captured?.referrer);
} catch (e) { check("run", false, e.message); }
chrome.kill(); setTimeout(() => process.exit(), 300);
