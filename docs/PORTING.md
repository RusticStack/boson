# Porting helpers (Electron / Tauri v2 / Wails v3)

Boson ships thin compatibility layers so an existing Electron, Tauri or Wails frontend can be moved over
without rewriting every IPC call site first. They are deliberately small: they map *shapes*, they do not emulate
the other frameworks' runtimes. New code should use typed Boson controllers (`BOSON_CONTROLLER` + reflection);
the shims are a migration ramp. See [MIGRATION.md](MIGRATION.md) for the concept-by-concept mapping tables and
[BENCHMARKS.md](BENCHMARKS.md) for the benchmark harness.

## C++: `boson::compat::ElectronIpc` (include/boson/compat/electron_ipc.hpp)

`ElectronIpc` is a **dynamic controller** named `"ElectronIpc"`. Put it in the same `Router` as your static
controllers. The Router still parses the envelope, runs the call on the `TaskRunner` pool, catches every
exception and sends the reply to the WebView that made the request; static controllers keep their
zero-overhead reflected dispatch (the dynamic path is selected per controller with `if constexpr`).

```cpp
#include <boson/boson.hpp>

boson::compat::ElectronIpc ipc;                       // ipcMain
boson::ipc::Router<boson::ipc::SystemController, boson::compat::ElectronIpc> router(system_controller, ipc);
boson::ipc::Bridge bridge(runner, marshal, router);
bridge.attach(webview);
ipc.set_bridge(&bridge);                              // needed for send()/broadcast()/event.reply()

using Json = boson::compat::ElectronIpc::Json;        // glz::generic

// ipcMain.handle('read-config', async (event, name) => ...)
ipc.handle("read-config", [](const boson::compat::IpcEvent& e, Json args) {
    Json out;
    out["name"] = args[0].get_string();
    out["sender"] = static_cast<double>(e.sender_id);
    return out;                                      // throw -> rejected Promise in JS
});

// Zero-parse variant: raw JSON array in, JSON text out (validated before it reaches the page).
ipc.handle_raw("ping", [](const boson::compat::IpcEvent&, std::string_view args_json) {
    return std::string{args_json};
});

// ipcMain.on('log', (event, ...args) => ...) + event.reply()
ipc.on("log", [](const boson::compat::IpcEvent& e, const Json& args) {
    e.reply("log-ack", args.get_array().size());     // only to the sending WebView
});

// webContents.send(channel, ...args) / broadcast to every attached WebView
ipc.send(webview.id(), "progress", 42, std::string{"step"});
ipc.broadcast("theme-changed", std::string{"dark"});
```

| Electron (main) | `ElectronIpc` |
|---|---|
| `ipcMain.handle(ch, fn)` | `handle(ch, Handler)` / `handle_raw(ch, RawHandler)` (false if `ch` already has a handler) |
| `ipcMain.handleOnce` | `handle_once` |
| `ipcMain.removeHandler` | `remove_handler`, `has_handler` |
| `ipcMain.on / once` | `on / once` (returns a `ListenerId`) |
| `ipcMain.removeListener / removeAllListeners` | `remove_listener(id)`, `remove_all_listeners(ch = {})`, `listener_count` |
| `event.sender.id`, `event.reply` | `IpcEvent::sender_id` (= `WebView::id()`), `IpcEvent::reply(ch, args...)` |
| `webContents.send` | `send(webview_id, ch, args...)`, `broadcast(ch, args...)`, `send_json` |
| (Tauri/Wails frontend `emit`) | `on_event(name, Listener)` |

Wire protocol (what the TypeScript shims send):

| JS call | Request | Reply / event |
|---|---|---|
| invoke | `ElectronIpc.invoke {channel, args: [...]}` | handler return value; unknown channel -> error -32601, bad params -32602, throw -> -32000 |
| send | `ElectronIpc.send {channel, args: [...]}` | `null` after every listener ran |
| emit | `ElectronIpc.emit {event, payload, broadcast = true}` | `on_event` listeners get `[payload]`; with broadcast the Bridge re-emits `event` to every attached WebView |
| C++ -> JS | — | Bridge event `"ipc:<channel>"`, data = JSON array of the arguments |

Threading: handlers and listeners run on a TaskRunner pool thread, never on the UI thread; every member
function is thread-safe (shared mutex; handlers are invoked outside the lock, so a handler may register or
remove handlers). Listener exceptions are caught and logged with `OutputDebugStringA`; handler exceptions
become the rejection message (`Error invoking remote method '<ch>': <what>`, like Electron).

### Sender context (include/boson/ipc/request_context.hpp)

`boson::ipc::current_sender_id()` returns the `WebView::id()` of the WebView whose request is being served
on the current thread (0 outside a Bridge request). `Bridge` sets it with `SenderScope` before dispatching and
`Router::dispatch_async` carries it to the pool thread. Any controller — not only `ElectronIpc` — may use it to
reply to the sender with `Bridge::emit_to(current_sender_id(), ...)`.

### Writing your own dynamic controller

Any type satisfying `boson::ipc::DynamicController` (a `BOSON_CONTROLLER` with
`bool boson_dispatch(std::string_view method, std::uint64_t id, std::string_view params_json, std::string& out) noexcept`)
is routed by name at runtime. Return `false` for unknown methods (the Router answers "Method not found") and
always write a full envelope with `make_success_envelope` / `make_error_envelope`.

## TypeScript shims (frontend/src/compat/)

All three only import from `../boson`; alias them in Vite (`resolve.alias`) to keep the original import paths:

```ts
// vite.config.ts
resolve: { alias: {
  electron: '/src/compat/electron.ts',
  '@tauri-apps/api/core': '/src/compat/tauri.ts',
  '@tauri-apps/api/event': '/src/compat/tauri.ts',
  '@wailsio/runtime': '/src/compat/wails.ts',
} }
```

### electron.ts

```ts
import { ipcRenderer, contextBridge } from './compat/electron';
const cfg = await ipcRenderer.invoke('read-config', 'main');   // ipc.handle("read-config")
ipcRenderer.send('log', 'hello');                               // ipc.on("log")
ipcRenderer.on('log-ack', (_event, count) => console.log(count)); // e.reply("log-ack", ...)
contextBridge.exposeInMainWorld('api', { readConfig: (n: string) => ipcRenderer.invoke('read-config', n) });
```

`ipcRenderer`: `invoke, send, sendToHost, on, once, addListener, removeListener, off, removeAllListeners,
listenerCount, eventNames`; `sendSync` throws (the bridge is asynchronous by design). `contextBridge.exposeInMainWorld`
defines a frozen, non-writable `window[key]` — Boson pages have one JS world, so there is no isolation boundary.
Arguments are JSON, not structured clone.

### tauri.ts

```ts
import { invoke, listen, emit, mapCommand } from './compat/tauri';
await invoke('Boson.set_title', { title: 'Hi' });   // a "Controller.method" goes straight to the Router
await invoke('greet', { name: 'Boson' });          // ElectronIpc.invoke {channel:'greet', args:[{name}]}
mapCommand('save_file', 'Files.save');             // route one command to a static controller
const unlisten = await listen<{ v: number }>('progress', (e) => console.log(e.payload.v)); // Bridge::emit("progress", ...)
await emit('ui-ready', { at: Date.now() });        // ElectronIpc::on_event("ui-ready") + re-broadcast
```

Exports `invoke, isTauri (false), mapCommand, listen, once, emit, emitTo` and the `Event<T>` / `UnlistenFn` /
`EventTarget` types. `emitTo` cannot address window labels and behaves like `emit`; binary invoke payloads and
unmapped `plugin:*` commands reject.

### wails.ts

```ts
import { Call, Events, bindService, defineBinding } from './compat/wails';
await Call.ByName('main.Math.add', { a: 1, b: 2 });             // -> Boson "Math.add" {a,b}
defineBinding('main.Math.Add', { method: 'Math.add', params: ['a', 'b'] });
await Call.ByName('main.Math.Add', 1, 2);                        // positional -> {a:1,b:2}
defineBinding('main.Greeter.Greet', { channel: 'greet' });      // positional args to ElectronIpc::handle("greet")
const GreetService = bindService<{ Greet(r: { name: string }): Promise<string> }>('main.GreetService', ['Greet']);
const off = Events.On('tick', (e) => console.log(e.name, e.data));
await Events.Emit('ui-ready', { ok: true });
```

`Call.ByName / Call.Call` (by name), `Call.ByID` rejects (Wails' numeric IDs come from its generator);
`Events.On / Once / OnMultiple / Off / OffAll / Emit`; `bindService` mimics the generated `frontend/bindings` modules.

## Verification

Tested on this machine (Windows 11 Pro 10.0.26300, WebView2 runtime 158.0.4370.33, MSVC Debug, /W4, zero warnings):

- `boson_porting_tests`: **119/119 checks passed**.
  - Headless: ElectronIpc through a real `Router` next to a static controller (invoke, raw handlers and invalid-JSON
    rejection, handle_once, remove_handler, duplicate handlers, missing / non-array args, unknown methods, C++ and
    non-std exceptions -> error envelopes), send/on/once/remove listeners (throwing listener contained), frontend
    emit -> `on_event`, `encode_ipc_args`, `SenderScope` nesting, and the sender id being carried by
    `Router::dispatch_async` onto a pool thread.
  - Real WebView2 (two WebViews on one Bridge): invoke / error / send / static call from page A; the
    `event.reply` event reaches only A; `send(b.id())` reaches only B; `broadcast` reaches both; frontend emit
    reaches the C++ listener and is re-broadcast.
  - The three TypeScript shims, bundled with `bun build` (tests/js/compat_selftest.ts), run inside the real
    WebView against the C++ side: **27/27 JS assertions passed** (skipped automatically when bun is not installed).
- `boson_tests` still prints "All Boson tests passed."; `boson_integration_tests` 108/108 (Router and Bridge were touched).
- `tsc --noEmit` passes for `frontend/` (including `src/compat/*`) and for the self-test file. The TypeScript
  compiler used was the one already installed in the main checkout's `frontend/node_modules`; this worktree has no
  `node_modules` and nothing was downloaded.

Not verified: real Electron / Tauri / Wails applications were not ported end to end (none is installed here);
the shims were checked against the documented API shapes, not against those frameworks' own test suites.
