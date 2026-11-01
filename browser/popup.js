const api = globalThis.browser ?? globalThis.chrome;
for (const key of ["capture", "panel", "autoOpen"]) {
  const box = document.getElementById(key);
  api.storage.local.get(key).then((stored) => { box.checked = stored[key] !== false; }); // both default to on
  box.addEventListener("change", () => api.storage.local.set({ [key]: box.checked }));
}
document.getElementById("show").addEventListener("click", async () => {
  const [tab] = await api.tabs.query({ active: true, currentWindow: true });
  if (tab) api.tabs.sendMessage(tab.id, { type: "haul-show" }).catch(() => {});
  window.close();
});
api.tabs.query({ active: true, currentWindow: true }).then(([tab]) => {
  try {
    if (tab && /(^|\.)(youtube\.com|youtu\.be|youtube-nocookie\.com)$/i.test(new URL(tab.url).hostname)) {
      document.getElementById("unsupported").hidden = false;
      document.getElementById("show").hidden = true;
    }
  } catch {}
});
