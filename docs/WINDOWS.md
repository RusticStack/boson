# Windows: multi-window apps by label

`boson::app::WindowManager` runs Electron `BrowserWindow` / Tauri `WebviewWindow`-style apps: an Orca-like main window plus a
dashboard pop-out and workspace windows, a modal settings dialog, a tray app whose window hides instead of closing.
Every window is addressed by a string **label**; every WebView of every window shares the app's single
`core::Environment` (one browser process tree).

| Layer | What | Where |
|---|---|---|
| C++ API | `app::WindowManager`, `app::ManagedWindow`, `app::CloseRequest` | `include/boson/app/window_manager.hpp`, `src/core/window_manager.cpp` |
| IPC | `Bridge::emit_to(label, ...)`, `Bridge::emit_json_to_window`, `BridgeAttachOptions::window_label` | `include/boson/ipc/bridge.hpp` |
| Controller | JS `Window.*` (`ipc::WindowController`), events `window:*`, `ipc::sender_resolver` for `Boson.*` | `include/boson/ipc/controllers/window_controller.hpp` |
| Security | `Capability::windows` (labels a page may create), URL allow-list | `include/boson/security/capabilities.hpp` |
| JS | `windows`, `AppWindow`, `currentLabel()` | `frontend/src/windows.ts` |
| Shims | Tauri `WebviewWindow` / `getCurrentWindow` / `getAllWindows` / `emitTo(label)`, Electron `BrowserWindow` proxy, Wails `Window.Get(name)` | `frontend/src/compat/{tauri,electron,wails}.ts` |
| Example | pop-out dashboard, modal settings, emit by label, close confirm, hide-on-close | `examples/windows/main.cpp` (`boson_windows_demo`) |
| Tests | `tests/test_windows.cpp` (`boson_window_tests`), `tests/js/windows_selftest.ts` | |

## C++

```cpp
#include <boson/boson.hpp>

boson::core::WebViewConfig page;                       // template for windows created from JS
page.settings.non_client_region_support_enabled = true;

boson::app::WindowManager wm({
    .instance = instance,
    .asset_lookup = [](std::string_view p) { return boson::assets::AssetRegistry::find(p); },
    .default_webview = page,
    .app_name = L"Orca",                               // state: %LOCALAPPDATA%\Orca\window-state-<label>.json
});

// Wake the UI thread through the manager's message-only window: it outlives every managed window, so
// marshaled work keeps flowing after the main window closes.
boson::exec::Scheduler scheduler;
boson::exec::UiMarshal marshal([&wm] { wm.wake_ui(); });
wm.set_invoke_callback([&marshal] { marshal.drain_on_ui_thread(); });
boson::exec::TaskRunner runner(scheduler, marshal);

// Controllers first, then Router and Bridge; set_bridge attaches every managed WebView with its label.
boson::ipc::WindowController windows(wm, marshal, {.policy = policy});          // JS "Window.*"
boson::ipc::SystemController system(marshal, boson::ipc::sender_resolver(wm));   // "Boson.*" -> calling window
system.set_quit_handler([&wm] { wm.quit(); });
boson::ipc::Router<boson::ipc::WindowController, boson::ipc::SystemController> router(windows, system);
boson::ipc::Bridge bridge(runner, marshal, router);   // declared after wm, so destroyed first
wm.set_bridge(&bridge);

boson::app::ManagedWindowConfig main;
main.label = "main";
main.window.title = L"Orca";
main.window.headless_titlebar = true;
main.window.backdrop = boson::core::BackdropType::Mica;
main.persist_state = true;
main.webview = page;
main.webview->initial_url = "boson://app/index.html";
auto created = wm.create(std::move(main));            // std::expected<ManagedWindow*, std::string>

boson::app::ManagedWindowConfig dash;
dash.label = "dashboard";
dash.webview = page;
dash.webview->initial_url = wm.resolve_url("dashboard.html"); // boson://app/... or the dev server
wm.create(std::move(dash));

bridge.emit_to(std::string_view{"dashboard"}, "metrics", payload); // only the dashboard's WebViews
bridge.emit("theme", theme);                                      // every window

return boson::core::Window::run_message_loop();       // last window closed -> app::quit({environment})
```

### ManagedWindowConfig

| Field | Effect |
|---|---|
| `label` | unique, 1-64 chars of `A-Z a-z 0-9 - _ : / .` |
| `window` | `core::WindowConfig`: title, size / min size (DIPs), `borderless` (frameless), `headless_titlebar`, `backdrop`, `always_on_top`, dark mode, corners. `owner` and `quit_on_destroy` are managed for you |
| `x`, `y`, `center` | outer position in physical px, or centered (on the owner when there is one) |
| `visible`, `focus` | create hidden; show without activating (`SW_SHOWNOACTIVATE`) |
| `skip_taskbar` | owned by an invisible top-level window, so no taskbar button; `set_skip_taskbar()` toggles at runtime (ITaskbarList) |
| `transparent` | `BackdropType::Transparent` + transparent WebView background |
| `parent` | owner label: stays above it, minimizes with it, is destroyed with it |
| `modal` | needs `parent`: owner `EnableWindow(FALSE)` while open (counted, so stacked modals work), centered on the owner, owner re-enabled *before* the modal is destroyed so activation returns to it |
| `persist_state` | `desktop::WindowStateKeeper` per label (`window-state-<label>.json` under `state_directory` or `%LOCALAPPDATA%\<app_name>`); applied on first show |
| `hide_on_close` | tray pattern: X / Alt+F4 / `close()` hide; `destroy()` and `quit()` close |
| `close_confirm_timeout` | close anyway when a deferred close is not answered in time (0 = wait forever) |
| `webview` | the main WebView (optional; `ManagedWindow::add_webview` adds more, all on the shared Environment) |
| `hosting` | windowed or DirectComposition `WebViewHost` |

### Close: veto, asynchronous confirm, sender

WM_CLOSE (X button, Alt+F4, `wm.close(label)`, `Window.close`) runs, in order: C++ handlers, the page's close guard,
`hide_on_close`, then destroys the window.

```cpp
wm.on_close_requested([&](boson::app::CloseRequest& req) {
    if (req.label() == "editor" && has_unsaved_changes()) {
        req.defer();                                   // keep it open; answer later
        ask_user_async([&wm, label = std::string(req.label()), id = req.id()](bool discard) {
            wm.resolve_close(label, id, discard);      // false = veto, true = close
        });
    }
});

// stdexec: completes with true when the window was destroyed, false when vetoed or hidden.
scope.spawn(wm.close_async("editor") | stdexec::then([](bool closed) noexcept { /* ... */ }));
```

`CloseRequest::prevent()` vetoes synchronously. While `quit()` runs, requests cannot be vetoed (`is_quitting()`). A second
WM_CLOSE while one is pending is ignored. Start `close_async` on the UI thread.

Page side (Electron `close` + `preventDefault`, Tauri `onCloseRequested`): `Window.set_close_guard({enabled: true})` makes every
close of that window emit `window:close-requested {label, request_id}` to it and wait for
`Window.confirm_close({request_id, allow})`. `AppWindow.onCloseRequested` in `windows.ts` does both.

### Quit on last window, hidden-to-tray

When the last window is gone the manager calls `on_last_window_closed` if set, else `app::quit({environment})` (waits for
the browser process, then `WM_QUIT`). `quit_on_last_window_closed = false` turns that off. Hidden windows still count as
open, so a tray app keeps running with its window hidden; `wm.show(label)` from the tray click brings it back.
`destroy_all()` never triggers the last-window action; `quit()` = `destroy_all()` + `app::quit`.

### Threads

Everything is UI-thread only except `wake_ui()`, `label_of_webview()` and `current_label()` (a shared-mutex registry of
WebView id -> label). A destroyed window is removed on the next loop turn (one message posted to the message-only
window), so destroying a window from inside its own callbacks is safe; `Closed` fires after removal.

## IPC routing

* **emit_to(label)**: `BridgeAttachOptions::window_label` (filled in by the manager) tags each attachment;
  `emit_to(std::string_view label, event, payload)` serializes once and posts only to that window's WebViews. The existing
  `emit_to(webview_id, ...)` and `emit(...)` are unchanged.
* **Sender window**: `ipc::current_sender_id()` (set by `SenderScope` in the Bridge, carried by Router to the pool thread)
  resolves to the calling window with `wm.current_label()` / `wm.find_by_webview(id)`. `SystemController` gained a resolver
  constructor; with `ipc::sender_resolver(wm)` every `Boson.*` call (minimize, set_title, dialogs owned by the window, zoom,
  devtools...) acts on the window whose page called it. The fixed `(window, webview, marshal)` form is kept.
* **Label in the page**: every managed WebView gets `window.__BOSON_WINDOW__ = {label}` before any page script, so
  `currentLabel()`, Tauri `getCurrentWindow()` and Electron `BrowserWindow.getCurrentWindow()` are synchronous.

### Window controller (JS `Window.*`)

`label: ''` means the calling window. Sizes and positions are DIPs of the window's monitor.

| Method | |
|---|---|
| `create(WindowCreateRequest)` | label, url, title, width/height, min sizes, x/y, center, frameless, headless_titlebar, backdrop, dark_mode, always_on_top, skip_taskbar, transparent, visible, focus, parent, modal (parent defaults to the caller), persist_state, hide_on_close -> `WindowInfo` |
| `list()`, `get({label})`, `current()` | `WindowInfo`: label, title, url, bounds, scale_factor, visible, focused, enabled, maximized, minimized, fullscreen, modal, parent, close_guard, webview_ids |
| `show({label, focus})`, `hide`, `focus`, `close`, `destroy`, `navigate({label, url})` | |
| `minimize`, `maximize`, `unmaximize`, `restore`, `center` | |
| `set_title`, `set_bounds({label, x?, y?, width?, height?})`, `set_min_size`, `set_always_on_top`, `set_fullscreen`, `set_skip_taskbar` | |
| `set_close_guard({label, enabled})`, `confirm_close({label, request_id, allow})` | page-side close confirmation |
| `emit({target, event, payload})` | target label, `'*'` (all) or `''` (self); `window:*` names are reserved |

Events (`WindowManagerEvents`, typed in `BosonEvents`): `window:created`, `window:closed`, `window:focus`, `window:blur`,
`window:maximized`, `window:minimized`, `window:restored`, `window:shown`, `window:hidden` (every window, `{label}`);
`window:moved`, `window:resized` (`{label, x, y, width, height, scale_factor}`, to the window itself unless
`broadcast_geometry`); `window:close-requested` (`{label, request_id}`, to the closing window only).

### Security

Window creation from JS is denied unless the policy grants the label:

```json
{ "default_deny": true, "capabilities": [
  { "identifier": "main-window", "origins": ["boson://app"],
    "allow": ["Window.*", "Boson.*"], "windows": ["dashboard", "workspace-*"] } ] }
```

* `Capability::windows`: label patterns (`"settings"`, `"workspace-*"`, `"*"`) a matching origin may create;
  `CapabilityPolicy::may_create_window(label, source_uri)`. `default_deny = false` never grants window creation. The caller's
  origin is its WebView's current document URL. Method-level access (`Window.create` at all) is the usual Bridge check.
* URLs: relative paths and `boson://app/...` (or the dev server's origin) are always allowed; any other absolute URL needs
  `WindowControllerOptions::allowed_url_origins` (`OriginPattern`s), and such windows are neither attached to the Bridge nor
  served `boson://app`. Same rule for `Window.navigate`.
* `max_windows` (default 32) bounds JS-created windows.

## JavaScript

```ts
import { windows, AppWindow } from './windows';

const dash = await windows.create('dashboard', { url: 'dashboard.html', width: 520, height: 420, persistState: true });
await dash.emit('metrics', { cpu: 3 });
await windows.emitTo('*', 'theme', { dark: true });
dash.on('closed', () => console.log('dashboard gone'));

const me = AppWindow.current();                       // sync: label injected
await me.onCloseRequested(async (req) => { if (!(await confirmDiscard())) req.preventDefault(); });
```

### Shims

| Framework API | Boson |
|---|---|
| Tauri `new WebviewWindow(label, opts)` + `tauri://created` / `tauri://error` | `Window.create` (events delivered locally) |
| Tauri `getCurrentWindow()`, `getAllWindows()`, `Window.getByLabel`, `getCurrentWebviewWindow()` | injected label, `Window.list` |
| Tauri window methods (`setTitle`, `setSize(LogicalSize)`, `outerPosition`, `isMaximized`, `onCloseRequested`, `onMoved`, `onResized`, `onFocusChanged`...) | `Window.*` (the class is exported as `TauriWindow`; alias it to `Window` in your import) |
| Tauri `emitTo('label' or {kind, label}, ...)` | `Window.emit` to that window (before: broadcast) |
| Electron `new BrowserWindow(opts)`, `loadURL`, `loadFile`, `getAllWindows`, `getFocusedWindow`, `fromId` | renderer-side proxy like `@electron/remote`; getters return Promises; `ready` resolves on creation |
| Electron `win.webContents.send(ch, ...args)` | `Window.emit` of `ipc:<ch>` -> `ipcRenderer.on(ch)` in that window |
| Electron `win.on('close' / 'closed' / 'focus' / 'move' / 'resize' / 'maximize' / 'unmaximize' ...)` | `window:*` events; `close` with `preventDefault()` for the current window |
| Wails `Window.*` | `Boson.*`, routed to the calling window by `sender_resolver` |
| Wails `Window.Get(name)`, `Window.Name()` | the current window for its own name, else a by-label proxy on `Window.*` (page-level calls such as `SetZoom` reject for other windows) |

## Zero overhead

* No timers or polling: window events come from the window procedure, cleanup is one posted message, the close timeout
  timer exists only while a close is pending and only when configured.
* The Environment is created with the first WebView: WebView-less windows never start a browser process.
* Label lookups scan the live windows (a handful in practice); `emit_to(label)` serializes once and filters attachments
  on the UI thread. The IPC hot path (`Bridge::on_web_message`, Router) is unchanged: sender -> window resolution happens
  only in the controllers that need it.
* Each extra window costs one renderer process (WebView2 does not share renderers between WebViews), never another
  browser / GPU / utility process.

## Performance

Measured by `boson_window_tests` (Debug build, MSVC, WebView2 158.0.4370.33, Windows 11, machine shared with other
builds; ranges over three runs, the slower end from a run under parallel load):

| Metric | Value |
|---|---|
| Create a hidden window without WebView | 0.45-0.98 ms |
| Destroy + deferred cleanup | 0.25-1.16 ms |
| `find(label)` among 20 windows | 0.9-1.15 µs (Debug, unoptimised) |
| Processes, 1 window -> 3 windows | 5 -> 7 (browser 1 -> 1, renderer 1 -> 3) |
| Private bytes, app + WebView2 tree | 145-146 MiB (1 window) -> 197-202 MiB (3 windows): **~26-28 MiB per extra window** |
| First window to page loaded (incl. browser start) | 196-416 ms |
| Extra window to page loaded | 29-47 ms |
| `emit_to(label)` enqueue (serialize + post) | 5.7-7.6 µs/event |
| `emit_to(label)` route + PostWebMessageAsJson | 7.6-9.0 µs/event (2000 events received by the page in 31-38 ms) |
| `Window.current` round trip from JS (Bridge -> pool -> UI -> reply) | 0.17-0.33 ms |
| `boson_windows_demo --smoke` (start, pop out dashboard, verify, quit) | ~0.7 s wall time |

## Verification

Verified (automated, `boson_window_tests`, 210 checks):
* 3 windows by label: create / find / labels; duplicate, invalid label, unknown parent and modal-without-parent rejected.
* Owned windows (Win32 owner = parent; destroyed with the owner), `skip_taskbar` (invisible owner).
* Modal: owner disabled while open, centered on the owner (±2 px), nested modals counted, owner re-enabled on close;
  `focus(owner)` hands the focus to the modal.
* Close veto: `prevent()`, deferred confirm (wrong id rejected, cancel, allow), close guard, timeout, `close_async` sender
  (false when vetoed, true when closed), `hide_on_close`.
* Quit on last window (custom callback), hidden windows keep the app alive, `destroy_all` does not trigger it, can be disabled.
* State persistence per label: two windows, two files, bounds restored per label (±2 px).
* Move / resize / maximize / minimize / restore / show / hide events.
* WebView2: one browser process for 3 and 4 windows, the same browser PID for every WebView, `webview_count`; process
  count and memory per extra window measured (table above); no Environment for WebView-less windows.
* Label script injection; `set_bridge` attaching existing WebViews with their labels.
* `Bridge::emit_to(label)` reaches only that window, `emit` all three; `Window.emit` from a page to another window; reserved
  `window:*` names rejected.
* Sender routing: `Window.current` and `Boson.set_title` from the dashboard act on the dashboard only.
* `Window.create` from JS: label outside `Capability::windows` denied, external URL denied, allowed label created as an owned
  window, attached to the Bridge, listed by `Window.list`, `window:created` broadcast.
* Page close guard end to end: `window:close-requested` -> `Window.confirm_close` false keeps the window, true closes it,
  `window:closed` reaches the other windows.
* `windows.ts` and the Tauri / Electron / Wails window shims, bundled with bun and run in a real window against the real
  controller (`tests/js/windows_selftest.ts`, 22 checks) plus C++ checks of the cross-window effects (Tauri `emitTo` by label,
  Wails `Window.Get(other).SetTitle`, Electron `webContents.send` -> `ipc:chan` with its args). The shims type-check with `tsc`
  (frontend build and `tests/js/tsconfig.json`).
* Capability `windows` patterns (exact, prefix, `*`, JSON, invalid pattern rejected, origin mismatch).
* `boson_windows_demo --smoke`: the main page pops out the dashboard through `Window.create`; 1 browser process, 2 WebViews.
* Every pre-existing test executable still passes (SystemController refactor, Bridge and capability changes).

Not verified:
* Real user input: clicking the X button / Alt+F4 by hand, the interactive `confirm()` in the demo dashboard, dragging
  windows between monitors with different DPI (`set_bounds` converts DIPs with the window's current monitor), Snap.
* Taskbar appearance of `skip_taskbar` windows and `set_skip_taskbar` (ITaskbarList) was not inspected visually; the test
  only checks the invisible owner.
* Release-build numbers (all measurements above are Debug).
* Memory per window is the private-bytes delta of the whole tree right after the pages loaded (includes renderer
  warm-up); it was not tracked over time.
* Windows 10 (tests ran on Windows 11 only).
