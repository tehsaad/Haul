// Haul's indicator and panel. A small chip shows how many media files this page has; clicking it
// opens the list. Files are checked with the server only when the panel is opened.
(() => {
  const api = globalThis.browser ?? globalThis.chrome;
  const { isMediaLink, mediaKey, fileNameFor, EXCLUDED_HOSTS } = globalThis.HaulDetector;
  if (EXCLUDED_HOSTS.test(location.hostname)) return;

  const MAX_ROWS = 40;
  const MAX_CHECKS_AT_ONCE = 3;

  const rows = new Map(); // key -> { media, elements, ... }
  let root = null;
  let chip = null;
  let panel = null;
  let list = null;
  let open = false;
  let dismissed = false;
  let enabled = false;
  let checksRunning = 0;
  const checkQueue = [];

  function formatSize(bytes) {
    const units = ["B", "KB", "MB", "GB", "TB"];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit++;
    }
    return `${value.toFixed(value < 10 && unit > 0 ? 2 : value < 100 && unit > 0 ? 1 : 0)} ${units[unit]}`;
  }

  function formatDuration(seconds) {
    if (!seconds || !isFinite(seconds)) return "";
    const total = Math.round(seconds);
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    const rest = String(total % 60).padStart(2, "0");
    return hours ? `${hours}:${String(minutes).padStart(2, "0")}:${rest}` : `${minutes}:${rest}`;
  }

  function formatBitrate(bitsPerSecond) {
    return bitsPerSecond >= 1000000 ? `${(bitsPerSecond / 1000000).toFixed(1)} Mbps` : `${Math.round(bitsPerSecond / 1000)} kbps`;
  }

  function buildUi() {
    const host = document.createElement("div");
    host.id = "haul-panel-host";
    root = host.attachShadow({ mode: "open" });
    root.innerHTML = `
      <style>
        :host { all: initial; position: fixed; right: 16px; bottom: 16px; z-index: 2147483647; }
        * { box-sizing: border-box; }
        .chip { all: unset; cursor: pointer; display: block; margin-left: auto; width: max-content; padding: 4px 10px;
                font: 600 12px/1.4 system-ui, sans-serif; color: #1b1a18; background: #cf8a42; box-shadow: 0 2px 8px #0005; }
        .chip:hover { background: #db9a55; }
        .panel { width: 380px; margin-bottom: 6px; font: 13px/1.35 system-ui, sans-serif; color: #e6e2da; background: #242321;
                 border: 1px solid #393633; border-left: 3px solid #cf8a42; box-shadow: 0 4px 14px #0006; }
        .top { display: flex; justify-content: space-between; align-items: center; padding: 6px 8px; font-weight: 600; }
        .close { all: unset; cursor: pointer; padding: 0 5px; color: #9b958b; font-size: 16px; }
        .close:hover { color: #e6e2da; }
        .list { max-height: 340px; overflow-y: auto; }
        .row { display: flex; align-items: center; gap: 8px; padding: 5px 8px; border-top: 1px solid #393633; }
        .info { flex: 1; min-width: 0; }
        .name { overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }
        .meta, .note { font-size: 12px; color: #9b958b; overflow-wrap: anywhere; }
        .note.good { color: #8dae76; }
        .note.warn { color: #cdaa4c; }
        .note.bad { color: #d06f5c; }
        .legend { padding: 4px 8px; font-size: 11px; color: #9b958b; border-top: 1px solid #393633; }
        .get { all: unset; cursor: pointer; padding: 3px 9px; color: #1b1a18; background: #cf8a42; font-weight: 600; }
        .get:hover { background: #db9a55; }
        .get[disabled] { cursor: default; color: #9b958b; background: #35322e; }
        select { font: inherit; color: #e6e2da; background: #2e2c29; border: 1px solid #393633; padding: 1px 3px; max-width: 120px; }
      </style>
      <div class="panel" hidden>
        <div class="top"><span>Media on this page</span><button class="close" title="Hide on this page">×</button></div>
        <div class="list"></div>
        <div class="legend">≈ means an estimate. Other values come from the server or the player.</div>
      </div>
      <button class="chip"></button>`;
    chip = root.querySelector(".chip");
    panel = root.querySelector(".panel");
    list = root.querySelector(".list");
    chip.addEventListener("click", () => setOpen(!open));
    root.querySelector(".close").addEventListener("click", () => {
      dismissed = true;
      host.remove();
      root = null;
    });
    (document.body || document.documentElement).appendChild(host);
  }

  function setOpen(value) {
    open = value;
    panel.hidden = !open;
    if (open) rows.forEach(queueCheck);
  }

  function updateCount() {
    const count = rows.size;
    if (chip) {
      chip.textContent = `Haul: ${count}`;
      chip.title = `${count} media ${count === 1 ? "file" : "files"} on this page`;
      if (root) root.host.hidden = count === 0;
    }
    api.runtime.sendMessage({ type: "haul-count", count }).catch(() => {});
  }

  function pageTitle() {
    const og = document.querySelector('meta[property="og:title"]');
    const title = ((og && og.content) || document.title || "video").trim();
    return title.replace(/[\\/:*?"<>|\x00-\x1f]/g, " ").replace(/\s+/g, " ").trim().slice(0, 120) || "video";
  }

  function selectedVariant(row) {
    if (!row.media.stream || !row.media.variants || !row.media.variants.length) return null;
    return row.media.variants[row.select ? row.select.selectedIndex : 0];
  }

  // Everything known about a row, with estimates marked.
  function renderMeta(row) {
    const m = row.media;
    const parts = [];
    if (m.stream) {
      const variant = selectedVariant(row);
      parts.push(m.kind === "dash" ? "DASH" : "HLS");
      if (variant && variant.width && variant.height) parts.push(`${variant.width}×${variant.height}`);
      if (m.duration) parts.push(formatDuration(m.duration));
      if (variant && variant.size > 0) parts.push(`≈ ${formatSize(variant.size)}`);
      if (variant && variant.videoBitrate) parts.push(`video ${formatBitrate(variant.videoBitrate)}`);
      if (variant && variant.audioBitrate) parts.push(`audio ${formatBitrate(variant.audioBitrate)}`);
      else if (variant && variant.bitrate && !variant.videoBitrate) parts.push(formatBitrate(variant.bitrate));
    } else {
      if (m.format) parts.push(m.format.toUpperCase());
      if (m.width && m.height) parts.push(`${m.width}×${m.height}`);
      if (m.duration) parts.push(formatDuration(m.duration));
      if (m.size) parts.push(formatSize(m.size));
      if (m.size && m.duration) parts.push(`≈ ${formatBitrate((m.size * 8) / m.duration)}`);
    }
    row.meta.textContent = parts.join(" · ");

    const about = [m.source, m.mime].filter(Boolean).join(" · ");
    row.about.textContent = about;
  }

  function setNote(row, text, tone) {
    row.note.textContent = text;
    row.note.className = `note ${tone || ""}`;
  }

  function removeRow(key) {
    const row = rows.get(key);
    if (!row) return;
    row.element.remove();
    rows.delete(key);
    updateCount();
  }

  function queueCheck(row) {
    if (row.media.stream || row.checked) return;
    row.checked = true;
    setNote(row, "Checking…");
    checkQueue.push(row);
    pumpChecks();
  }

  function pumpChecks() {
    while (checksRunning < MAX_CHECKS_AT_ONCE && checkQueue.length) {
      const row = checkQueue.shift();
      checksRunning++;
      api.runtime.sendMessage({ type: "haul-verify", url: row.media.url })
        .then((answer) => applyCheck(row, answer))
        .catch(() => setNote(row, "Could not check this file.", "warn"))
        .finally(() => {
          checksRunning--;
          pumpChecks();
        });
    }
  }

  function applyCheck(row, answer) {
    const m = row.media;
    if (!answer || answer.unreachable) {
      setNote(row, "Not checked. In Haul: Settings → Browser → Allow the browser extension", "warn");
      return;
    }
    if (!answer.ok) {
      if (answer.restricted) setNote(row, "The server wants a login. You can still try, and enter it in Haul.", "warn");
      else if (answer.gone) setNote(row, "This link no longer works.", "bad");
      else setNote(row, answer.error || "Could not check this file.", "warn");
      return;
    }
    if (answer.notMedia) {
      removeRow(m.key); // the name said media, the content didn't
      return;
    }
    if (answer.effectiveUrl) {
      const finalKey = mediaKey(answer.effectiveUrl);
      if (finalKey !== m.key && rows.has(finalKey)) { // two links that redirect to one file
        removeRow(m.key);
        return;
      }
    }
    if (answer.size > 0) m.size = answer.size;
    if (answer.mime && !m.mime) m.mime = answer.mime;
    if (answer.format) m.format = answer.format;
    m.name = fileNameFor(answer.name || m.name, m.format, m.source);
    row.name.textContent = row.name.title = m.name;
    renderMeta(row);
    const what = answer.format ? `Checked: a real ${answer.format.toUpperCase()} file.` : "Checked: the server answered, but the format wasn't recognised.";
    const resume = answer.resumable ? " Can resume." : " Can't resume if interrupted.";
    setNote(row, what + resume, answer.format ? "good" : "warn");
  }

  function addRow(media) {
    const row = { media, elements: new Set() };
    const element = document.createElement("div");
    element.className = "row";
    element.dataset.key = media.key;
    row.element = element;

    const info = document.createElement("div");
    info.className = "info";
    row.name = document.createElement("div");
    row.name.className = "name";
    if (media.stream) media.name = `${pageTitle()}.mp4`;
    row.name.textContent = row.name.title = media.name;
    row.meta = document.createElement("div");
    row.meta.className = "meta";
    row.about = document.createElement("div");
    row.about.className = "meta";
    row.note = document.createElement("div");
    row.note.className = "note";
    info.append(row.name, row.meta, row.about, row.note);
    element.append(info);
    list.append(element);
    rows.set(media.key, row);

    if (media.stream && media.error) {
      setNote(row, media.encrypted ? "Protected video. Haul can't download this."
        : media.restricted ? "This video needs a login, so Haul can't check it from here."
        : media.unreachable ? "In Haul: Settings → Browser → Allow the browser extension"
        : media.error, media.encrypted ? "bad" : "warn");
      renderMeta(row);
      return row;
    }

    if (media.stream && media.variants.length > 1) {
      row.select = document.createElement("select");
      for (const variant of media.variants) {
        const option = document.createElement("option");
        option.value = variant.id;
        option.textContent = variant.label;
        row.select.append(option);
      }
      row.select.addEventListener("change", () => renderMeta(row));
      element.append(row.select);
    }

    const button = document.createElement("button");
    button.className = "get";
    button.textContent = "Download";
    button.addEventListener("click", async () => {
      button.disabled = true;
      button.textContent = "Sending";
      const answer = await api.runtime.sendMessage({
        type: "haul-add", url: media.url, filename: media.name,
        stream: media.stream ? media.kind : "", variant: row.select ? row.select.value : "",
      });
      if (answer && answer.ok) {
        button.textContent = "Sent";
      } else {
        button.disabled = false;
        button.textContent = "Retry";
        setNote(row, "In Haul: Settings → Browser → Allow the browser extension", "bad");
      }
    });
    element.append(button);
    if (media.stream) setNote(row, "From the stream's manifest. Sizes are estimates.");
    else if (media.via === "page") setNote(row, "Linked from this page. Not checked yet.");
    else setNote(row, "Seen loading in this page.");
    renderMeta(row);
    if (open) queueCheck(row);
    return row;
  }

  function offer(media) {
    if (dismissed || !enabled) return;
    const existing = rows.get(media.key);
    if (existing) {
      if (media.size && !existing.media.size) existing.media.size = media.size;
      if (media.mime && !existing.media.mime) existing.media.mime = media.mime;
      if (media.format && !existing.media.format) existing.media.format = media.format;
      renderMeta(existing);
      return;
    }
    if (rows.size >= MAX_ROWS) return;
    // The same name and size under another address is the same file.
    if (media.size) {
      for (const row of rows.values()) {
        if (row.media.size === media.size && row.media.name === media.name) return;
      }
    }
    if (!root) buildUi();
    addRow(media);
    updateCount();
  }

  function offerPageAddress(address, kind) {
    const url = new URL(address);
    const name = decodeURIComponent(url.pathname.split("/").pop() || "");
    const extension = (/\.([a-z0-9]+)$/i.exec(name) || [])[1] || "";
    offer({ url: address, key: mediaKey(address), kind, format: extension.toLowerCase(), mime: "", name: name || url.hostname,
            size: 0, source: url.hostname, via: "page" });
  }

  function watchElement(element, row) {
    if (row.elements.has(element)) return;
    row.elements.add(element);
    const read = () => {
      if (element.duration && isFinite(element.duration)) row.media.duration = element.duration;
      if (element.videoWidth) {
        row.media.width = element.videoWidth;
        row.media.height = element.videoHeight;
      }
      renderMeta(row);
    };
    element.addEventListener("loadedmetadata", read);
    if (element.readyState >= 1) read();
  }

  function scanPage() {
    for (const element of document.querySelectorAll("video, audio")) {
      if (element.mediaKeys) continue; // playing protected content
      const src = element.currentSrc || element.src || (element.querySelector("source") || {}).src;
      if (!/^https?:/i.test(src || "")) continue; // blob: and data: are streams, not files
      if (/\.(m3u8|m3u|mpd|ts|m4s)$/i.test(new URL(src).pathname)) continue; // reported by the background
      if (isMediaLink(src)) offerPageAddress(src, element.tagName === "AUDIO" ? "audio" : "video");
      const row = rows.get(mediaKey(src));
      if (row) watchElement(element, row);
    }
    let seen = 0;
    for (const link of document.querySelectorAll("a[href]")) {
      if (++seen > 3000) break;
      if (/^https?:/i.test(link.href) && isMediaLink(link.href)) offerPageAddress(link.href, "");
    }
  }

  let scanTimer = null;
  function scanSoon() {
    if (scanTimer) return;
    scanTimer = setTimeout(() => {
      scanTimer = null;
      scanPage();
    }, 500);
  }

  async function start() {
    const { panel: wanted } = await api.storage.local.get("panel");
    enabled = wanted !== false;
    api.runtime.onMessage.addListener((message) => {
      if (message.type === "haul-media") {
        offer(message.media);
        scanSoon();
      }
      if (message.type === "haul-media-remove") removeRow(message.key);
      if (message.type === "haul-show") {
        dismissed = false;
        if (!root && rows.size) {
          buildUi();
          for (const row of rows.values()) list.append(row.element);
          updateCount();
        }
        if (root) setOpen(true);
      }
    });
    const known = await api.runtime.sendMessage({ type: "haul-list" });
    (known || []).forEach(offer);
    if (!enabled) return;
    scanPage();
    new MutationObserver(scanSoon).observe(document.documentElement, { childList: true, subtree: true });
  }
  start();
})();
