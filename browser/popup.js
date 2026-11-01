const api = globalThis.browser ?? globalThis.chrome;
const box = document.getElementById("capture");
api.storage.local.get("capture").then(({ capture }) => { box.checked = !!capture; });
box.addEventListener("change", () => api.storage.local.set({ capture: box.checked }));
