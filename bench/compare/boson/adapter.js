// Boson adapter: window.chrome.webview.postMessage({id, method, params}) -> Router -> reply {id, status, data};
// events arrive as {type: 'event', event, data}. Parameters are the bare JSON values (a number / a string).
(() => {
  const host = window.chrome && window.chrome.webview;
  const pending = new Map();
  const listeners = new Map();
  let nextId = 1;
  host.addEventListener('message', (e) => {
    const d = e.data;
    if (d && d.type === 'event') {
      const cb = listeners.get(d.event);
      if (cb) cb(d.data);
      return;
    }
    const p = d && pending.get(d.id);
    if (!p) return;
    pending.delete(d.id);
    if (d.status === 'success') p.resolve(d.data); else p.reject(new Error(d.error ? d.error.message : 'IPC error'));
  });
  window.__cmp = {
    invoke: (name, arg) => new Promise((resolve, reject) => {
      const id = nextId++;
      pending.set(id, { resolve, reject });
      host.postMessage(arg === undefined ? { id, method: 'Compare.' + name } : { id, method: 'Compare.' + name, params: arg });
    }),
    listen: async (event, cb) => { listeners.set(event, cb); },
  };
  window.__cmpReady = Promise.resolve();
})();
