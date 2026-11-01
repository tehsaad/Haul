const api = globalThis.browser ?? globalThis.chrome;
for (const key of ["capture", "panel"]) {
  const box = document.getElementById(key);
  api.storage.local.get(key).then((stored) => { box.checked = stored[key] !== false; }); // both default to on
  box.addEventListener("change", () => api.storage.local.set({ [key]: box.checked }));
}
document.getElementById("show").addEventListener("click", async () => {
  const [tab] = await api.tabs.query({ active: true, currentWindow: true });
  if (tab) api.tabs.sendMessage(tab.id, { type: "haul-show" }).catch(() => {});
  window.close();
});
