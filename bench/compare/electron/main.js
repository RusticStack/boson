// Electron reference app of the comparison suite (bench/compare). Same commands and timeline as the other frameworks.
'use strict';
const { app, BrowserWindow, ipcMain, Menu } = require('electron');
const fs = require('node:fs');
const path = require('node:path');

const dir = process.env.COMPARE_DIR || '';
const config = process.env.COMPARE_CONFIG || '{}';

function writeAtomically(name, text) {
  if (!dir) return;
  const target = path.join(dir, name);
  const tmp = target + '.tmp';
  fs.writeFileSync(tmp, text);
  fs.renameSync(tmp, target);
}

function log(line) {
  if (dir) fs.appendFileSync(path.join(dir, 'electron.log'), new Date().toISOString() + ' ' + line + '\n');
}

function createWindow(second) {
  const win = new BrowserWindow({
    width: 900,
    height: 640,
    title: second ? 'Comparison app (2)' : 'Comparison app',
    backgroundColor: '#1b1b1f',
    webPreferences: { preload: path.join(__dirname, 'preload.js') },
  });
  win.webContents.on('console-message', (event) => log('console[' + event.level + '] ' + event.message));
  win.webContents.on('did-fail-load', (_e, code, desc) => log('did-fail-load ' + code + ' ' + desc));
  win.webContents.on('render-process-gone', (_e, details) => log('render-process-gone ' + details.reason));
  win.webContents.on('did-finish-load', () => log((second ? 'window 2' : 'window 1') + ' did-finish-load'));
  win.loadFile(path.join(__dirname, 'index.html'), second ? { query: { w: '2' } } : undefined);
  return win;
}

ipcMain.handle('cmp:config', () => config);
ipcMain.handle('cmp:ping', (_e, seq) => seq);
ipcMain.handle('cmp:echo', (_e, text) => text);
ipcMain.handle('cmp:items', () => {
  const items = new Array(10000);
  for (let i = 0; i < 10000; i++) items[i] = { id: i, name: 'item-' + i, value: i * 0.5, active: (i & 1) === 0 };
  return items;
});
ipcMain.handle('cmp:burst', (e, n) => {
  const contents = e.sender; // the window that asked, like Boson/Tauri emit_to("main")
  for (let i = 0; i < n; i++) contents.send('cmp:burst', i);
});
ipcMain.handle('cmp:open_window', () => { createWindow(true); });
ipcMain.handle('cmp:mark', (_e, text) => {
  const space = text.indexOf(' ');
  const name = text.slice(0, space);
  if (space > 0 && /^[a-z0-9_]{1,32}$/.test(name)) writeAtomically(name + '.mark', text.slice(space + 1));
});
ipcMain.handle('cmp:report', (_e, json) => { writeAtomically('result.json', json); writeAtomically('done.mark', '1'); });

app.on('window-all-closed', () => app.quit());
Menu.setApplicationMenu(null);
app.whenReady().then(() => { createWindow(false); });
