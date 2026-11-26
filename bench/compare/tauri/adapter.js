// Tauri v2 adapter (app.withGlobalTauri = true): window.__TAURI__.core.invoke / webviewWindow listen.
(() => {
  const tauri = window.__TAURI__;
  const names = { echo: 'text', ping: 'seq', mark: 'text', report: 'json', burst: 'n' };
  window.__cmp = {
    invoke: (name, arg) => tauri.core.invoke(name, name in names ? { [names[name]]: arg } : {}),
    listen: async (event, cb) => { await tauri.webviewWindow.getCurrentWebviewWindow().listen(event, (e) => cb(e.payload)); },
  };
  window.__cmpReady = Promise.resolve();
})();
