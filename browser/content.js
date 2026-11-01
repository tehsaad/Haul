// Haul's small panel: lists audio and video files found on this page, with a Download button.
(() => {
  const api = globalThis.browser ?? globalThis.chrome;
  const EXCLUDED_HOST = /(^|\.)(youtube\.com|youtube-nocookie\.com)$/i;
  const MAX_ROWS = 5;
  if (EXCLUDED_HOST.test(location.hostname)) return;

  const found = new Map();
  let root = null;
  let list = null;
  let dismissed = false;

  function formatSize(bytes) {
    if (!bytes) return "";
    const units = ["B", "KB", "MB", "GB", "TB"];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit++;
    }
    return `${value.toFixed(value < 10 ? 1 : 0)} ${units[unit]}`;
  }

  function buildPanel() {
    const host = document.createElement("div");
    host.id = "haul-panel-host";
    root = host.attachShadow({ mode: "open" });
    root.innerHTML = `
      <style>
        :host { all: initial; position: fixed; right: 16px; bottom: 16px; z-index: 2147483647; }
        .panel { width: 320px; font: 13px/1.35 system-ui, sans-serif; color: #e6e2da; background: #242321;
                 border: 1px solid #393633; border-left: 3px solid #cf8a42; box-shadow: 0 4px 14px #0006; }
        .top { display: flex; justify-content: space-between; align-items: center; padding: 6px 8px; font-weight: 600; }
        .close { all: unset; cursor: pointer; padding: 0 5px; color: #9b958b; font-size: 16px; }
        .close:hover { color: #e6e2da; }
        .row { display: flex; align-items: center; gap: 8px; padding: 5px 8px; border-top: 1px solid #393633; }
        .info { flex: 1; min-width: 0; }
        .name { overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }
        .size, .error { font-size: 12px; color: #9b958b; }
        .error { color: #d06f5c; }
        .get { all: unset; cursor: pointer; padding: 3px 9px; color: #1b1a18; background: #cf8a42; font-weight: 600; }
        .get:hover { background: #db9a55; }
        .get[disabled] { cursor: default; color: #9b958b; background: #35322e; }
      </style>
      <div class="panel">
        <div class="top"><span>Download with Haul</span><button class="close" title="Hide">×</button></div>
        <div class="list"></div>
      </div>`;
    list = root.querySelector(".list");
    root.querySelector(".close").addEventListener("click", () => {
      dismissed = true;
      host.remove();
    });
    (document.body || document.documentElement).appendChild(host);
  }

  function addRow(media) {
    const row = document.createElement("div");
    row.className = "row";
    const info = document.createElement("div");
    info.className = "info";
    const name = document.createElement("div");
    name.className = "name";
    name.textContent = name.title = media.name;
    const detail = document.createElement("div");
    detail.className = "size";
    detail.textContent = formatSize(media.size);
    const button = document.createElement("button");
    button.className = "get";
    button.textContent = "Download";
    button.addEventListener("click", async () => {
      button.disabled = true;
      button.textContent = "Sending";
      const answer = await api.runtime.sendMessage({ type: "haul-add", url: media.url, filename: media.name });
      if (answer && answer.ok) {
        button.textContent = "Sent";
      } else {
        button.disabled = false;
        button.textContent = "Retry";
        detail.className = "error";
        detail.textContent = "In Haul: Settings → Browser → Allow the browser extension";
      }
    });
    info.append(name, detail);
    row.append(info, button);
    list.append(row);
  }

  function offer(media) {
    if (dismissed || found.has(media.url) || found.size >= MAX_ROWS) return;
    found.set(media.url, media);
    if (!root) buildPanel();
    addRow(media);
  }

  function scanPage() {
    for (const element of document.querySelectorAll("video, audio, video source, audio source")) {
      const src = element.currentSrc || element.src;
      if (!/^https?:/i.test(src || "")) continue; // blob: and data: are streams, not files
      const url = new URL(src);
      if (/\.(m3u8|mpd|ts|m4s)$/i.test(url.pathname)) continue;
      offer({ url: src, name: decodeURIComponent(url.pathname.split("/").pop() || url.hostname), size: 0 });
    }
  }

  async function start() {
    const { panel } = await api.storage.local.get("panel");
    if (panel === false) return;
    api.runtime.onMessage.addListener((message) => {
      if (message.type === "haul-media") offer(message.media);
    });
    const known = await api.runtime.sendMessage({ type: "haul-list" });
    (known || []).forEach(offer);
    scanPage();
    new MutationObserver(scanPage).observe(document.documentElement, { childList: true, subtree: true });
  }
  start();
})();
