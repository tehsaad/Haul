// Haul browser extension. Sends links to the Haul desktop app; it reads no pages,
// cookies or passwords. Works in Chromium browsers and Firefox.
const api = globalThis.browser ?? globalThis.chrome;
const HOST = "io.github.tehsaad.haul";
const MENU_ID = "haul-download";
const passThrough = new Set(); // downloads we restarted in the browser because Haul was unavailable

function sendToHaul(url, filename) {
  return new Promise((resolve) => {
    const done = (answer) => {
      const failure = api.runtime.lastError;
      resolve(failure ? { ok: false, error: failure.message } : answer);
    };
    const message = { type: "add", url, filename: filename || "" };
    const result = api.runtime.sendNativeMessage(HOST, message, done);
    if (result && result.then) { // Firefox returns a promise
      result.then(resolve, (e) => resolve({ ok: false, error: e.message }));
    }
  });
}

function setBadge(ok) {
  api.action.setBadgeText({ text: ok ? "" : "!" });
  api.action.setTitle({ title: ok ? "Haul" : "Haul could not be reached. See the popup." });
}

api.runtime.onInstalled.addListener(() => {
  api.contextMenus.create({
    id: MENU_ID,
    title: "Download with Haul",
    contexts: ["link", "image", "video", "audio"],
  });
});

api.contextMenus.onClicked.addListener(async (info) => {
  if (info.menuItemId !== MENU_ID) return;
  const answer = await sendToHaul(info.linkUrl || info.srcUrl);
  setBadge(answer && answer.ok);
});

// Optional, off by default: hand every browser download to Haul.
// A download that needs the browser's login session is better left in the browser.
api.downloads.onCreated.addListener(async (item) => {
  if (passThrough.delete(item.url)) return;
  const { capture } = await api.storage.local.get("capture");
  if (!capture || !/^(https?|ftp):/i.test(item.url)) return;

  await api.downloads.cancel(item.id);
  api.downloads.erase({ id: item.id });
  const name = (item.filename || "").split(/[\\/]/).pop();
  const answer = await sendToHaul(item.url, name);
  setBadge(answer && answer.ok);
  if (!answer || !answer.ok) {
    passThrough.add(item.url); // never lose the user's download
    api.downloads.download({ url: item.url });
  }
});
