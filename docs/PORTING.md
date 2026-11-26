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

`ipcRenderer`: `invoke, send, sendSync, sendToHost, on, once, addListener, removeListener, off, removeAllListeners,
listenerCount, eventNames`. `contextBridge.exposeInMainWorld` defines a frozen, non-writable `window[key]` — Boson
pages have one JS world (WebView2 has no preload isolated world), so there is no isolation boundary: page scripts
share the world with "preload" code and functions are passed by reference, not proxied. Arguments are JSON, not
structured clone.

#### Part 2: `sendSync`, `webUtils`, `webFrame`, `process`, `clipboard`, `startDrag`

These need a `boson::ipc::SyncBridge` attached to the WebView (see [SYNC_IPC.md](SYNC_IPC.md) for the threading
cost) and, for `startDrag`, a `DragController` in the Router:

```cpp
boson::compat::ElectronIpc ipc;                         // ElectronIpc.sendSync is sync-enabled by the class
boson::transfer::DragOut drag(marshal, window.hwnd());
boson::ipc::DragController drag_controller(drag, &bridge);
boson::ipc::ClipboardController clipboard;
boson::ipc::Router<boson::compat::ElectronIpc, boson::ipc::ClipboardController, boson::ipc::DragController /*, ...*/>
    router(ipc, clipboard, drag_controller);
boson::ipc::SyncBridge sync(router, {"Clipboard.*"});   // extra patterns for controllers without boson_sync_methods()
sync.attach(webview);                                     // after the WebView is ready, and again after Environment::recreate

ipc.handle_sync("get-settings-sync", [&](const boson::compat::IpcEvent&, auto) { return settings_json; }); // UI thread!

boson::ipc::BootValues boot;                              // answers zero-argument sendSync with no IPC at all
boot.set_send_sync("get-app-path", app_path_utf8);
boot.set_process_info("1.4.0");
webview_config.scripts_on_document_created.push_back(boot.wide_script()); // before the WebView is created
```

| Electron (renderer) | Boson shim | Native side |
|---|---|---|
| `ipcRenderer.sendSync(ch)` (no args) | frozen boot value `__BOSON_BOOT__.sendSync[ch]`, returned as a copy | `BootValues::set_send_sync` |
| `ipcRenderer.sendSync(ch, ...args)` | `boson.invokeSync('ElectronIpc.sendSync', {channel, args})` | `ElectronIpc::handle_sync` (UI thread) |
| `contextBridge.exposeInMainWorld` | frozen `window[key]` | — |
| `webUtils.getPathForFile(file)` | `postMessageWithAdditionalObjects` + `SyncHost.file_paths`; `''` for in-memory Files | `ICoreWebView2File::get_Path` |
| `webFrame.get/setZoomFactor`, `get/setZoomLevel` | `SyncHost.zoom_factor` / `set_zoom_factor` (level: factor = 1.2^level) | `ICoreWebView2Controller` ZoomFactor |
| `process.platform/arch/versions/env/type` | frozen stub (`process` export, `installProcessGlobal()`); `versions.chrome/webview2/app`, no `electron`/`node` | `BootValues::set_process_info` |
| `clipboard.readText/writeText/readHTML/writeHTML/write/clear/availableFormats/has` | `boson.invokeSync('Clipboard.*')` | `ClipboardController` (sync-enabled by pattern) |
| `webContents.startDrag({file, files, icon})` | `startDrag(item)` from a `dragstart` handler (renderer side) | `DragController` → `transfer::DragOut` |

In Electron `startDrag` is a main-process API called on `event.sender`; with Boson the renderer calls
`startDrag(item)` (or `enableDragOut(el, getFiles)` from `frontend/src/dragout.ts`) and C++ code can call
`DragOut::start` directly. `icon` is an image file path (no `NativeImage`). Clipboard `readImage` / `writeImage`
are not shimmed synchronously: use `boson.call('Clipboard.read_image')`.

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

`Call.ByName / ByID / Call`, `CancellablePromise`, `Create`, `Events.On / Once / OnMultiple / Off / OffAll / Emit`,
`Window`, `Application.Quit`; `bindService` builds a service object by hand.

#### Running `wails3 generate bindings` output unchanged

The files Wails writes to `frontend/bindings/` import `Call`, `CancellablePromise` and `Create` from
`@wailsio/runtime` and call methods by numeric ID. With the alias above (`frontend/vite.config.ts` and
`frontend/tsconfig.json` already carry it) they build and run as generated:

```ts
import { registerBindingPackage } from '@wailsio/runtime';      // Boson-only export of the shim
import { TabService, Tab } from './bindings/myapp';              // generated, not edited
registerBindingPackage('main');                                   // once, before the first call
const tab: Tab | null = await TabService.CreateTab('https://example.com', 'Example');
```

```cpp
// C++ side: controller name = Go service name, glz::meta key = Go method name, one request struct whose
// members are the Go parameters in order (their names do not have to match the Go names).
struct CreateTabRequest { std::string raw_url; std::string title; };
class TabService {
public:
    BOSON_CONTROLLER("TabService");
    std::optional<Tab> create_tab(CreateTabRequest req);   // throw -> the promise rejects with the message
};
template <> struct glz::meta<TabService> {
    using T = TabService;
    static constexpr auto value = glz::object("CreateTab", &T::create_tab);
};
```

- **Method IDs.** Wails' ID is the 32-bit FNV-1a hash of the UTF-8 bytes of `"pkg.Service.Method"` (`internal/hash/fnv.go`
  in wails v3.1.0, used by both the generator and `pkg/application/bindings.go`); `pkg` is `main` for package main,
  else the Go import path. `bindingId(name)` computes it. Register names once:
  `registerBindingPackage(pkg)` hashes `pkg.Controller.method` for every method in `boson-method-params.ts`
  (plus any `registerBindingParams`), `registerBindingNames([...])` takes full names (e.g. the strings in
  `wails3 generate bindings -names` output), `registerBindingIds({ id: name })` covers `//wails:id` overrides.
  An unregistered ID rejects with code -32601.
- **Positional arguments.** `boson_tsgen <bindings.d.ts> <boson-method-params.ts>` writes the request struct's
  reflected member names for every method (`TsGenerator::generate_method_params<Controllers...>()`); the shim
  loads `generated/boson-method-params.ts` and sends `Method(a, b)` as `{ [names[0]]: a, [names[1]]: b }`.
  A single object argument that already is the request (Boson style) is passed unchanged; more arguments than
  names reject with -32602. For controllers not in your tsgen run, call `registerBindingParams({...})` with
  `TsGenerator::method_params_json<...>()`, or `defineBinding(name, { params })`.
- **`CancellablePromise`** is a Promise subclass; `then / catch / finally` keep the class, `cancel(cause)` rejects
  a pending promise with `CancelError` (`cause` set), runs `oncancelled` and cancels the promise it was chained
  from; also `cancelOn(signal)`, `withResolvers()`, `CancellablePromise.cancel / timeout / sleep`. Boson has no
  cancel message: the C++ handler still runs to completion and its late response is dropped.
- **`Create`**: `Any, ByteSlice, Array, Map, Nullable, Struct, DateFromTime, Events`, same behaviour as the runtime's.
- **`Window`** (the current window; `Get(name)` returns the same object) maps onto `SystemController`:
  `Minimise` -> `minimize`, `Maximise / UnMaximise` -> `is_maximized` then `maximize_or_restore` when needed,
  `ToggleMaximise` -> `maximize_or_restore`, `Restore` -> `set_fullscreen(false)` when fullscreen, else `restore`,
  `UnMinimise` -> `restore`, `IsMaximised / IsMinimised / IsFullscreen`, `Fullscreen / UnFullscreen / ToggleFullscreen`,
  `Close`, `SetTitle`, `Center`, `SetAlwaysOnTop`, `SetMinSize`, `SetZoom / ZoomReset`, `Reload / ForceReload`,
  `OpenDevTools`. Other Wails window methods (position, size, hide/show, max size, frameless...) are not provided,
  so a port that uses them fails to type-check instead of silently doing nothing.
- **`Application.Quit`** -> `SystemController::quit`: runs `set_quit_handler(fn)` on the UI thread (e.g.
  `[&] { boson::app::quit({env}); }`), or closes the window when no handler is set.
- `SystemController` gained `restore`, `is_minimized`, `is_fullscreen`, `quit` and `set_quit_handler` for this.

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
    WebView against the C++ side (skipped automatically when bun is not installed).
- `boson_tests` still prints "All Boson tests passed."; `boson_integration_tests` 108/108 (Router and Bridge were touched).
- `tsc --noEmit` passes for `frontend/` (including `src/compat/*`) and for the self-test file. The TypeScript
  compiler used was the one already installed in the main checkout's `frontend/node_modules`; this worktree has no
  `node_modules` and nothing was downloaded.

Not verified: real Electron / Tauri / Wails applications were not ported end to end (none is installed here);
the shims were checked against the documented API shapes, not against those frameworks' own test suites.

### Verification: Wails generated bindings (issue #16)

Run on 2026-11-26, same machine, MSVC Debug /W4 (zero warnings), wails3 v3.1.0, Go 1.27.4, bun 1.4.4, TypeScript 5.9.3:

- **Fixture is real generator output.** A throwaway Wails v3 project outside the repo (package main, one
  `TabService` with a `Tab` model holding `[]string`, `map[string]string` and `*Tab`, and methods with 0, 2 and 3
  positional args, a model argument, a void and an error result) was run through `wails3 generate bindings -ts`;
  `tests/js/fixtures/wails/bindings/` is that output, unchanged; the Go source is `tests/js/fixtures/wails/tabs.go.txt`.
- **ID algorithm** read from the wails v3.1.0 module source (`internal/hash/fnv.go`: `fnv.New32a` over `pkg.Type.Method`,
  `internal/generator/collect/service.go` and `pkg/application/bindings.go`). All six IDs in the fixture equal
  `bindingId('main.TabService.<Method>')`; the self-test asserts the `CreateTab` literal.
- `boson_porting_tests`: **132/132 checks passed**, of which **60/60 JS assertions** in the WebView. New: the fixture's
  generated functions called by ID against `pt::TabService` (model instances via `createFrom`, nested
  `Create.Nullable(Tab.createFrom)`, `Create.Array`, argument order, void and throwing methods, unregistered IDs,
  extra args, `-names` style `Call.ByName`, `registerBindingIds`), cancellation (direct, chained, `withResolvers`),
  `Create` helpers, and `Window` / `Application.Quit` against `pt::FakeWindowController`, a recording controller with
  `SystemController`'s method names, so the test never moves or focuses a real window. A headless check asserts that
  every method the fake exposes exists on the real `SystemController` with the same parameter names, and checks
  `TsGenerator::method_params_json` output exactly. The positional names the page uses come from
  `TsGenerator::method_params_json<pt::TabService>()` injected at run time.
- `boson_compat_typecheck` (new ctest): `tsc --noEmit` with `strict` / `noUnusedLocals` over the self-test and the
  fixture, with `@wailsio/runtime` resolved through `tests/js/tsconfig.json` "paths". `frontend`: `bun run build`
  (tsc + vite) passes with the new alias.
- **Vite alias.** The ctest bundle is built by `bun build` (alias from tsconfig "paths"). Separately, once, the same
  self-test was bundled by Vite 6.4.6 with `resolve.alias['@wailsio/runtime']` as in `frontend/vite.config.ts`, and
  that bundle also passed 60/60 in `boson_porting_tests`. That Vite run is not part of ctest.
- Full ctest: 13/13 passed.

Not verified: the real `SystemController` methods added here (`restore`, `is_minimized`, `is_fullscreen`, `quit`) were
compiled and their names / parameters checked, but not run against a live window (that would move and focus a window
during the test run). The Wails `Window` shim was therefore run against the fake controller only. Wails' event
payload typing (`Create.Events` filled by the generator for typed custom events) was only exercised as the empty map
the fixture produces. Under the real C++26 reflection path (`__cpp_impl_reflection`) method names come from
identifiers rather than `glz::meta` keys, so a `CreateTab` key needs `defineBinding` there; MSVC uses the
`glz::meta` path, which is the one tested.

### Verification: Electron shim part 2 (sync IPC, drag-out)

- `boson_porting_tests`: 181/181 checks; the shim self-test now runs **78** JS assertions (was 60) with a `SyncBridge`,
  boot values, an in-memory "Clipboard" controller and a `DragController` attached. See [SYNC_IPC.md](SYNC_IPC.md) for the
  sync-path tests (`boson_sync_tests`, 170/170), latency numbers and what was not verified (real mouse drags,
  `getPathForFile` with disk-backed files).
