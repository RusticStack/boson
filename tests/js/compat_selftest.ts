// Runs inside a real WebView2 page in boson_porting_tests (bundled with bun build --format=iife).
// Exercises the Electron / Tauri v2 / Wails v3 shims against the C++ ElectronIpc + Math controllers, and the
// generated Wails bindings fixture against pt::TabService / pt::FakeWindowController.
import { ipcRenderer, contextBridge } from '../../frontend/src/compat/electron';
import * as tauri from '../../frontend/src/compat/tauri';
import { boson } from '../../frontend/src/boson';
import {
  Application,
  Call,
  CancelError,
  CancellablePromise,
  Create,
  Events,
  Window as WailsWindow,
  bindService,
  bindingId,
  defineBinding,
  registerBindingIds,
  registerBindingPackage,
  registerBindingParams,
  type MethodParams,
} from '../../frontend/src/compat/wails';
// Real `wails3 generate bindings -ts` output; it imports '@wailsio/runtime', aliased in tsconfig.json "paths"
// to compat/wails.ts (the same alias as frontend/vite.config.ts).
import { Tab, TabService } from './fixtures/wails/bindings/wailsfixture';
import './fixtures/wails/bindings/events';

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

async function wailsGenerated(): Promise<void> {
  const tsgen = (window as unknown as { __tsgenParams?: MethodParams }).__tsgenParams;
  expect(tsgen !== undefined && Array.isArray(tsgen['TabService.CreateTab']), 'wails tsgen method params injected');
  registerBindingParams(tsgen ?? {});

  // IDs: the literal below is what wails3 v3.1.0 wrote into tabservice.ts for main.TabService.CreateTab.
  expect(bindingId('main.TabService.CreateTab') === 714123261, 'wails FNV-1a method ID matches wails3 output');
  let unknownRejected = false;
  await Call.ByID(4161582358).catch(() => { unknownRejected = true; });
  expect(unknownRejected, 'wails Call.ByID with an unregistered ID rejects');
  registerBindingPackage('main');

  const created = TabService.CreateTab('https://example.com/a', 'A');
  expect(created instanceof CancellablePromise, 'wails generated call returns a CancellablePromise');
  const a = await created;
  expect(a instanceof Tab && a.id === 1 && a.url === 'https://example.com/a' && a.title === 'A', 'wails CreateTab(rawURL, title) by ID');
  expect(a !== null && a.opener === null && a.tags[0] === 'new' && a.meta.source === 'wails', 'wails model fields from C++');
  const b = await TabService.CreateTab('https://example.com/b', 'B');
  expect(b !== null && b.opener instanceof Tab && b.opener.id === 1, 'wails nested model via Create.Nullable(Tab.createFrom)');
  let emptyError = '';
  await TabService.CreateTab('', 'x').catch((e: Error) => { emptyError = e.message; });
  expect(emptyError.includes('empty url'), 'wails C++ exception rejects the generated call');
  const list = await TabService.ListTabs();
  expect(list.length === 2 && list.every((t) => t instanceof Tab), 'wails ListTabs via Create.Array(Tab.createFrom)');
  const renamed = await TabService.Rename(new Tab({ id: 1, url: 'u', title: 'old' }), 'A2');
  expect(renamed instanceof Tab && renamed.id === 1 && renamed.title === 'A2', 'wails Rename(model, string)');
  expect((await TabService.Resize(1, 800, 600)) && !(await TabService.Resize(1, 600, 800)), 'wails positional args keep their order');
  await TabService.SetMuted(2, true);
  expect((await TabService.ListTabs())[1].muted, 'wails SetMuted(id, muted) void method');
  let mutedRejected = false;
  await TabService.SetMuted(99, true).catch(() => { mutedRejected = true; });
  expect(mutedRejected, 'wails void method rejection');
  expect((await TabService.Version()) === '1.0', 'wails zero-arg method by ID');
  expect((await Call.ByName<string>('main.TabService.Version')) === '1.0', 'wails -names style Call.ByName');
  let tooMany = false;
  await Call.ByName('main.TabService.Version', 1).catch(() => { tooMany = true; });
  expect(tooMany, 'wails extra positional args reject');
  registerBindingIds({ 7: 'main.TabService.Version' });
  expect((await Call.Call<string>({ methodID: 7 })) === '1.0', 'wails registerBindingIds + Call.Call by ID');
  expect((await Call.ByName<{ sum: number }>('main.Math.add', { a: 2, b: 2 })).sum === 4, 'wails object arg still passes through');

  // Cancellation.
  const pending = TabService.Version();
  void pending.cancel();
  let cancelReason: unknown;
  await pending.catch((e: unknown) => { cancelReason = e; });
  expect(cancelReason instanceof CancelError, 'wails cancel() rejects with CancelError');
  const chained = TabService.ListTabs();
  void chained.cancel('stop');
  let chainedReason: unknown;
  await chained.catch((e: unknown) => { chainedReason = e; });
  expect(chainedReason instanceof CancelError && (chainedReason as CancelError).cause === 'stop', 'wails cancel() of a chained call');
  const resolvers = CancellablePromise.withResolvers<number>();
  let cancelledHits = 0;
  resolvers.oncancelled = () => { cancelledHits += 1; };
  resolvers.promise.catch(() => {});
  await resolvers.promise.cancel();
  resolvers.resolve(5);
  await resolvers.promise.cancel();
  expect(cancelledHits === 1, 'wails withResolvers oncancelled runs once');
  expect((await CancellablePromise.sleep(1, 'v')) === 'v', 'wails CancellablePromise.sleep');

  // Create.
  expect(Array.isArray(Create.Array(Create.Any)(null)) && Object.keys(Create.Map(Create.Any, Create.Any)(null)).length === 0, 'wails Create.Array / Map null');
  expect(Create.Nullable(Create.Any) === Create.Any && Create.ByteSlice(null) === '', 'wails Create.Nullable / ByteSlice');
  expect(Create.DateFromTime('2020-01-02T03:04:05Z').getUTCFullYear() === 2020, 'wails Create.DateFromTime');
  expect(Object.isFrozen(Create.Events), 'wails generated events module froze Create.Events');

  // Window / Application against pt::FakeWindowController ("Boson", same method names as SystemController).
  await WailsWindow.SetTitle('Wails title');
  await WailsWindow.Minimise();
  expect(await WailsWindow.IsMinimised(), 'wails Window.Minimise');
  await WailsWindow.Restore();
  expect(!(await WailsWindow.IsMinimised()), 'wails Window.Restore from minimized');
  await WailsWindow.Maximise();
  await WailsWindow.Maximise();
  expect(await WailsWindow.IsMaximised(), 'wails Window.Maximise is idempotent');
  await WailsWindow.UnMaximise();
  await WailsWindow.UnMaximise();
  expect(!(await WailsWindow.IsMaximised()), 'wails Window.UnMaximise is idempotent');
  await WailsWindow.ToggleMaximise();
  expect(await WailsWindow.Get('main').IsMaximised(), 'wails Window.ToggleMaximise');
  await WailsWindow.Restore();
  await WailsWindow.Fullscreen();
  expect(await WailsWindow.IsFullscreen(), 'wails Window.Fullscreen');
  await WailsWindow.Restore();
  await WailsWindow.Close();
  await Application.Quit();
  const calls = await boson.call<string[]>('Boson.calls');
  expect(
    calls.join(',') ===
      'set_title:Wails title,minimize,restore,maximize_or_restore,maximize_or_restore,maximize_or_restore,restore,set_fullscreen:1,set_fullscreen:0,close,quit',
    'wails Window / Application call sequence: ' + calls.join(',')
  );
}

(async () => {
  try {
    await electron();
    await tauriShim();
    await wails();
    await wailsGenerated();
  } catch (err) {
    result.failures.push('exception: ' + String(err instanceof Error ? err.message : err));
  }
  result.done = true;
})();
