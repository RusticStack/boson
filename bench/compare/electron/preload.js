'use strict';
const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('__cmpBridge', {
  invoke: (name, arg) => ipcRenderer.invoke('cmp:' + name, arg),
  on: (event, cb) => { ipcRenderer.on('cmp:' + event, (_e, value) => cb(value)); },
});
