// Haul browser extension (background). It watches response headers for media, keeps a list per
// tab, and passes downloads to the Haul desktop app. It never reads cookies or passwords.
const api = globalThis.browser ?? globalThis.chrome;
if (typeof importScripts === "function" && !globalThis.HaulDetector) importScripts("detector.js");
const { classifyResponse, mediaKey, EXCLUDED_HOSTS } = globalThis.HaulDetector;

const HOST = "io.github.tehsaad.haul";
const MENU_ID = "haul-download";

const passThrough = new Set(); // downloads restarted in the browser because Haul was unavailable

// tab id -> { media: Map(key -> entry), children: Set(key) }. The browser may stop this worker when
// idle, so the lists are also kept in session storage.
const tabs = new Map();
const storage = api.storage.session;
const restored = storage
  ? storage.get("tabs").then(({ tabs: saved }) => {
      for (const [id, state] of Object.entries(saved || {}))
        tabs.set(Number(id), { media: new Map(state.media), children: new Set(state.children) });
    }).catch(() => {})
  : Promise.resolve();

let saveTimer = null;
function saveSoon() {
  if (!storage || saveTimer) return;
  saveTimer = setTimeout(() => {
    saveTimer = null;
    const saved = {};
    for (const [id, state] of tabs)
      saved[id] = { media: [...state.media], children: [...state.children] };
    storage.set({ tabs: saved }).catch(() => {});
  }, 500);
}

function tabState(tabId) {
  if (!tabs.has(tabId)) tabs.set(tabId, { media: new Map(), children: new Set() });
  return tabs.get(tabId);
}

function forgetTab(tabId) {
  if (tabs.delete(tabId)) saveSoon();
}

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
  }).then((answer) => answer || { ok: false, error: "No answer from Haul." });
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

  // Hold the download while Haul asks the server. If the server won't serve Haul (a login, say, or
  // a site that only accepts browsers), the browser simply carries on and nothing is lost.
  try {
    await api.downloads.pause(item.id);
  } catch {
    return; // it already finished in the browser, so there is nothing left to take over
  }
  const check = await askHost({ type: "verify", url: item.url, referrer: originOf(item.referrer) });
  if (!check || !check.ok) {
    api.downloads.resume(item.id).catch(() => {});
    return;
  }
  try {
    await api.downloads.cancel(item.id);
  } catch {
    return;
  }
  api.downloads.erase({ id: item.id });
  const name = (item.filename || "").split(/[\\/]/).pop();
  const answer = await sendToHaul(item.url, name, item.referrer);
  if (!answer.ok) {
    passThrough.add(item.url); // never lose the user's download
    api.downloads.download({ url: item.url });
  }
});

function tell(tabId, message) {
  api.tabs.sendMessage(tabId, message, { frameId: 0 }).catch(() => {});
}

// One parser for everything: the native host reads the manifest and lists its qualities. A master
// playlist also names its child playlists, which are the same video and must not be listed twice.
async function offerStream(details, found) {
  const state = tabState(details.tabId);
  if (state.media.has(found.key) || state.children.has(found.key)) return;
  const entry = { url: details.url, key: found.key, name: "", size: 0, stream: true, pending: true,
                  mime: found.mime, source: new URL(details.url).hostname };
  state.media.set(found.key, entry);

  const answer = await askHost({ type: "inspect", url: details.url, referrer: originOf(details.initiator || details.documentUrl) });
  const current = tabs.get(details.tabId);
  if (!current || current.children.has(found.key)) return;
  delete entry.pending;
  if (answer && answer.ok && answer.kind === "file") {
    current.media.delete(found.key);
    saveSoon();
    return;
  }
  entry.kind = answer ? answer.kind : "";
  entry.variants = (answer && answer.variants) || [];
  entry.duration = (answer && answer.duration) || 0;
  entry.error = answer && answer.ok ? "" : (answer ? answer.error : "No answer from Haul.");
  entry.encrypted = !!(answer && answer.encrypted);
  entry.restricted = !!(answer && answer.restricted);
  entry.unreachable = !!(answer && answer.unreachable);

  for (const child of (answer && answer.children) || []) {
    const childKey = mediaKey(child);
    current.children.add(childKey);
    if (current.media.delete(childKey)) tell(details.tabId, { type: "haul-media-remove", key: childKey });
  }
  saveSoon();
  tell(details.tabId, { type: "haul-media", media: entry });
}

function isExcludedPage(details) {
  try {
    const page = details.initiator || details.documentUrl;
    return !!page && EXCLUDED_HOSTS.test(new URL(page).hostname);
  } catch {
    return false;
  }
}

api.webRequest.onHeadersReceived.addListener(
  (details) => {
    if (details.tabId < 0 || isExcludedPage(details)) return;
    const found = classifyResponse(details);
    if (!found) return;
    Promise.all([restored, api.storage.local.get("panel")]).then(([, { panel }]) => {
      if (panel === false) return; // switched off: no list, and no contact with any server
      if (found.stream) {
        offerStream(details, found);
        return;
      }
      const state = tabState(details.tabId);
      // With a manifest on the page, whatever a script fetches next is that stream's pieces.
      if (found.via === "xmlhttprequest" && [...state.media.values()].some((m) => m.stream)) return;
      if (state.children.has(found.key)) return;
      const known = state.media.get(found.key);
      if (known) {
        known.size = known.size || found.size;
        return;
      }
      state.media.set(found.key, found);
      saveSoon();
      tell(details.tabId, { type: "haul-media", media: found });
    });
  },
  // main_frame: a media file opened directly. xmlhttprequest: players that fetch by script.
  { urls: ["http://*/*", "https://*/*"], types: ["media", "object", "other", "xmlhttprequest", "main_frame", "sub_frame"] },
  ["responseHeaders"]
);

// A new page starts with an empty list.
api.webRequest.onBeforeRequest.addListener(
  (details) => { restored.then(() => forgetTab(details.tabId)); },
  { urls: ["http://*/*", "https://*/*"], types: ["main_frame"] }
);
api.tabs.onRemoved.addListener((tabId) => { restored.then(() => forgetTab(tabId)); });

api.runtime.onMessage.addListener((message, sender, respond) => {
  const tabId = sender.tab && sender.tab.id;
  if (message.type === "haul-list") {
    restored.then(() => {
      const state = tabs.get(tabId);
      respond(state ? [...state.media.values()].filter((media) => !media.pending) : []);
    });
    return true;
  }
  if (message.type === "haul-count") {
    api.action.setBadgeText({ tabId, text: message.count ? String(message.count) : "" });
    return false;
  }
  if (message.type === "haul-verify") {
    askHost({ type: "verify", url: message.url, referrer: originOf(sender.tab && sender.tab.url) }).then(respond);
    return true;
  }
  if (message.type === "haul-browser-download") {
    passThrough.add(message.url);
    api.downloads.download({ url: message.url }).then(() => respond({ ok: true }), (e) => respond({ ok: false, error: e.message }));
    return true;
  }
  if (message.type === "haul-add") {
    sendToHaul(message.url, message.filename, sender.tab && sender.tab.url, message.stream, message.variant).then(respond);
    return true;
  }
  return false;
});
