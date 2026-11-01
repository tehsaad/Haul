const api = globalThis.browser ?? globalThis.chrome;
for (const key of ["capture", "panel"]) {
  const box = document.getElementById(key);
  api.storage.local.get(key).then((stored) => { box.checked = stored[key] !== false; }); // both default to on
  box.addEventListener("change", () => api.storage.local.set({ [key]: box.checked }));
}
