// Haul browser extension (background). It hands downloads to the Haul desktop app and notices
// audio and video files that pages load. It never reads cookies, passwords or page text.
const api = globalThis.browser ?? globalThis.chrome;
const HOST = "io.github.tehsaad.haul";
const MENU_ID = "haul-download";
const MIN_MEDIA_SIZE = 1024 * 1024; // smaller responses are icons, previews and stream pieces

// YouTube streams are protected and its terms forbid downloading, so Haul stays out of them.
const EXCLUDED_HOSTS = /(^|\.)(youtube\.com|youtube-nocookie\.com|youtu\.be|googlevideo\.com|ytimg\.com)$/i;
const MEDIA_NAME = /\.(mp4|m4v|webm|mkv|mov|avi|flv|wmv|mp3|m4a|flac|wav|ogg|opus|aac)$/i;
const STREAM_PIECE = /\.(ts|m4s|m3u8|mpd)$/i;

const passThrough = new Set(); // downloads restarted in the browser because Haul was unavailable
const mediaByTab = new Map();

function originOf(page) {
  try {
    const url = new URL(page);
    return /^https?:$/.test(url.protocol) ? url.origin + "/" : "";
  } catch {
    return "";
  }
}

function sendToHaul(url, filename, referrer) {
  return new Promise((resolve) => {
    const message = { type: "add", url, filename: filename || "", referrer: originOf(referrer) };
    const done = (answer) => {
      const failure = api.runtime.lastError;
      resolve(failure ? { ok: false, error: failure.message } : answer);
    };
    const result = api.runtime.sendNativeMessage(HOST, message, done);
    if (result && result.then) { // Firefox returns a promise
      result.then(resolve, (e) => resolve({ ok: false, error: e.message }));
    }
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

function describeMedia(details) {
  const url = new URL(details.url);
  if (EXCLUDED_HOSTS.test(url.hostname) || details.statusCode >= 300) return null;
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
    if (details.tabId < 0) return;
    const media = describeMedia(details);
    if (!media) return;
    if (!mediaByTab.has(details.tabId)) mediaByTab.set(details.tabId, new Map());
    mediaByTab.get(details.tabId).set(media.url, media);
    api.tabs.sendMessage(details.tabId, { type: "haul-media", media }, { frameId: 0 }).catch(() => {});
  },
  { urls: ["http://*/*", "https://*/*"], types: ["media", "object", "other"] },
  ["responseHeaders"]
);

// A new page starts with an empty list.
api.webRequest.onBeforeRequest.addListener(
  (details) => { mediaByTab.delete(details.tabId); },
  { urls: ["http://*/*", "https://*/*"], types: ["main_frame"] }
);
api.tabs.onRemoved.addListener((tabId) => mediaByTab.delete(tabId));

api.runtime.onMessage.addListener((message, sender, respond) => {
  if (message.type === "haul-list") {
    respond([...(mediaByTab.get(sender.tab.id) ?? new Map()).values()]);
    return false;
  }
  if (message.type === "haul-add") {
    sendToHaul(message.url, message.filename, sender.tab && sender.tab.url).then(respond);
    return true; // the answer comes later
  }
  return false;
});
