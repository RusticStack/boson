// Runs inside a real WebView2 page in boson_porting_tests (bundled with bun build --format=iife).
// Exercises the Electron / Tauri v2 / Wails v3 shims against the C++ ElectronIpc + Math controllers.
import { ipcRenderer, contextBridge } from '../../frontend/src/compat/electron';
import * as tauri from '../../frontend/src/compat/tauri';
import { Call, Events, bindService, defineBinding } from '../../frontend/src/compat/wails';

interface SelfTestResult {
  done: boolean;
  passed: number;
  failures: string[];
}

const result: SelfTestResult = { done: false, passed: 0, failures: [] };
(window as unknown as { __compat: SelfTestResult }).__compat = result;

function expect(cond: boolean, label: string): void {
  if (cond) {
    result.passed += 1;
  } else {
    result.failures.push(label);
  }
}

const tick = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));
async function waitFor(cond: () => boolean, ms = 3000): Promise<boolean> {
  const end = performance.now() + ms;
  while (!cond()) {
    if (performance.now() > end) return false;
    await tick(10);
  }
  return true;
}

async function electron(): Promise<void> {
  const sum = await ipcRenderer.invoke<{ total: number }>('sum', 1, 2, 3.5);
  expect(sum.total === 6.5, 'electron invoke sum');
  let rejected = '';
  await ipcRenderer.invoke('fail').catch((e: Error) => { rejected = e.message; });
  expect(rejected.includes('handler exploded'), 'electron invoke rejection carries the C++ message');

  const pongs: unknown[][] = [];
  const onPong = (_e: unknown, ...args: unknown[]) => { pongs.push(args); };
  ipcRenderer.on('pong', onPong);
  let onceCount = 0;
  ipcRenderer.once('pong', () => { onceCount += 1; });
  expect(ipcRenderer.listenerCount('pong') === 2, 'electron listenerCount');
  await ipcRenderer.invoke('whoami', 'a');
  await ipcRenderer.invoke('whoami', 'b');
  expect(await waitFor(() => pongs.length === 2), 'electron on() receives event.reply');
  expect(pongs[0][0] === 'a' && typeof pongs[0][1] === 'number', 'electron reply args spread');
  expect(onceCount === 1, 'electron once() fires once');
  ipcRenderer.removeListener('pong', onPong);
  await ipcRenderer.invoke('whoami', 'c');
  await tick(100);
  expect(pongs.length === 2 && ipcRenderer.listenerCount('pong') === 0, 'electron removeListener');

  ipcRenderer.send('fire', 1);
  ipcRenderer.on('x', () => {});
  ipcRenderer.removeAllListeners();
  expect(ipcRenderer.eventNames().length === 0, 'electron removeAllListeners');
  let threw = false;
  try { ipcRenderer.sendSync('nope'); } catch { threw = true; }
  expect(threw, 'electron sendSync throws');

  contextBridge.exposeInMainWorld('portingApi', { version: 1 });
  const api = (window as unknown as { portingApi: { version: number } }).portingApi;
  expect(api.version === 1 && Object.isFrozen(api), 'contextBridge.exposeInMainWorld');
}

async function tauriShim(): Promise<void> {
  const added = await tauri.invoke<{ sum: number }>('Math.add', { a: 1, b: 2 });
  expect(added.sum === 3, 'tauri invoke Controller.method');
  const greeting = await tauri.invoke<string>('greet', { name: 'Boson' });
  expect(greeting === 'Hello, Boson!', 'tauri invoke plain command via ElectronIpc');
  tauri.mapCommand('add_numbers', 'Math.add');
  expect((await tauri.invoke<{ sum: number }>('add_numbers', { a: 5, b: 6 })).sum === 11, 'tauri mapCommand');
  let pluginRejected = false;
  await tauri.invoke('plugin:fs|read').catch(() => { pluginRejected = true; });
  expect(pluginRejected, 'tauri unmapped plugin command rejects');

  const got: Array<{ event: string; payload: { v: number } }> = [];
  const unlisten = await tauri.listen<{ v: number }>('tauri:evt', (e) => { got.push(e); });
  let onceHits = 0;
  await tauri.once('tauri:evt', () => { onceHits += 1; });
  await tauri.emit('tauri:evt', { v: 1 });
  await tauri.emit('tauri:evt', { v: 2 });
  expect(await waitFor(() => got.length === 2), 'tauri listen receives emit (re-broadcast)');
  expect(got[0].event === 'tauri:evt' && got[0].payload.v === 1, 'tauri Event shape');
  expect(onceHits === 1, 'tauri once');
  unlisten();
  await tauri.emit('tauri:evt', { v: 3 });
  await tick(150);
  expect(got.length === 2, 'tauri unlisten');
}

async function wails(): Promise<void> {
  expect((await Call.ByName<{ sum: number }>('main.Math.add', { a: 2, b: 3 })).sum === 5, 'wails Call.ByName object arg');
  defineBinding('main.Math.Add', { method: 'Math.add', params: ['a', 'b'] });
  expect((await Call.ByName<{ sum: number }>('main.Math.Add', 4, 5)).sum === 9, 'wails positional params mapping');
  defineBinding('main.Greeter.Greet', { channel: 'greet' });
  expect((await Call.ByName<string>('main.Greeter.Greet', { name: 'W' })) === 'Hello, W!', 'wails channel route');
  let positionalRejected = false;
  await Call.ByName('main.Math.add', 1, 2).catch(() => { positionalRejected = true; });
  expect(positionalRejected, 'wails unmapped positional call rejects');
  const Math2 = bindService<{ add(req: { a: number; b: number }): Promise<{ sum: number }> }>('main.Math', ['add']);
  expect((await Math2.add({ a: 10, b: 1 })).sum === 11, 'wails bindService');
  expect((await Call.Call<{ sum: number }>({ methodName: 'main.Math.add', args: [{ a: 1, b: 1 }] })).sum === 2, 'wails Call.Call');

  const seen: number[] = [];
  Events.On<{ n: number }>('wails:evt', (e) => { seen.push(e.data.n); });
  let onceHits = 0;
  Events.Once('wails:evt', () => { onceHits += 1; });
  await Events.Emit('wails:evt', { n: 1 });
  await Events.Emit({ name: 'wails:evt', data: { n: 2 } });
  expect(await waitFor(() => seen.length === 2), 'wails Events.On receives Emit');
  expect(onceHits === 1, 'wails Events.Once');
  Events.Off('wails:evt');
  await Events.Emit('wails:evt', { n: 3 });
  await tick(150);
  expect(seen.length === 2, 'wails Events.Off');
}

(async () => {
  try {
    await electron();
    await tauriShim();
    await wails();
  } catch (err) {
    result.failures.push('exception: ' + String(err instanceof Error ? err.message : err));
  }
  result.done = true;
})();
