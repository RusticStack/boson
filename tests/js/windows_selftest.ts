// Window-manager shim self-test: runs in the "main" window of tests/test_windows.cpp against the real Window
// controller (app::WindowManager). Results land in window.__wst; C++ checks the cross-window side effects.
import { AppWindow, currentLabel, windows } from '../../frontend/src/windows';
import { BrowserWindow } from '../../frontend/src/compat/electron';
import { TauriWindow, WebviewWindow, emitTo as tauriEmitTo, getCurrentWindow, LogicalSize } from '../../frontend/src/compat/tauri';
import { Window as WailsWindow } from '../../frontend/src/compat/wails';

interface SelfTestResult {
  done: boolean;
  passed: number;
  failed: number;
  failures: string[];
}

declare global {
  interface Window {
    __wst?: SelfTestResult;
    /** Called by the C++ test once popout-electron's page has loaded (events sent earlier would be lost). */
    __wstElectronSend?: () => void;
  }
}

const result: SelfTestResult = { done: false, passed: 0, failed: 0, failures: [] };
window.__wst = result;
window.__wstElectronSend = () => BrowserWindow.fromLabel('popout-electron').webContents.send('chan', 1, 2);

function expect(condition: boolean, name: string): void {
  if (condition) {
    result.passed += 1;
  } else {
    result.failed += 1;
    result.failures.push(name);
  }
}

async function run(): Promise<void> {
  // windows.ts
  expect(currentLabel() === 'main', 'currentLabel() is the injected label');
  const all = await windows.list();
  expect(all.some((w) => w.label === 'main') && all.some((w) => w.label === 'dashboard'), 'windows.list()');
  expect((await AppWindow.current().info()).label === 'main', 'AppWindow.current().info()');
  const popout = await windows.create('popout-js', { url: 'popout.html', title: 'JS pop-out', width: 320, height: 220, focus: false });
  expect(popout.label === 'popout-js', 'windows.create()');
  expect((await windows.get('popout-js')) !== null && (await windows.get('missing')) === null, 'windows.get()');
  await popout.setTitle('renamed');
  expect((await popout.info()).title === 'renamed', 'AppWindow.setTitle');
  const resized = await popout.setSize(340, 230);
  expect(resized.width === 340 && resized.height === 230, 'AppWindow.setSize (DIPs)');
  let denied = false;
  await windows.create('not-allowed', {}).catch(() => { denied = true; });
  expect(denied, 'windows.create() outside the capability allow-list rejects');

  // Tauri
  expect(getCurrentWindow().label === 'main', 'tauri getCurrentWindow().label');
  expect((await TauriWindow.getByLabel('dashboard')) !== null, 'tauri Window.getByLabel');
  await tauriEmitTo('dashboard', 'tauri:to', { a: 1 });
  await tauriEmitTo({ kind: 'WebviewWindow', label: 'dashboard' }, 'tauri:to', { a: 2 });
  const created = await new Promise<boolean>((resolve) => {
    const w = new WebviewWindow('popout-tauri', { url: 'popout.html', width: 300, height: 200, focus: false });
    void w.once('tauri://created', () => resolve(true));
    void w.once('tauri://error', () => resolve(false));
  });
  expect(created, 'tauri new WebviewWindow emits tauri://created');
  const tw = new TauriWindow('popout-tauri');
  await tw.setSize(new LogicalSize(310, 210));
  const outer = await tw.outerSize();
  const scale = await tw.scaleFactor();
  expect(Math.abs(outer.width - 310 * scale) <= 2, 'tauri setSize / outerSize');
  const failed = await new Promise<boolean>((resolve) => {
    const w = new WebviewWindow('popout-tauri', { url: 'popout.html' }); // duplicate label
    void w.once('tauri://error', () => resolve(true));
    void w.once('tauri://created', () => resolve(false));
  });
  expect(failed, 'tauri duplicate label emits tauri://error');

  // Electron
  expect(BrowserWindow.getCurrentWindow().label === 'main', 'electron getCurrentWindow');
  const bw = new BrowserWindow({ label: 'popout-electron', url: 'popout.html', width: 300, height: 200, show: false });
  await bw.ready;
  expect(!(await bw.isVisible()), 'electron show:false');
  const bounds = await bw.getBounds();
  expect(bounds.width === 300 && bounds.height === 200, 'electron getBounds');
  expect((await BrowserWindow.getAllWindows()).some((w) => w.label === 'popout-electron'), 'electron getAllWindows');
  await bw.setTitle('electron title');
  expect((await bw.getTitle()) === 'electron title', 'electron setTitle / getTitle');

  // Wails
  expect((await WailsWindow.Name()) === 'main', 'wails Window.Name()');
  expect(WailsWindow.Get('main') === WailsWindow, 'wails Window.Get(current) is Window');
  await WailsWindow.Get('dashboard').SetTitle('wails title');
  expect(!(await WailsWindow.Get('dashboard').IsMaximised()), 'wails Window.Get(other).IsMaximised');

  // Cleanup of the JS-created windows (popout-electron stays for the C++ ipc:chan check).
  await popout.destroy();
  await new TauriWindow('popout-tauri').destroy();
  expect((await windows.get('popout-js')) === null, 'AppWindow.destroy');
}

run()
  .catch((err: unknown) => {
    result.failed += 1;
    result.failures.push('threw: ' + (err instanceof Error ? err.message : String(err)));
  })
  .finally(() => {
    result.done = true;
  });
