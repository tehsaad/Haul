// Haul browser extension (background). It hands downloads to the Haul desktop app and notices
// video files and streams that pages load. It never reads cookies or passwords.
const api = globalThis.browser ?? globalThis.chrome;
const HOST = "io.github.tehsaad.haul";
const MENU_ID = "haul-download";
const MIN_MEDIA_SIZE = 1024 * 1024; // smaller responses are icons, previews and stream pieces

// YouTube streams are protected and its terms forbid downloading, so Haul stays out of them.
const EXCLUDED_HOSTS = /(^|\.)(youtube\.com|youtube-nocookie\.com|youtu\.be|googlevideo\.com|ytimg\.com)$/i;
const MEDIA_NAME = /\.(mp4|m4v|webm|mkv|mov|avi|flv|wmv|mp3|m4a|flac|wav|ogg|opus|aac)$/i;
const STREAM_PIECE = /\.(ts|m4s)$/i;
const MANIFEST_NAME = /\.(m3u8|m3u|mpd)$/i;
const MANIFEST_TYPE = /mpegurl|dash\+xml/i;

const passThrough = new Set(); // downloads restarted in the browser because Haul was unavailable
const mediaByTab = new Map();
const streamChildren = new Map(); // tab -> playlists that belong to a master already listed

function originOf(page) {
  try {
    const url = new URL(page);
    return /^https?:$/.test(url.protocol) ? url.origin + "/" : "";
  } catch {
    return "";
  }
}

function askHost(message) {
  return new Promise((resolve) => {
    const done = (answer) => {
      const failure = api.runtime.lastError;
      resolve(failure ? { ok: false, error: failure.message, unreachable: true } : answer);
    };
    const result = api.runtime.sendNativeMessage(HOST, message, done);
    if (result && result.then) { // Firefox returns a promise
      result.then(resolve, (e) => resolve({ ok: false, error: e.message, unreachable: true }));
    }
  });
}

function sendToHaul(url, filename, referrer, stream, variant) {
  return askHost({
    type: "add", url, filename: filename || "", referrer: originOf(referrer),
    stream: stream || "", variant: variant || "",
  }).then((answer) => {
    const ok = !!(answer && answer.ok);
    api.action.setBadgeText({ text: ok ? "" : "!" });
    api.action.setTitle({ title: ok ? "Haul" : "Haul could not be reached. Allow the extension in Haul's settings." });
    return answer || { ok: false, error: "No answer from Haul." };
  });
}

api.runtime.onInstalled.addListener(() => {
  api.contextMenus.create({
    id: MENU_ID,
    title: "Download with Haul",
    contexts: ["link", "image", "video", "audio"],
  });
});

api.contextMenus.onClicked.addListener((info) => {
  if (info.menuItemId === MENU_ID)
    sendToHaul(info.linkUrl || info.srcUrl, "", info.pageUrl);
});

// Browser downloads go to Haul automatically unless switched off in the popup.
api.downloads.onCreated.addListener(async (item) => {
  if (passThrough.delete(item.url)) return;
  const { capture } = await api.storage.local.get("capture");
  if (capture === false || !/^(https?|ftp):/i.test(item.url)) return;

  try {
    await api.downloads.cancel(item.id);
  } catch {
    return; // it already finished in the browser, so there is nothing left to take over
  }
  api.downloads.erase({ id: item.id });
  const name = (item.filename || "").split(/[\\/]/).pop();
  const answer = await sendToHaul(item.url, name, item.referrer);
  if (!answer.ok) {
    passThrough.add(item.url); // never lose the user's download
    api.downloads.download({ url: item.url });
  }
});

function header(details, name) {
  const found = details.responseHeaders.find((h) => h.name.toLowerCase() === name);
  return found ? found.value : "";
}

function isExcluded(details) {
  const url = new URL(details.url);
  if (EXCLUDED_HOSTS.test(url.hostname) || details.statusCode >= 300) return true;
  const page = details.documentUrl || details.initiator;
  try {
    return !!page && EXCLUDED_HOSTS.test(new URL(page).hostname);
  } catch {
    return false;
  }
}

function isManifest(details) {
  return MANIFEST_NAME.test(new URL(details.url).pathname) || MANIFEST_TYPE.test(header(details, "content-type"));
}

function tabState(tabId) {
  if (!mediaByTab.has(tabId)) mediaByTab.set(tabId, new Map());
  if (!streamChildren.has(tabId)) streamChildren.set(tabId, new Set());
  return mediaByTab.get(tabId);
}

function tell(tabId, message) {
  api.tabs.sendMessage(tabId, message, { frameId: 0 }).catch(() => {});
}

// One parser for everything: the native host reads the manifest and lists its qualities. A master
// playlist also names its child playlists, which are the same video and must not be listed twice.
async function offerStream(details) {
  const tabId = details.tabId;
  const media = tabState(tabId);
  const url = details.url;
  if (media.has(url) || streamChildren.get(tabId).has(url)) return;
  const entry = { url, name: "", size: 0, stream: true, pending: true };
  media.set(url, entry);

  const answer = await askHost({ type: "inspect", url, referrer: originOf(details.documentUrl || details.initiator) });
  if (!mediaByTab.has(tabId) || streamChildren.get(tabId).has(url)) return;
  delete entry.pending;
  if (answer && answer.ok && answer.kind === "file") {
    media.delete(url);
    return;
  }
  entry.kind = answer ? answer.kind : "";
  entry.variants = (answer && answer.variants) || [];
  entry.error = answer && answer.ok ? "" : (answer ? answer.error : "No answer from Haul.");
  entry.encrypted = !!(answer && answer.encrypted);
  entry.restricted = !!(answer && answer.restricted);
  entry.unreachable = !!(answer && answer.unreachable);

  for (const child of (answer && answer.children) || []) {
    streamChildren.get(tabId).add(child);
    if (media.delete(child)) tell(tabId, { type: "haul-media-remove", url: child });
  }
  tell(tabId, { type: "haul-media", media: entry });
}

function describeMedia(details) {
  const url = new URL(details.url);
  const type = header(details, "content-type").toLowerCase();
  const disposition = /filename\*?=(?:UTF-8'')?"?([^";]+)/i.exec(header(details, "content-disposition"));
  const name = decodeURIComponent(disposition ? disposition[1] : url.pathname.split("/").pop() || url.hostname);
  if (STREAM_PIECE.test(url.pathname) || /mpegurl|dash\+xml/.test(type) || url.searchParams.has("range")) return null;
  if (!/^(video|audio)\//.test(type) && !MEDIA_NAME.test(name)) return null;

  const total = /\/(\d+)\s*$/.exec(header(details, "content-range"));
  const size = Number(total ? total[1] : header(details, "content-length")) || 0;
  return size >= MIN_MEDIA_SIZE ? { url: details.url, name, size } : null;
}

api.webRequest.onHeadersReceived.addListener(
  (details) => {
    if (details.tabId < 0 || isExcluded(details)) return;
    if (isManifest(details)) {
      offerStream(details);
      return;
    }
    if (details.type === "xmlhttprequest") return; // players fetch stream pieces this way
    const media = describeMedia(details);
    if (!media) return;
    tabState(details.tabId).set(media.url, media);
    tell(details.tabId, { type: "haul-media", media });
  },
  // Players such as hls.js and dash.js fetch manifests as xmlhttprequest.
  { urls: ["http://*/*", "https://*/*"], types: ["media", "object", "other", "xmlhttprequest"] },
  ["responseHeaders"]
);

// A new page starts with an empty list.
api.webRequest.onBeforeRequest.addListener(
  (details) => {
    mediaByTab.delete(details.tabId);
    streamChildren.delete(details.tabId);
  },
  { urls: ["http://*/*", "https://*/*"], types: ["main_frame"] }
);
api.tabs.onRemoved.addListener((tabId) => {
  mediaByTab.delete(tabId);
  streamChildren.delete(tabId);
});

api.runtime.onMessage.addListener((message, sender, respond) => {
  if (message.type === "haul-list") {
    respond([...(mediaByTab.get(sender.tab.id) ?? new Map()).values()].filter((media) => !media.pending));
    return false;
  }
  if (message.type === "haul-add") {
    sendToHaul(message.url, message.filename, sender.tab && sender.tab.url, message.stream, message.variant).then(respond);
    return true; // the answer comes later
  }
  return false;
});
