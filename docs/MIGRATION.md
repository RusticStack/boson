# Migrating from Electron, Tauri v2 and Wails v3

Boson is Windows-only (Windows 10/11, WebView2). Concepts below map to the Boson equivalent. Entries marked
"provided by the *X* module (parallel workstream)" are being implemented in separate branches at the same time
as this document; the names are the expected ones and may still change when they are merged — check that
module's own doc in `docs/`. Everything else exists on `main` today.

Quick start for an existing frontend: alias the original imports to the shims in `frontend/src/compat/`
(see [PORTING.md](PORTING.md)), register `boson::compat::ElectronIpc` in your Router, re-create each
handler / command / bound method in C++ with `ElectronIpc::handle`, then move hot paths to typed controllers.

## Architecture

| Concept | Electron | Tauri v2 | Wails v3 | Boson |
|---|---|---|---|---|
| Backend language | Node.js (main process) | Rust | Go | C++26 |
| Web engine | Bundled Chromium | WebView2 on Windows | WebView2 on Windows | WebView2 (evergreen or fixed runtime: `EnvironmentConfig::browser_executable_folder`) |
| Main / UI loop | Node event loop | tao event loop | Wails app loop | Win32 message loop (`Window::run_message_loop`); never blocked, IPC runs on the `TaskRunner` pool |
| Window | `BrowserWindow` | `WebviewWindow` | `application.NewWebviewWindow` | `core::Window` + `core::WebViewHost` |
| Several webviews in one window | `WebContentsView` / `BrowserView` | `Webview` (unstable multiwebview) | — | `WebViewHost::create` (windowed or DirectComposition hosting) |
| Asset serving | `file://` / custom protocol | `tauri://` / asset protocol | embedded FS | `boson://app/...` from `#embed`-ed assets (`assets::AssetRegistry`); never from disk |
| Dev server | `loadURL('http://localhost:5173')` | `devUrl` | `frontend:dev` | `WebViewConfig::dev_url` / `--dev` / `BOSON_DEV_URL` |
| Process model | browser + renderer + GPU + utility; Node in main | 1 app process + WebView2 tree | 1 app process + WebView2 tree | 1 app process + WebView2 tree (one browser process per `core::Environment`) |

## IPC

| Concept | Electron | Tauri v2 | Wails v3 | Boson native | Boson shim |
|---|---|---|---|---|---|
| Request / response | `ipcRenderer.invoke` + `ipcMain.handle` | `invoke(cmd, args)` + `#[tauri::command]` | `Call.ByName` / generated bindings + bound Go service | `boson.call('Controller.method', params)` + `BOSON_CONTROLLER` class with reflected methods | `ipcRenderer.invoke` / `invoke` / `Call.ByName` -> `ElectronIpc::handle` |
| Fire and forget | `ipcRenderer.send` + `ipcMain.on` | — (commands only) | — | void controller method | `ipcRenderer.send` -> `ElectronIpc::on` |
| Backend -> frontend | `webContents.send` | `app.emit` / `emit_to` | `app.EmitEvent` | `Bridge::emit / emit_to / emit_json` + `boson.on` | `ElectronIpc::send / broadcast` -> `ipcRenderer.on`; `Bridge::emit` -> `listen` / `Events.On` |
| Frontend -> backend event | — | `emit` + `app.listen` | `Events.Emit` + `app.OnEvent` | controller method | `emit` / `Events.Emit` -> `ElectronIpc::on_event` (+ re-broadcast) |
| Reply to the sender | `event.reply`, `event.sender` | `Window` argument | `sender` of the event | `ipc::current_sender_id()` + `Bridge::emit_to` | `IpcEvent::reply`, `IpcEvent::sender_id` |
| Exposing an API to the page | `contextBridge.exposeInMainWorld` (preload) | global `__TAURI__` / npm package | `@wailsio/runtime` | ES module `frontend/src/boson.ts` | `contextBridge.exposeInMainWorld` (single world, frozen global) |
| Preload / init script | `webPreferences.preload` | `initialization_script` | — | `WebViewConfig::scripts_on_document_created`, `WebView::add_script_on_document_created` | — |
| Typed bindings | manual / `electron-trpc` | `tauri-specta` | generated `bindings/` | `boson_tsgen` -> `frontend/src/generated/boson-bindings.d.ts` | `wails3 generate bindings` output unchanged via the `@wailsio/runtime` alias (`Call.ByID`, `Create`, `CancellablePromise`), or `bindService` |
| Serialization | structured clone | JSON (serde) | JSON | JSON (glaze, zero-copy `raw_json` params) | JSON |
| Access control | `contextIsolation`, sandbox | capabilities / permissions | — | `BridgeAttachOptions::allowed_origins` (default `boson://app` + dev origin); detached WebViews never reach the Router | same |
| Sync IPC | `sendSync` | — | — | not supported (UI never blocks) | `sendSync` throws |

## Windows, webviews and app lifecycle

| Concept | Electron | Tauri v2 | Wails v3 | Boson |
|---|---|---|---|---|
| Frameless / custom titlebar | `frame: false`, `titleBarOverlay` | `decorations(false)` | `Frameless` | `WindowConfig::headless_titlebar / borderless`, CSS `app-region: drag`, `boson.registerMaximizeButton` (Snap Layouts) |
| Mica / acrylic | `backgroundMaterial` | `window-vibrancy` | `BackdropType` | `Window::set_backdrop`, `Boson.set_backdrop` |
| Minimize / maximize / close / fullscreen / always on top / title / opacity / min size / center | `BrowserWindow` methods | `Window` methods | `Window` methods | `core::Window`; JS: `Boson.minimize, maximize_or_restore, close, set_fullscreen, set_always_on_top, set_title, set_opacity, set_min_size, center` |
| Window events | `win.on('resize' ...)` | `WindowEvent` | `OnWindowEvent` | `Window::on(WindowEventKind::...)` (resize, move, focus, blur, minimize, maximize, restore, DPI, close-requested with cancel) |
| Navigation, reload, devtools, zoom | `webContents` | `Webview` | `Window` | `core::WebView` (`navigate, go_back, reload, open_devtools, set_zoom_factor`, ...) |
| `window.open` / new windows | `setWindowOpenHandler` | `on_new_window` | — | `WebViewEventHandlers::on_new_window_requested` |
| Permissions, downloads | `session.setPermissionRequestHandler`, `will-download` | plugins | — | `on_permission_requested`, `on_download_starting` |
| Renderer crash | `render-process-gone` | — | — | `on_process_failed` |
| Background tab throttling / memory | `backgroundThrottling` | — | — | `WebViewHost::set_sleep_policy`, `WebView::try_suspend / set_memory_usage_target` |
| Single instance | `app.requestSingleInstanceLock` | `tauri-plugin-single-instance` | `SingleInstance` option | `app::single_instance` (argv + cwd forwarded) |
| Quit / relaunch | `app.quit / app.relaunch` | `app.exit / restart` | `app.Quit` | `app::quit` (waits for the browser process), `app::relaunch` |
| File open / save / folder dialogs | `dialog.showOpenDialog` | `tauri-plugin-dialog` | `application.OpenFileDialog` | `native::open_file_dialog / save_file_dialog`; JS `Boson.open_file_dialog / save_file_dialog` |
| Notifications | `Notification` | `tauri-plugin-notification` | notifications service | `native::Notifier`; JS `Boson.notify` (richer toasts: provided by the `notifications` module (parallel workstream)) |

## Platform services

| Concept | Electron | Tauri v2 | Wails v3 | Boson |
|---|---|---|---|---|
| Application / context menus | `Menu`, `Menu.setApplicationMenu` | `tauri::menu` | `application.NewMenu` | provided by the `menu` module (parallel workstream) |
| Tray icon | `Tray` | `tauri::tray` | `SystemTray` | provided by the `tray` module (parallel workstream) |
| Clipboard | `clipboard` | `tauri-plugin-clipboard-manager` | `Clipboard` | provided by the `clipboard` module (parallel workstream) |
| Open URL / file in default app, reveal in Explorer, trash | `shell.openExternal / showItemInFolder / trashItem` | `tauri-plugin-opener`, `tauri-plugin-shell` | `Browser.OpenURL` | provided by the `shell` module (parallel workstream) |
| Global shortcuts | `globalShortcut` | `tauri-plugin-global-shortcut` | key bindings | provided by the `shortcuts` module (parallel workstream) |
| Deep links / protocol handler | `app.setAsDefaultProtocolClient`, `open-url` | `tauri-plugin-deep-link` | — | provided by the `deeplink` module (parallel workstream) (argv delivery already works through `app::single_instance`) |
| Displays / cursor position | `screen` | `Monitor` API | `Screens` | provided by the `screen` module (parallel workstream) |
| Persist window size / position | `electron-window-state` | `tauri-plugin-window-state` | — | provided by the `window_state` module (parallel workstream) |
| Native file drag and drop | `will-navigate` / `webUtils.getPathForFile` | `DragDropEvent` | `OnFileDrop` | provided by the `dragdrop` module (parallel workstream) |
| Print / print to PDF | `webContents.print / printToPDF` | `Webview::print` | `Print` | provided by the `print` module (parallel workstream) |
| Cookies | `session.cookies` | `Webview::cookies` | — | provided by the `cookies` module (parallel workstream) |
| Sessions / profiles / partitions | `session.fromPartition` | data directory | — | provided by the `profiles` module (parallel workstream) (separate `core::Environment` + `user_data_folder` works today) |
| Secret storage | `safeStorage`, `keytar` | `tauri-plugin-stronghold` | — | provided by the `safe_storage` module (parallel workstream) (DPAPI / Credential Manager) |
| File system access from JS | Node `fs` | `tauri-plugin-fs` | Go service | provided by the `fs` module (parallel workstream) |
| Taskbar progress, overlay icon, jump list, flash | `setProgressBar`, `setOverlayIcon`, `setJumpList`, `flashFrame` | partial | — | provided by the `taskbar` module (parallel workstream) |
| Power events / prevent sleep | `powerMonitor`, `powerSaveBlocker` | — | — | provided by the `power` module (parallel workstream) |
| Start at login | `app.setLoginItemSettings` | `tauri-plugin-autostart` | — | provided by the `autolaunch` module (parallel workstream) |
| Auto update | `autoUpdater`, `electron-updater` | `tauri-plugin-updater` | updater service | provided by the `updater` module (parallel workstream) |
| Installer / packaging / signing | `electron-builder`, Forge | `tauri build` (MSI / NSIS) | `wails3 package` | provided by the `packaging` module (parallel workstream); assets are already inside the exe (`#embed`) |
| Crash reporting | `crashReporter` | — | — | not provided (WebView2 crash events via `on_process_failed`) |

## Things that do not translate

- **Node.js in the page or the main process.** There is no Node runtime; move that logic to C++ controllers
  or to the platform modules above. `require`, `process`, `Buffer` and npm native modules are not available.
- **Synchronous IPC** (`sendSync`, `remote`): not supported by design.
- **Structured clone** types over IPC (Date, Map, typed arrays, MessagePort): convert to JSON first.
- **Multiple JS worlds** (contextIsolation): Boson pages run in one world; protect the bridge with
  `allowed_origins` and by not attaching third-party-content WebViews to a Bridge.
- **macOS / Linux**: out of scope.

## Performance expectations

Measure your port with `boson_bench` instead of relying on claims: it records the whole process tree
(including every `msedgewebview2.exe`) for any framework's executable. Numbers measured on this machine for
Boson, and the procedure for running the same harness against Electron / Tauri / Wails builds, are in
[BENCHMARKS.md](BENCHMARKS.md).
