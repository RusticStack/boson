// Electron adapter: contextBridge (preload.js) -> ipcRenderer.invoke / webContents.send.
window.__cmp = {
  invoke: (name, arg) => window.__cmpBridge.invoke(name, arg),
  listen: async (event, cb) => { window.__cmpBridge.on(event, cb); },
};
window.__cmpReady = Promise.resolve();
