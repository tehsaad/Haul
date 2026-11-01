// Haul's small panel: lists video files and streams found on this page, with quality choices.
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
        select { font: inherit; color: #e6e2da; background: #2e2c29; border: 1px solid #393633; padding: 1px 3px; max-width: 110px; }
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

  function pageTitle() {
    const og = document.querySelector('meta[property="og:title"]');
    const title = ((og && og.content) || document.title || "video").trim();
    return title.replace(/[\\/:*?"<>|\x00-\x1f]/g, " ").replace(/\s+/g, " ").trim().slice(0, 120) || "video";
  }

  function addRow(media) {
    const row = document.createElement("div");
    row.className = "row";
    row.dataset.url = media.url;
    const info = document.createElement("div");
    info.className = "info";
    const name = document.createElement("div");
    name.className = "name";
    const title = media.stream ? `${pageTitle()}.mp4` : media.name;
    name.textContent = name.title = title;
    const detail = document.createElement("div");
    detail.className = "size";
    detail.textContent = formatSize(media.size);
    info.append(name, detail);

    if (media.stream && media.error) {
      detail.className = "error";
      detail.textContent = media.encrypted ? "Protected video. Haul can't download this."
        : media.restricted ? "This video needs a login, so Haul can't check it from here."
        : media.unreachable ? "In Haul: Settings → Browser → Allow the browser extension"
        : media.error;
      row.append(info);
      list.append(row);
      return;
    }

    let quality = null;
    if (media.stream) {
      const sizeOf = (variant) => (variant && variant.size > 0 ? `About ${formatSize(variant.size)}` : "Stream");
      detail.textContent = sizeOf(media.variants[0]);
      if (media.variants.length > 1) {
        quality = document.createElement("select");
        for (const variant of media.variants) {
          const option = document.createElement("option");
          option.value = variant.id;
          option.textContent = variant.label;
          quality.append(option);
        }
        quality.addEventListener("change", () => {
          detail.textContent = sizeOf(media.variants[quality.selectedIndex]);
        });
      }
    }
    const button = document.createElement("button");
    button.className = "get";
    button.textContent = "Download";
    button.addEventListener("click", async () => {
      button.disabled = true;
      button.textContent = "Sending";
      const answer = await api.runtime.sendMessage({
        type: "haul-add", url: media.url, filename: title,
        stream: media.stream ? media.kind : "", variant: quality ? quality.value : "",
      });
      if (answer && answer.ok) {
        button.textContent = "Sent";
      } else {
        button.disabled = false;
        button.textContent = "Retry";
        detail.className = "error";
        detail.textContent = "In Haul: Settings → Browser → Allow the browser extension";
      }
    });
    row.append(info);
    if (quality) row.append(quality);
    row.append(button);
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
      if (/\.(m3u8|m3u|mpd|ts|m4s)$/i.test(url.pathname)) continue; // reported by the background page
      offer({ url: src, name: decodeURIComponent(url.pathname.split("/").pop() || url.hostname), size: 0 });
    }
  }

  async function start() {
    const { panel } = await api.storage.local.get("panel");
    if (panel === false) return;
    api.runtime.onMessage.addListener((message) => {
      if (message.type === "haul-media") offer(message.media);
      if (message.type === "haul-media-remove" && found.delete(message.url) && list) {
        for (const row of list.querySelectorAll(".row")) {
          if (row.dataset.url === message.url) row.remove();
        }
      }
    });
    const known = await api.runtime.sendMessage({ type: "haul-list" });
    (known || []).forEach(offer);
    scanPage();
    new MutationObserver(scanPage).observe(document.documentElement, { childList: true, subtree: true });
  }
  start();
})();
