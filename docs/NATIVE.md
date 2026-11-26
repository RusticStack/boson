# Native platform pieces

The small native APIs that IDE-class apps (Orca ADE, T3 Code) and full native rewrites (Scribe) need beyond
windows and WebViews. Every piece is a C++ API first (`include/boson/native/*.hpp`, usable without IPC), then a
controller (`include/boson/ipc/controllers/native_controller.hpp`, typed TS through tsgen), then JS helpers
(`frontend/src/native.ts`) and Electron shims (`frontend/src/compat/electron.ts`).

| Piece | C++ API | Controller (JS) | Events |
|---|---|---|---|
| File open / save / folder pickers | `native/dialog.hpp` | `Dialog.open/save/pick_folder` | — |
| Message box / task dialog / error box | `native/message_box.hpp` | `Dialog.message_box/error_box` | — |
| Permission policy (per origin, persisted, JS prompt hook) | `native/permissions.hpp` | `Permissions.*` | `permission:prompt`, `permission:changed` |
| Web Notification API -> Windows toasts | `native/web_notifications.hpp` | — (pages use `new Notification`) | `web-notification:shown/clicked/closed` |
| Zoom (factor, preset steps, Electron levels) | `WebView::set_zoom_factor`, `events().on_zoom_factor_changed`, `native/zoom.hpp` | `Zoom.*` | `zoom:changed` |
| Browser-argument helpers | `native/browser_args.hpp` | — (fixed at environment creation) | — |
| App + WebView2 process metrics | `native/metrics.hpp` | `Native.metrics` | — |
| Locale / preferred languages | `native/system_info.hpp` | `Native.locale` | `system:locale-changed` |
| Accent, high contrast, reduced motion, text scale | `native/system_info.hpp` (reuses `titlebar::query_theme`) | `Native.system_preferences` | `system:preferences-changed` |
| Session end (WM_QUERYENDSESSION / WM_ENDSESSION) | `native/session.hpp` `SessionEndWatcher` | `Native.set_shutdown_block_reason` | `session:query-end`, `session:end` |
| Close to tray | `native/session.hpp` `CloseToTray` | `Native.set_close_to_tray_enabled/hide_to_tray/restore_from_tray` | `tray:window-hidden` |
| Tray balloon click | already in `TrayIcon` (`TrayEventKind::BalloonClicked`) | `Menu.*` | `tray:click` with `button: "balloon-click"` |

`examples/native/main.cpp` (`boson_native_example`) wires all of it into one window.

## Zero-overhead notes

- Nothing here runs while the app is idle. There are no timers and no polling: session end, preferences and locale
  changes are window message hooks (`WM_QUERYENDSESSION`, `WM_ENDSESSION`, `WM_SETTINGCHANGE`, `WM_THEMECHANGED`,
  `WM_SYSCOLORCHANGE`, `WM_DWMCOLORIZATIONCOLORCHANGED`). Metrics compute CPU % from the previous call's sample.
- Lazy init: the toast notifier behind `WebNotificationHost` is created on the first page notification, the
  permission file is read on the first policy query, comctl32 v6 (task dialogs) is loaded on the first message
  box, the deferral window behind `defer_to_message_loop` is created on the first dialog.
- The UI thread never blocks on IPC. Dialog controller calls hold their pool thread until the user answers. The UI
  thread keeps pumping inside the dialog's own modal loop. That loop starts from its own posted message
  (`native::defer_to_message_loop`), never nested inside a `UiMarshal` batch or a WebView2 event handler, so the
  rest of the batch that launched the dialog still runs while the dialog is open (tested).

## Dialogs

```cpp
#include <boson/native/dialog.hpp>
#include <boson/native/message_box.hpp>

boson::native::FileDialogOptions open;
open.title = "Open workspace file";
open.default_path = "C:\\src\\project";              // folder, or folder\\file.ext
open.filters = {{"Source", {"cpp", "hpp", "ts"}}, {"All files", {"*"}}};
open.allow_multiple = true;
boson::native::show_file_dialog_async(boson::native::FileDialogKind::Open, window.hwnd(), open,
    [](boson::native::FileDialogResult r) { /* r.cancelled, r.paths (UTF-8), r.filter_index */ });

boson::native::MessageBoxOptions box;
box.title = "Boson";
box.message = "Save changes to 2 files?";
box.icon = boson::native::MessageBoxIcon::Warning;
box.buttons = {"Save", "Don't save", "Cancel"};   // cancel_id auto-resolves to "Cancel"
box.checkbox_label = "Don't ask again";
boson::native::show_message_box_async(window.hwnd(), box, [](boson::native::MessageBoxResult r) {
    // r.response = button index, r.checkbox_checked, r.cancelled (Esc / close box)
});
```

- `IFileOpenDialog` / `IFileSaveDialog` with `FOS_PICKFOLDERS` for folders. Options: title, default path/name,
  filters + initial filter index (the chosen index is returned), OK button label, default extension (save; falls
  back to the first extension of the selected filter), overwrite prompt, show hidden, don't add to recent. Paths
  are UTF-8. The sync forms (`open_file_dialog`, `save_file_dialog`, `pick_folder_dialog`) exist for C++ apps that
  call them from their own message dispatch.
- Message boxes are `TaskDialogIndirect`: custom buttons (or command links), info / warning / error / question /
  shield icons, default and cancel button, verification checkbox, footer, owner window. comctl32 v6 is activated
  at run time through shell32's common-controls manifest (resource 124), so executables need no manifest
  dependency. When that fails, `MessageBoxW` is used (`used_fallback`; no checkbox).
- `show_error_box(owner, title, content)` is Electron's `dialog.showErrorBox`.
- Automation hooks: `FileDialogOptions::on_shown(IFileDialog&, HWND)` (from the first `IFileDialogEvents::OnFolderChange`)
  and `MessageBoxOptions::on_shown(HWND)` (`TDN_CREATED`). The tests drive every dialog through them:
  `SetFileName` plus `WM_COMMAND IDOK/IDCANCEL` for the file dialog, `TDM_CLICK_BUTTON` / `TDM_CLICK_VERIFICATION`
  for task dialogs (button control ids are `kMessageBoxButtonBaseId + index`).

JS:

```ts
import { dialog } from './native';
const { canceled, filePaths } = await dialog.showOpenDialog({ properties: ['openFile', 'multiSelections'],
  filters: [{ name: 'Images', extensions: ['png', 'jpg'] }] });
const { response, checkboxChecked } = await dialog.showMessageBox({ message: 'Overwrite?', type: 'question',
  buttons: ['Overwrite', 'Cancel'], checkboxLabel: "Don't ask again" });
// or the raw controller shapes: dialog.open({ multiple: true, filters: [...] }) -> { cancelled, paths, path, filter_index }
```

## Permissions

```cpp
boson::native::PermissionManager permissions(app_data / L"permissions.json"); // empty path = in memory
permissions.set_fallback(boson::native::PermissionDecision::Default);         // unknown kinds: WebView2's own UI
permissions.set_default(boson::core::PermissionKind::Notifications, boson::native::PermissionDecision::Ask);
permissions.set_default(boson::core::PermissionKind::ClipboardRead, boson::native::PermissionDecision::Deny);
permissions.set("https://meet.example.com", boson::core::PermissionKind::Camera, boson::native::PermissionDecision::Allow);
permissions.attach(webview);  // takes events().on_permission_requested; an earlier handler is chained for "default"
permissions.set_prompt_handler([&](const boson::native::PermissionPrompt& p) {
    // show any UI, later (UI thread): permissions.answer(p.id, PermissionDecision::Allow, /*remember*/ true);
});
```

- Kinds: microphone, camera, geolocation, notifications, other_sensors, clipboard_read, multiple_automatic_downloads,
  file_read_write, autoplay, local_fonts, midi_sysex, window_management, persistent_storage (new
  `core::PermissionKind::PersistentStorage`).
- Resolution: a remembered rule for (origin, kind), then a `"*"` rule for the kind, then `set_default(kind)`, then the
  fallback (`Ask`). Only Allow / Deny are stored. Origins are normalized (`scheme://host[:port]`, lower case).
- `Ask` defers the WebView2 request (`GetDeferral`) until `answer()`. Without a prompt handler it degrades to
  `Default`. Decisions made by the policy set `SavesInProfile = FALSE` (new
  `core::PermissionRequest::set_saves_in_profile`), so the WebView2 profile never caches a decision behind the
  policy's back: revoking a rule takes effect on the next request.
- Persistence: `{"version":1,"rules":[{"origin","kind","decision"}]}`, written atomically (temp file +
  `MoveFileExW`) on every change. An unreadable file is ignored (empty policy), and the next change rewrites it.
- JS prompt hook (opt-in, so a page without a listener never hangs):

```ts
import { permissions } from './native';
const off = permissions.onPrompt(async (p) => {        // p: { id, webview_id, origin, uri, kind, user_initiated }
  const ok = confirm(`${p.origin} wants ${p.kind}`);
  return { decision: ok ? 'allow' : 'deny', remember: true };
});
await permissions.set('https://x.example', 'geolocation', 'deny');
await permissions.list();                               // remembered rules
```

## Web Notification API -> native toasts

```cpp
boson::native::WebNotificationHost notifications(marshal);  // app_id defaults to the process AppUserModelID
notifications.attach(webview, window.hwnd());              // window restored + focused on click
connect_web_notifications(native_controller, notifications); // optional: web-notification:* events
```

- Handles `ICoreWebView2_24::NotificationReceived` (SDK 1.0.4370) through the new
  `core::WebViewEventHandlers::on_notification_received` slot, so the handler survives `Environment::recreate`.
  It sets `Handled = TRUE` (no WebView2 default popup) and shows a `shell::ToastNotifier` toast: title, body,
  origin as attribution, `silent`, `requireInteraction` -> long duration, local (`file:`) icon / image.
- Toast click: `ReportClicked()` (the page's `notification.onclick` runs), the window is restored and focused,
  `web-notification:clicked` is emitted. User dismissal or a failed toast: `ReportClosed()` (`onclose`).
  `notification.close()` from the page removes the toast. A new notification with the same origin + tag replaces
  the older one.
- The runtime rejects `ReportClosed()` after `ReportClicked()`, after a page `close()` and for tag-replaced
  notifications (`0x8007139F`, found by the test). The host therefore only reports close for user dismissal,
  failures, detach and destruction.
- Whether a page may notify at all is the Notifications permission, so combine it with `PermissionManager`.
- Fallback: when the toast cannot be shown (toasts disabled, or an unpackaged app whose AppUserModelID is not
  registered with `shell::register_app_user_model_id`), a `Shell_NotifyIcon` balloon is shown. A balloon
  click is not reported back to the page.
- `set_presenter(fn)` replaces the toast with custom UI (an in-app notification centre), then call
  `click(id)` / `close(id)`. The tests use it.

## Zoom

- C++: `WebView::set_zoom_factor` / `zoom_factor` (existing) plus the new `events().on_zoom_factor_changed`
  (`ICoreWebView2Controller::ZoomFactorChanged`). `native::next_zoom_level(f, ±1)` steps through Chromium's preset
  levels (0.25 … 5.0); `zoom_level_to_factor` / `zoom_factor_to_level` convert Electron zoom levels (1.2^level).
- The runtime raises ZoomFactorChanged for user zoom (Ctrl+wheel, Ctrl+Plus/Minus), not for `put_ZoomFactor`. The
  test checks both behaviours. The `Zoom` controller therefore emits `zoom:changed` itself after programmatic
  changes, and `connect_events(zoom_controller, bridge)` forwards user zoom.
- JS: `zoom.get/set/setLevel/zoomIn/zoomOut/reset/onChanged`. Electron shim: `webFrame.setZoomFactor/getZoomFactor/
  setZoomLevel/getZoomLevel`. The getters return the last known value, because Boson IPC is asynchronous.

## Browser arguments (EnvironmentConfig helpers)

```cpp
namespace ba = boson::native::browser_args;
boson::core::EnvironmentConfig cfg;
ba::set_max_active_webgl_contexts(cfg, 32);              // Orca: many xterm.js / canvas WebGL contexts (default 16)
ba::add_js_flags(cfg, {L"--max-old-space-size=8192"});   // merged into ONE --js-flags="..." switch
ba::disable_features(cfg, {L"msSmartScreenProtection"}); // merged by core::build_browser_arguments
ba::set_autoplay_policy(cfg, ba::AutoplayPolicy::NoUserGestureRequired);
ba::ignore_gpu_blocklist(cfg);
ba::set_remote_debugging_port(cfg, 9222);                // automation only, never ship it
auto env = boson::core::Environment::create(cfg);
// -> --max-active-webgl-contexts=32 --js-flags="--max-old-space-size=8192" --autoplay-policy=no-user-gesture-required
//    --ignore-gpu-blocklist --disable-features=msSmartScreenProtection
```

Chromium keeps only the last copy of a switch, so every setter replaces an earlier `--name` / `--name=value`
(`find_switch`, `set_switch`, `remove_switch` are the generic forms). The arguments are read once, when the browser
process starts. Changing them later needs `Environment::recreate` or `app::relaunch`. Every WebView sharing a user
data folder must use the same arguments, or environment creation fails.

## Metrics

```cpp
boson::native::MetricsSampler sampler;   // keep one: CPU % is measured since its previous sample
const auto pids = boson::native::collect_webview_processes(std::span{&env, 1}); // UI thread (GetProcessInfos)
const boson::native::AppMetrics m = sampler.sample(pids);                        // any thread
// m.processes[i]: pid, type ("app", "browser", "renderer", "gpu", "utility", ...), working_set_bytes,
// peak_working_set_bytes, private_bytes, cpu_percent, cpu_time_ms, created_unix_ms, accessible
```

`Native.metrics()` reads the PIDs on the UI thread and queries each process (`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)`,
`GetProcessMemoryInfo` / `PROCESS_MEMORY_COUNTERS_EX2`, `GetProcessTimes`) on the pool thread. JS: `app.metrics()`, or
`app.getAppMetrics()` for Electron's shape (memory in KiB). The controller is named `Native`, not `App`, because the
demo already registers an `App` controller.

## Locale and system preferences

- `query_locale()`: user / system locale, preferred UI languages (`GetUserPreferredUILanguages`, Electron
  `getPreferredSystemLanguages`), home region (ISO 3166), decimal and list separators, 24 h clock, first day of
  week, metric system.
- `query_system_preferences()`: dark mode, high contrast (+ scheme), reduced motion (`SPI_GETCLIENTAREAANIMATION`),
  transparency, accent colour (`titlebar::query_theme`, the same source the titlebar uses), text scale factor,
  keyboard cues, caret blink, double-click time, wheel lines, swapped buttons.
- `SystemPreferencesWatcher(window)` re-reads both snapshots on settings messages and calls back only when something
  changed. The `Native` controller owns one and emits `system:preferences-changed` / `system:locale-changed`.
  `applySystemPreferences()` (JS) mirrors reduced motion / high contrast / text scale / accent onto `<html>`.

## Session end and close to tray

```cpp
boson::native::SessionEndWatcher session(window);
session.set_block_reason("2 unsaved files");          // logoff/shutdown asks the user; "" clears
session.set_end_callback([&](bool ending, const boson::native::SessionEndInfo&) {
    if (ending) { store.flush(); }                     // synchronous only: the process dies after this returns
});

boson::native::TrayIcon tray(window.hwnd());
boson::native::CloseToTray close_to_tray(window, tray, {.first_hide_title = "Still running"});
close_to_tray.install_tray_callback();                // or forward events with handle_tray_event()
// tray menu "Quit": close_to_tray.quit();
```

- `WM_QUERYENDSESSION` answers FALSE while a block reason is set (`ShutdownBlockReasonCreate`, shown by Windows)
  or when the query callback vetoes. Critical ends (`ENDSESSION_CRITICAL`) are always allowed.
- `CloseToTray` cancels `CloseRequested` (caption X, Alt+F4, `WM_CLOSE`), hides the window and keeps the tray icon.
  Left/double click, keyboard select or a balloon click restores it. `quit()` closes for real. It does not take over
  the TrayIcon callback unless you call `install_tray_callback()`, so it can sit next to `MenuBackend`'s tray
  listener.
- JS: `app.setShutdownBlockReason(reason)`, `app.onQueryEndSession`, `app.onSessionEnd`, `app.setCloseToTray`,
  `app.hideToTray`, `app.restoreFromTray`, `app.onHiddenToTray`. The close-to-tray methods need the app to
  pass its `CloseToTray` to `NativeController::set_close_to_tray`.

## Wiring (controllers)

```cpp
boson::ipc::DialogController dialog_controller(window, marshal);
boson::ipc::PermissionController permission_controller(permissions, marshal);
boson::ipc::ZoomController zoom_controller(webview, marshal);           // one per WebView
boson::ipc::NativeController native_controller(window, marshal, {environment});
native_controller.set_close_to_tray(&close_to_tray);                   // optional
boson::ipc::Router<boson::ipc::DialogController, boson::ipc::PermissionController,
                   boson::ipc::ZoomController, boson::ipc::NativeController>
    router(dialog_controller, permission_controller, zoom_controller, native_controller);
boson::ipc::Bridge bridge(runner, marshal, router);
bridge.attach(webview);
connect_events(permission_controller, bridge);
connect_events(zoom_controller, bridge);
connect_events(native_controller, bridge);
connect_web_notifications(native_controller, notifications);
```

Event names and payloads are in `boson::ipc::NativeEvents` (registered with tsgen, so `BosonEvents` is typed).

## Performance

Measured by `boson_native_tests` (Debug build, MSVC, WebView2 runtime 158.0.4370.33, 6 processes in the tree):

| Hot path | Cost |
|---|---|
| `MetricsSampler::sample` (app + 5 WebView2 processes) | 26–50 µs per call (38 µs last run) |
| `PermissionManager::resolve` (64 rules, origin normalisation of a raw URL) | ~8.6 µs; it runs once per WebView2 permission request, not per IPC message |
| `query_system_preferences` (once per settings broadcast) | ~84 µs |
| `query_locale` (once per settings broadcast) | ~26 µs |
| Memory (this run) | app process 12 MiB working set; WebView2 tree 346 MiB working set / 149 MiB private |
| Idle cost of every piece | 0: no timers, no threads; message hooks only |

## Verification

What `boson_native_tests` checks automatically (it opens one small non-activated window and briefly shows each
dialog, closing it through automation. No dialog stays open, and every check has a timeout):

- File dialogs through `IFileDialogEvents`: open with filters + default folder, multi-select, filter index round
  trip, save with default extension from the filter, folder picker, cancel, async variant.
- Task dialogs through `TDM_CLICK_BUTTON` / `TDM_CLICK_VERIFICATION`: custom buttons, checkbox state, Esc ->
  resolved cancel id, async variant. The `Dialog` controller is driven over Router + TaskRunner with a timer that
  finds the dialog inside its modal loop (message box answered, file dialog cancelled). A test also proves that a
  dialog started from a `UiMarshal` batch does not stall the rest of the batch.
- Permission policy: defaults, fallback, wildcard, normalisation, persistence round trip, corrupt file, change
  callback, controller methods over Router. With the WebView2 runtime: the prompt hook receives a real
  `Notification.requestPermission()` (kind, origin), a deny answer reaches the page as `"denied"`, and a remembered
  Allow rule answers `"granted"` without a prompt.
- Web notifications (runtime): `new Notification()` reaches the presenter with title / body / tag / origin, the
  page receives `show` and `click`, `notification.close()` removes it, same-tag replacement.
- Zoom (runtime): `put_ZoomFactor` round trip, ZoomFactorChanged from a Ctrl+wheel message posted to the render
  widget, `Zoom.zoom_in` over Router.
- Browser args: switch merging / replacement, the final argument string, and `--js-flags=--expose-gc` reaching V8
  (`typeof gc === "function"` in the page) with `--max-active-webgl-contexts=32` in the environment arguments.
- Metrics: own process, an inaccessible PID, CPU % after a busy loop, browser + renderer processes in the WebView2
  tree, `Native.metrics` over Router.
- Locale / preferences non-empty and well-formed. The watcher does not fire when nothing changed.
- Session end: `WM_QUERYENDSESSION` with and without a block reason (`ShutdownBlockReasonQuery` checked), critical
  end, query veto callback, `WM_ENDSESSION(FALSE)`, and `Native.set_shutdown_block_reason` over Router.
- Close to tray: `WM_CLOSE` hides instead of destroying, tray double-click restores, right click is ignored,
  `quit()` destroys. The tray shell calls are stubbed, so no real tray icon appears.

Not verified automatically:

- A real toast click (it needs a human, or UI Automation on the Action Center). The click path is tested through
  `WebNotificationHost::click`, which a toast activation calls after marshalling to the UI thread. The balloon
  fallback is not exercised.
- Real logoff / shutdown. The messages are sent synthetically. The "apps are preventing shutdown" screen and the
  termination after `WM_ENDSESSION(TRUE)` are not observed.
- `--max-active-webgl-contexts`: the test proves the switch is passed, but it does not create more than 16 WebGL
  contexts (that depends on the GPU).
- Live changes of high contrast, reduced motion, accent or locale (the test does not change system settings).
- The comctl32 v5 `MessageBoxW` fallback path, and screen-reader behaviour of the dialogs.
- `boson_native_example` was built but only smoke-launched. Its interactive flows (toast click focusing the window,
  JS permission prompt through a native dialog) were not driven automatically.
