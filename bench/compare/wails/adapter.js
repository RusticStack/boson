// Wails v3 adapter: the runtime is served by the asset server at /wails/runtime.js (Call.ByName / Events.On).
(() => {
  const methods = { config: 'Config', ping: 'Ping', echo: 'Echo', items: 'Items', open_window: 'OpenWindow', mark: 'Mark', report: 'Report', burst: 'Burst' };
  window.__cmpReady = import('/wails/runtime.js').then(({ Call, Events }) => {
    window.__cmp = {
      invoke: (name, arg) => Call.ByName('main.CompareService.' + methods[name], ...(arg === undefined ? [] : [arg])),
      listen: async (event, cb) => { Events.On(event, (e) => cb(e.data)); },
    };
  });
})();
