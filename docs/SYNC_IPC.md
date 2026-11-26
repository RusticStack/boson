# Synchronous IPC, boot values and native drag-out

Electron apps call `ipcRenderer.sendSync` (Orca: 3 call sites, T3 Code: 9) and `webContents.startDrag`. WebView2's
`postMessage` is asynchronous only, so Boson offers three tools, in order of preference:

| Need | Use | Cost per read / call |
|---|---|---|
| A value known before the page loads (config, paths, platform, feature flags) | `ipc::BootValues` → frozen `window.__BOSON_BOOT__` | a JS property read (5–11 ns), no IPC |
| A value only C++ can compute, needed synchronously | `ipc::SyncBridge` → `boson.invokeSync()` | ~0.11 ms round trip; page JS blocked; handler on the **UI thread** |
| Everything else | `Bridge` → `boson.call()` (async, pool thread) | ~0.16–0.19 ms round trip, nothing blocks |

Files: `include/boson/ipc/sync_bridge.hpp`, `include/boson/ipc/sync_boot.hpp`, `src/ipc/sync_bridge.cpp`,
`src/ipc/sync_boot.cpp`, `include/boson/transfer/drag_out.hpp`, `src/transfer/drag_out.cpp`,
`include/boson/ipc/controllers/drag_controller.hpp`, `frontend/src/boson.ts` (`invokeSync`, `boot`, `bootValues`, `isSyncAvailable`),
`frontend/src/dragout.ts`, `frontend/src/compat/electron.ts`, `tests/test_sync.cpp`.

## Which threads block (measured, not assumed)

A sync call is `chrome.webview.hostObjects.sync.boson.invoke(method, paramsJson)`:

1. **The page's JS thread (renderer process) blocks** for the whole round trip. Verified: a `setTimeout(…, 0)`
   scheduled before a 30 ms handler has not fired when `invoke` returns, and the JS-measured duration is ≥ 30 ms.
2. The request travels renderer → browser process → host process. **The UI thread is not blocked while the call is in
   flight**; WebView2 delivers it like any other WebView2 callback, through the UI thread's message loop.
3. **The handler runs on the UI thread** that owns the WebView (verified: `GetCurrentThreadId()` inside the handler equals
   the test's UI thread; the same method called through `boson.call` runs on a pool thread). While the handler runs,
   the window does not paint or process input, and every other WebView waits too.

Rules that follow:

- Sync handlers must be cheap (microseconds). `SyncBridge` measures each handler; calls longer than `budget()`
  (default 2 ms) are counted in `stats().over_budget` and logged (`OutputDebugString` + `std::clog`).
- Never wait for the UI thread from a sync handler (`UiMarshal::invoke` / `WebViewController` methods marshal to the
  UI thread and wait: **deadlock**). Never wait for the calling page (`ExecuteScript` result, a web message): its JS thread
  is blocked: **deadlock**.
- Only methods explicitly enabled for sync are reachable (below); everything else answers `-32005`.
- Prefer `BootValues` for anything that can be computed before navigation.

## C++ API

```cpp
#include <boson/boson.hpp>

class SettingsController {
public:
    BOSON_CONTROLLER("Settings");
    // Opt methods into the sync path ("*" = all). Without this, use SyncBridge's extra patterns.
    static constexpr std::array<std::string_view, 1> boson_sync_methods() noexcept { return {"get"}; }
    Settings get() const;          // cheap: reads a cached struct
    void save(Settings s);         // async only (disk I/O)
};

boson::ipc::Router<SettingsController, boson::compat::ElectronIpc, boson::ipc::ClipboardController> router(settings, ipc, clipboard);
boson::ipc::Bridge bridge(runner, marshal, router);                  // async path, unchanged
boson::ipc::SyncBridge sync(router, {"Clipboard.read_text"});        // + patterns for controllers without boson_sync_methods()

bridge.attach(webview, {.allowed_origins = {}, .capabilities = policy});
sync.attach(webview, {.allowed_origins = {}, .capabilities = policy}); // same origin list + CapabilityPolicy semantics
```

- `SyncBridge(Router&, extra_patterns)` collects `"Controller.method"` patterns from every controller that has
  `boson_sync_methods()` (concept `ipc::SyncController`) and adds `extra_patterns` (`"X.method"`, `"X.*"`, `"X.pre*"`,
  compiled with the CapabilityPolicy matcher). `SyncBridge(DispatchFn, patterns)` takes any dispatcher.
- `attach(webview, SyncAttachOptions)`: the WebView must be ready. Adds the host object with `AddHostObjectToScript`
  (name `"boson"` by default) and returns false when `AreHostObjectsAllowed` is off. Host objects belong to the
  WebView2 controller: **call `attach` again after `Environment::recreate`**. `detach` removes it
  (`RemoveHostObjectFromScript`; verified: the page's next call throws).
- Every call goes through `invoke_checked`: origin check → method-name validation → sync allow-list → CapabilityPolicy →
  `SenderScope` (so `current_sender_id()` is the calling WebView, as on the async path) → `Router::dispatch_sync`. It never
  throws; the IDispatch layer never lets an exception cross the COM boundary (`E_FAIL`/`E_OUTOFMEMORY` at worst).
- With a default-deny CapabilityPolicy, sync methods need an allow entry like any other (`"SyncHost.*"` for the
  `webFrame` / `webUtils` shims, `"ElectronIpc.sendSync"`, `"Clipboard.*"`); `SyncHost.*` is sync-enabled but not exempt.
- Origin: host objects are exposed to the top-level document only, so the WebView's current `Source` is the caller's
  URI (read at call time). Default allowed origins: `boson://app` + the `dev_url` origin, exactly like `Bridge`.
- UI thread only; no threads, timers or polling. Idle cost: one web-message observer per attached WebView doing an O(1)
  prefix test (`{"__boson_sync":`) on each message.

### `ElectronIpc::handle_sync` (Electron's `ipcMain.on` + `event.returnValue`)

```cpp
ipc.handle_sync("get-window-id", [](const boson::compat::IpcEvent& e, boson::compat::ElectronIpc::Json) {
    return boson::compat::ElectronIpc::Json(static_cast<double>(e.sender_id)); // UI thread: keep it tiny
});
```

Sync handlers live in their own table (`handle_sync`, `handle_sync_raw`, `remove_sync_handler`, `has_sync_handler`);
`invoke()` never sees them and vice versa. `ElectronIpc::boson_sync_methods()` enables only `ElectronIpc.sendSync`.

### `SyncHost` built-ins

Served by the `SyncBridge` itself for the calling WebView (`SyncHostController`, typed in the generated bindings).
Through the async path they throw ("only available through boson.invokeSync()").

| Method | Does |
|---|---|
| `SyncHost.zoom_factor` → `{value}` | `ICoreWebView2Controller::get_ZoomFactor` |
| `SyncHost.set_zoom_factor {value}` | `put_ZoomFactor` (0.25 – 5.0) |
| `SyncHost.file_paths {token}` → `{paths}` | paths of the File objects posted just before with `postMessageWithAdditionalObjects({__boson_sync:'files',token}, files)` |

### `BootValues`

```cpp
boson::ipc::BootValues boot;
boot.set("config", app_config);                       // any glaze-serializable value
boot.set_json("flags", R"({"beta":true})");          // pre-serialized (validated)
boot.set_send_sync("get-user-data-path", user_data);  // answers ipcRenderer.sendSync("get-user-data-path")
boot.set_process_info("1.4.0");                       // process.platform/arch/versions for the Electron shim
webview_config.scripts_on_document_created.push_back(boot.wide_script()); // survives Environment::recreate
// or, on a ready WebView: boot.install(webview, /*run_now=*/true); boot.uninstall(webview);
```

The script defines a deep-frozen, non-writable, non-configurable `window.__BOSON_BOOT__` before any page script (verified:
the page's first inline script sees it). It only does so when the document's origin is allowed (default `boson://app` +
`dev_url` origin; `set_allowed_origins`), so iframes / navigations to other origins never see the values (verified with a
`navigate_to_string` document). Values are serialized once, when the script is built.

## JavaScript

```ts
import { boson } from './boson';
const theme = boson.boot<{ theme: string }>('config')?.theme;           // no IPC
const s = boson.invokeSync<Settings>('Settings.get');                     // throws BosonIpcError
if (!boson.isSyncAvailable()) { /* no SyncBridge attached */ }
```

Error codes: `-32003` permission denied (untrusted origin or CapabilityPolicy), `-32005` method not enabled for
sync, `-32601` malformed / unknown method, `-32700` invalid params JSON, `-32000` handler exception, `-32001` (JS only) no
host object.

Electron shim (`frontend/src/compat/electron.ts`, details in [PORTING.md](PORTING.md)): `ipcRenderer.sendSync` (boot value
first for zero-argument calls, returned as a copy; else `ElectronIpc.sendSync`), `webUtils.getPathForFile`,
`webFrame.get/setZoomFactor/Level`, `process` stub + `installProcessGlobal()`, synchronous `clipboard`, `startDrag`.
`contextBridge.exposeInMainWorld` assigns a frozen global: WebView2 has no preload isolated world, so it is an API-shape
shim, not a security boundary.

`webUtils.getPathForFile(file)` posts the File with `postMessageWithAdditionalObjects` and immediately reads the path back
with `SyncHost.file_paths`. This relies on the async message being handled before the sync call that follows it;
WebView2 does not document that ordering, but it held in 200/200 consecutive attempts here (and across every test run).
If it ever did not, `file_paths` returns an error and `getPathForFile` throws instead of returning a wrong path. WebView2
refuses File objects that have no file on disk (built in script); the shim then returns `''` like Electron.

## Native drag-out (`webContents.startDrag`)

```cpp
boson::transfer::DragOut drag(marshal, window.hwnd());
boson::ipc::DragController drag_controller(drag, &bridge, scope /* optional FsScope: paths must be inside */);
// C++ apps can call it directly too:
drag.start({.files = {L"C:\out\report.pdf"}, .icon = L"C:\app\pdf.png"},
           [](const boson::transfer::DragOutResult& r) { /* UI thread: r.started, r.effect */ });
```

```ts
import { enableDragOut, startDrag } from './dragout';
enableDragOut(rowElement, () => [absolutePath]);          // dragstart -> preventDefault -> native drag
const r = await startDrag(paths, { icon: iconPath });      // resolves on drop/cancel: { effect: 'copy' | 'link' | 'move' | 'none' }
```

- Data object: `SHParseDisplayName` → `SHCreateShellItemArrayFromIDLists` → `BindToHandler(BHID_DataObject)` (the same
  object Explorer uses; falls back to `SHCreateDataObject` with absolute IDLists for items without a common parent), plus
  `CF_HDROP` if the shell did not add one, and `Preferred DropEffect = DROPEFFECT_COPY`. Verified formats: `CF_HDROP`
  (paths read back with `DragQueryFileW`, including non-ASCII names, folders and files from different folders), `Shell IDList Array`,
  `FileNameW`, `Preferred DropEffect`.
- Drag image: `icon` is decoded with WIC (PNG / ICO / BMP / JPEG; scaled to ≤ 256 px, premultiplied BGRA) and attached with
  `IDragSourceHelper::InitializeFromBitmap` (verified: `DragImageBits` appears in the data object). Without `icon`,
  `SHDoDragDrop` uses the shell's image for the items.
- Default effects copy + link: a drop never moves (deletes) the source unless `"move"` is requested.
- **Modal loop.** `SHDoDragDrop` / `DoDragDrop` runs a modal message loop on the UI thread until the button is released.
  That loop keeps dispatching messages (paint, WebView2 callbacks, UiMarshal tasks, timers, other sync calls), so the UI
  stays alive, but code after the call and the app's own message loop (accelerator translation, `IsDialogMessage`) resume
  only after the drop. Therefore `DragOut::start` never runs the loop inside the caller: it posts the drag through
  `UiMarshal` (so it runs from the top-level loop, not inside a WebView2 event or a sync host-object call, where it would
  also freeze the page) and reports through a callback; `Drag.start` returns at once and the outcome arrives as the
  `drag:ended` event (the frontend parks events that arrive before the `Drag.start` response). One drag at a time.
- A drag must start while a mouse button is down (from `dragstart` / `mousedown`). Otherwise `DoDragDrop` would drop
  immediately onto whatever is under the cursor, so `DragOut` refuses (`require_button_down`, default on) with
  "no mouse button is down". `OleInitialize` is called (balanced) around the drag; the UI thread must be STA.

## Performance (this machine, Release, 3 runs)

Windows 11 Pro 10.0.26300, WebView2 runtime 158.0.4370.33, MSVC Release (/O2), `boson_sync_tests`. JS timings use
`performance.now()`, which WebView2 coarsens to 0.1 ms, so p50 values are quantized; the means come from 2000 / 500 samples.

| Path | Result |
|---|---|
| C++ sync pipeline (origin + allow-list + CapabilityPolicy + request build + Router + glaze), 200k calls | 594–743 ns/call |
| Same through `IDispatch::Invoke` (UTF-16 ↔ UTF-8, BSTR) | 931–1125 ns/call |
| Sync cold: first `hostObjects.sync.boson` proxy / first `invoke` in a document | 1.3–1.4 ms / 0.6–0.7 ms |
| Sync hot `invoke('Math.add')`, JS round trip, 2000 calls | mean 0.109–0.128 ms, p95 0.2–0.3 ms, p99 0.4–0.5 ms |
| Sync hot `SyncHost.zoom_factor`, 500 calls | mean 0.100–0.128 ms |
| Async `boson.call` equivalent (postMessage → pool thread → postMessage), 500 sequential | mean 0.164–0.187 ms, p99 0.5–1.2 ms |
| Boot value read (`__BOSON_BOOT__.config.answer`), 100k reads | 5–11 ns |

Of the ~110 µs sync round trip, about 1 µs is spent on the UI thread (the C++ pipeline above plus the handler); the rest is
WebView2's cross-process transport, during which the UI thread is free. Debug builds: ~12 µs/call for the C++ pipeline.

## Verification

Verified on this machine (numbers above; MSVC Debug and Release, /W4, zero warnings in both):

- `boson_sync_tests`: **170/170 checks passed** (Debug and Release).
  - Headless: the IDispatch contract (GetIDsOfNames, Invoke with 1/2 arguments in reverse order, missing optional
    argument, type mismatch with `puArgErr`, bad argument count, property-get, named arguments, unknown DISPID, UTF-8/UTF-16
    round trip, a throwing handler → `E_FAIL`); allow-list from `boson_sync_methods()` and patterns (invalid pattern ignored);
    origin rejection; malformed names; invalid params; capability deny / not-granted on the sync path; handler exception;
    sender id and handler thread; `ElectronIpc.sendSync` / `handle_sync_raw` / separation from `invoke`; `SyncHost` refusing the
    async path; budget accounting; `BootValues` JSON, ordering, replacement, removal, reserved keys, origin list in the
    script; drag-out data object formats, multi-folder items, drag image (normal and scaled), invalid image, validation
    errors, button-up refusal, `DragOut::start` → UI-thread callback, `DragController` validation and `FsScope` containment.
  - Real WebView2: boot values present at first parse, deep-frozen, immutable, origin-gated; `install(run_now)`; cold/hot
    latency; async comparison; handler thread = UI thread (sync) vs pool thread (async); JS thread blocked; denials
    (`-32003` capability, `-32005` not enabled / unknown) on the sync path and the same policy on the async path; exception
    → error envelope, page keeps working; `ElectronIpc.sendSync` sender id; zoom set/get (also read back in C++) and range
    check; files-message ordering 200/200; WebView2 refusing an in-memory File; opaque-origin document rejected; `detach`.
- `boson_porting_tests`: 181/181, including the extended TypeScript shim self-test (**78** JS assertions, previously 60):
  `sendSync` (handler, boot fast path returning a copy, exception, missing handler), `invokeSync`, `-32005`, `boson.boot`,
  `webFrame` zoom factor/level, `webUtils` (in-memory File → `''`), `process` stub + `installProcessGlobal`, clipboard over an in-memory
  "Clipboard" controller with the real controller's method names and types, `startDrag` → `Drag.start` → `DragOut` → `drag:ended`
  (rejected because no button is down) and its path validation.
- `boson_tests`, `boson_integration_tests`, `boson_transfer_tests` still pass; `tsc --noEmit` passes for `frontend/` and
  `tests/js`; `bun run build` succeeds.

Not verified:

- **A real mouse drag** (no input was synthesized, to avoid taking the user's mouse): the actual `SHDoDragDrop` modal loop,
  the drop into Explorer / another app, the reported effect, the default shell drag image and the behaviour of the WebView's
  mouse capture when the OLE loop takes over.
- `webUtils.getPathForFile` with a disk-backed File (needs a user drop or file picker); only the message/sync plumbing and the
  in-memory case were exercised. The native side reuses `transfer::FileDrop::paths_from_objects`.
- The real `ClipboardController` through the sync path (the test uses a fake to avoid racing other processes on the shared
  clipboard); host-object behaviour in iframes and after `Environment::recreate` (documented: re-attach); sync calls from
  Web Workers (WebView2 host objects are not available there).
