# Boson vs. Electron & Tauri v2 — Gap Analysis & Roadmap

This document compares **Boson** against **Electron** and **Tauri v2** (based on their official documentation and plugin ecosystems) to identify what Boson already covers and what is still needed to fully replace them in production desktop applications.

---

## 1. Capability Comparison Matrix

| Area | Capability | Electron | Tauri v2 | Boson (Current) |
| :--- | :--- | :--- | :--- | :--- |
| **Windowing** | Multi-window creation & lifecycle | `BrowserWindow` + `getAllWindows()` | `WebviewWindow` / `WebviewWindowBuilder` | `app::WindowManager` by label (one shared `Environment`), `Window.*` controller, async close confirm, hide-to-tray, quit on last window ([WINDOWS.md](WINDOWS.md)) |
| | Multiple WebViews per window | `WebContentsView` | `Window::add_child` (unstable) | `WebViewHost` (bounds, visibility, z-order, shared `Environment`), windowed or DirectComposition hosting |
| | Tab sleeping / suspension | `webContents.setBackgroundThrottling` | — | `try_suspend` / `resume`, `MemoryUsageTarget`, `SleepPolicy` |
| | Modal / child / owned windows | `parent` + `modal: true` | `WebviewWindowBuilder::parent()` | `ManagedWindowConfig::parent` / `modal` (owner disabled, centered on the owner), `skip_taskbar` |
| | Window state persistence | Community plugin | `tauri-plugin-window-state` | `desktop::WindowStateKeeper` (position, size, maximized, fullscreen, monitor; clamped when a monitor is gone) |
| | Headless / custom titlebar | `titleBarStyle: 'hidden'` + `titleBarOverlay` | `decorations: false` + drag region | `headless_titlebar` + CSS `app-region: drag` + Snap Layouts on a custom maximize button |
| | Fluent 2 backdrops (Mica / Acrylic) | Third-party / limited | `window-vibrancy` / core effects | Built-in (`Mica`, `MicaAlt`, `Acrylic`, `Transparent`, `Translucent`) |
| | Display / Monitor DPI & bounds | `screen` module | `availableMonitors()` / `scaleFactor` | `desktop` screen API (`displays`, `display_nearest_*`, work area, scale, refresh rate) + `screen:changed` |
| **IPC & Data** | Request / Response RPC | `ipcRenderer.invoke` / `ipcMain.handle` | `invoke()` / `#[tauri::command]` | `Router` + `TaskRunner` (`stdexec`) + `boson_tsgen` |
| | Backend → Frontend events | `webContents.send` / `ipcRenderer.on` | `app.emit` / `emit_to` / `listen()` | `Bridge::emit` / `emit_to(webview_id or window label)` (any thread) + typed `boson.on` / `once`, origin allow-list |
| | Synchronous IPC | `ipcRenderer.sendSync` / `event.returnValue` | — (async only) | `ipc::SyncBridge` (IDispatch host object, `boson.invokeSync`, ~0.1 ms round trip, handler on the UI thread, allow-list + CapabilityPolicy) + `ipc::BootValues` (frozen `__BOSON_BOOT__`, no IPC) — [SYNC_IPC.md](SYNC_IPC.md) |
| | Navigation & content events | `webContents` events | `on_navigation`, `on_page_load` | Full set: navigation (cancellable), title, history, favicon, new window (deferral), fullscreen, audio, crash, permissions, downloads |
| | Integrated terminals (PTY) | `node-pty` (native Node addon) | Community plugins (`tauri-plugin-pty`) | `terminal::Pty` (ConPTY, kill-on-close job) + `TerminalHost` (shared-buffer output, ack flow control, hidden-pane replay) + `terminal.ts` / `compat/node_pty.ts` ([TERMINAL.md](TERMINAL.md)) |
| | Binary payloads & streaming | `MessageChannelMain` / `Buffer` | `ipc::Response` + `ipc::Channel` | `transfer::BinaryTransport` (zero-copy shared buffers, ~2.6 GB/s measured; base64 fallback) + `StreamServer` (HTTP Range, seeking) |
| **OS Shell** | Native App & Context Menus | `Menu.buildFromTemplate()` / `popup()` | `tauri::menu` (`MenuBuilder`) | `menu::MenuBar` (native bar or HTML-titlebar popups, roles, mnemonics) + `popup_menu` + `Menu.*` JS |
| | System Tray (menu + click events) | `Tray` + `setContextMenu()` | `TrayIconBuilder` + events | `TrayIcon` (PNG/ICO icons, menu, click events, balloons, Explorer-restart recovery) |
| | Global Keyboard Shortcuts | `globalShortcut.register()` | `tauri-plugin-global-shortcut` | `desktop::GlobalShortcuts` (Electron accelerator syntax) + window/WebView `AcceleratorTable` |
| | Clipboard (text / HTML / image) | `clipboard` | `tauri-plugin-clipboard-manager` | `desktop::Clipboard` (text, CF_HTML, PNG/DIBV5, files, history exclusion, change events) |
| | Deep Links (`myapp://`) | `setAsDefaultProtocolClient()` | `tauri-plugin-deep-link` | `desktop` protocol registration (HKCU) + forwarding into the running instance |
| | Single-Instance Lock | `requestSingleInstanceLock()` | `tauri-plugin-single-instance` | `app::single_instance` (argv + cwd forwarding) |
| | Relaunch / graceful quit | `app.relaunch()` / `app.quit()` | `tauri-plugin-process` | `app::relaunch` (`--boson-wait-pid`), `app::quit` waits for the browser process |
| | Native File Drag-and-Drop | `webUtils.getPathForFile()` | `onDragDropEvent` (`paths[]`) | `transfer::FileDrop` (real paths from WebView drops and non-WebView areas) |
| | Native file drag-out | `webContents.startDrag({file, icon})` | `tauri-plugin-drag` (community) | `transfer::DragOut` (shell IDataObject: Shell IDList Array + CF_HDROP + drag image, `SHDoDragDrop`) + `Drag` controller + `dragout.ts` — [SYNC_IPC.md](SYNC_IPC.md) |
| | Power / Idle / Sleep Inhibit | `powerMonitor` + `powerSaveBlocker` | Community / Win32 `SetThreadExecutionState` | `shell::PowerMonitor` (suspend/resume, AC/battery, display, lock) + `PowerSaveBlocker` |
| | Shell `openExternal` / `showItemInFolder` | `shell` module | `tauri-plugin-opener` + `shell` | `desktop` shell (`open_external` with scheme allow-list, `show_item_in_folder`, `move_to_trash`, known folders) |
| | Auto-Launch on Login | `app.setLoginItemSettings()` | `tauri-plugin-autostart` | `shell` auto-launch (HKCU Run + StartupApproved state) |
| **Notifications** | Native Notifications | WinRT Toast (`toastXml`) + balloon | `tauri-plugin-notification` (WinRT Toast) | WinRT toasts without the Windows App SDK (buttons, inline reply, progress, COM activation) + `Shell_NotifyIconW` fallback |
| | Web Notification API in pages | Native toasts | Plugin | `native::WebNotificationHost` (`NotificationReceived` -> toast, click focuses window + `onclick`, tag replace, close) |
| **Native UI** | File open / save / folder dialogs | `dialog.showOpenDialog` / `showSaveDialog` | `tauri-plugin-dialog` | `native::show_file_dialog_async` (`IFileOpenDialog` / `IFileSaveDialog`, multi-select, filters, default path) + `Dialog.*` JS + Electron shim |
| | Message box / error box | `dialog.showMessageBox` (checkbox) / `showErrorBox` | `message` / `ask` / `confirm` | `TaskDialogIndirect` (custom buttons, icons, "don't ask again", default/cancel id) + `MessageBoxW` fallback |
| | Permission handling | `session.setPermissionRequestHandler` | Capabilities (no runtime prompt API) | `native::PermissionManager` (per-origin rules, persisted, JS prompt hook, `SavesInProfile = FALSE`) |
| | Zoom | `webContents.setZoomFactor` / `webFrame` / `zoom-changed` | `set_zoom` | `Zoom.*` (factor, Chromium preset steps, Electron levels) + `zoom:changed` |
| | Process metrics | `app.getAppMetrics()` | — | `native::MetricsSampler` (app + WebView2 tree, memory + CPU %) + `Native.metrics` |
| | Locale / system preferences | `app.getLocale`, `getPreferredSystemLanguages`, `systemPreferences`, `nativeTheme` | `tauri-plugin-os` (locale) | `native::query_locale` / `query_system_preferences` (accent, high contrast, reduced motion, text scale) + change events |
| | Session end / shutdown blocking | `powerMonitor 'shutdown'` (no block reason) | — | `native::SessionEndWatcher` (`WM_QUERYENDSESSION` veto, `ShutdownBlockReasonCreate`, `WM_ENDSESSION`) |
| | Close to tray | Manual (`close` + `hide`) | Manual | `native::CloseToTray` (cancel close, hide, restore on tray / balloon click) |
| | Chromium switches | `app.commandLine.appendSwitch` | `additionalBrowserArgs` | `native::browser_args` (WebGL context limit, merged `--js-flags`, features, autoplay) |
| **Storage & Security** | Scoped Filesystem API | Node `fs` (unscoped) | `tauri-plugin-fs` (capability-scoped) | `security::ScopedFs` + `Fs.*` (traversal, junction, UNC/device-path protection; watch) |
| | Secure Credential Store | `safeStorage` (Win32 DPAPI) | `stronghold` / `keyring` | `SafeStorage` (DPAPI) + `CredentialStore` (Windows Credential Manager) |
| | Embedded database | `better-sqlite3` (native module) | `tauri-plugin-sql` | `db::Database` + `Table<T>` (glaze row mapping, prepared-statement cache, transactions) on SQLite or ZephyrDB, `KvStore` + namespaced `Db` controller ([NATIVE_SERVICES.md](NATIVE_SERVICES.md)) |
| | Settings Store | `electron-store` | `tauri-plugin-store` | `store::Store<T>` (typed, defaults, corrupt-file recovery, atomic saves) + `Store` controller and `store:changed` ([STORE.md](STORE.md)) |
| | Per-Window Capabilities / CSP | `contextIsolation` + `sandbox` + CSP | `capabilities/*.json` + `security.csp` | `CapabilityPolicy` per WebView/origin (checked before dispatch) + strict default CSP + navigation/new-window policies |
| **Native services** | HTTP client (no CORS) | `net.request` / `fetch` in main | `tauri-plugin-http` | `net::HttpClient` (WinHTTP, redirects checked per hop, gzip/deflate, retries, timeouts, cancel, caps, cache, system/explicit proxy, senders) + `Http` controller with a host allow-list and downloads to `ScopedFs` paths |
| | Archives | Userland (`adm-zip`, `yauzl`) | — | `archive::ZipReader` / `ZipWriter` (deflate, ZIP64, traversal / symlink / bomb rejection, staging extraction) |
| | Hashing | Node `crypto` | Rust crates | `crypto::Hasher` (CNG MD5/SHA-1/SHA-2, streaming, files) |
| | Atomic directory installs | — | — | `fs::InstallTransaction` (staged swap, rollback, crash recovery) + `TempDir` + known folders (OneDrive aware) |
| **Distribution** | Code Signing | Authenticode (`signtool`) | `signtool.exe` integration | `boson_sign_target` (thumbprint, PFX, Azure Trusted Signing parameters) |
| | Installers (NSIS / MSI / MSIX) | Forge / `electron-builder` | Built-in `tauri bundler` | `boson_package`: ZIP and MSIX built and tested; MSI (CPack WiX) templates untested (WiX not installed) |
| | Auto-Updater | `autoUpdater` / `electron-updater` | `tauri-plugin-updater` (Ed25519) | `shipping::Updater` (manifest, staged rollout, resume, SHA-256 + ECDSA P-256 signatures, rollback) |
| **WebView** | Print / PDF / capture / find | `webContents.print` / `printToPDF` / `capturePage` / `findInPage` | Partial (plugins) | `webview::print`, `print_to_pdf`, `capture_preview`, `FindSession` + `WebView.*` JS |
| | Cookies, profiles, clear data, proxy | `session` | Partial | `webview` cookies, named/InPrivate profiles, `clear_browsing_data`, `ProxyConfig` |
| **Shell** | Taskbar progress / overlay / thumbnail buttons / jump lists | `setProgressBar`, `setOverlayIcon`, `setThumbarButtons`, `setJumpList` | — | `shell::Taskbar`, `set_jump_list` |
| | Native caption buttons + accessibility | `titleBarOverlay` | — | `titlebar::Titlebar` (drawn caption buttons, Snap Layouts, UIA, system menu, theme events) |
| **Processes** | Child processes, sidecars, utility processes | `child_process` (Node) + `utilityProcess` | `tauri-plugin-shell` (`Command`, `sidecar`) | `process::ProcessManager` (no-shell argv, IOCP pipes, extra fds, kill-on-close job, kill tree), `Sidecar` (ready checks, backoff restart, graceful stop), named pipes, `Process.*` behind an executable allow-list + `compat/child_process.ts` / `utilityProcess` |
| **Diagnostics** | Crash reporting & logging | `crashReporter` | Community | `shipping::CrashReporter` (minidumps, WebView2 process failures) + rotating `Logger` |
| **Porting** | Framework API shims | — | — | `compat/electron.ts`, `compat/tauri.ts`, `compat/wails.ts` + C++ `ElectronIpc` (`sendSync`, `webUtils`, `webFrame` zoom, `process`, sync `clipboard`, `startDrag`) |
| **Rendering** | Native GPU content in the window | `OffscreenCanvas`/WebGL/WebGPU only (native via addons + separate windows) | — (raw-window-handle + wgpu in a separate surface) | `gpu::Surface`: D3D11/D3D12 flip-model swap chains composited below/above the WebView, `<boson-surface>` layout from HTML, 2D batch renderer with DirectWrite text grid, Vulkan interop, device-lost recovery ([GPU.md](GPU.md)) |
| **Platforms** | OS Support | Windows, macOS, Linux | Windows, macOS, Linux (+ Mobile) | Windows 10/11 only (macOS/Linux intentionally out of scope for now) |

---

## 2. Roadmap status

Done (see the module docs):
- Events, multi-WebView hosting, lifecycle: [USAGE.md](USAGE.md)
- Window manager by label (owned / modal windows, `emit_to(label)`, close veto, state per label): [WINDOWS.md](WINDOWS.md)
- Menus, tray, accelerators: [MENU.md](MENU.md)
- Clipboard, shell, global shortcuts, deep links, screen, window state: [DESKTOP.md](DESKTOP.md)
- File drop, zero-copy binary IPC, Range streaming: [TRANSFER.md](TRANSFER.md)
- Terminals (ConPTY, flow control, xterm.js wiring, node-pty shim): [TERMINAL.md](TERMINAL.md)
- Print/PDF, capture, find, cookies, profiles, proxy, auth/certificate events: [WEBVIEW_FEATURES.md](WEBVIEW_FEATURES.md)
- Capabilities, CSP, safe storage, credentials, scoped FS: [SECURITY.md](SECURITY.md)
- Taskbar, jump lists, toasts, power, auto-launch: [SHELL_INTEGRATION.md](SHELL_INTEGRATION.md)
- Packaging, signing, updater, crash reporting, logging: [SHIPPING.md](SHIPPING.md)
- Child processes, sidecars, named pipes, bundled Node: [PROCESS.md](PROCESS.md)
- HTTP client, archives, hashing, staged installs, SQLite / ZephyrDB: [NATIVE_SERVICES.md](NATIVE_SERVICES.md)
- Titlebar overlay, accessibility, theme events: [TITLEBAR.md](TITLEBAR.md)
- Dialogs, message boxes, permissions, web notifications, zoom, browser args, metrics, locale / preferences, session end, close-to-tray: [NATIVE.md](NATIVE.md)
- Native GPU surfaces, 2D renderer, Vulkan interop: [GPU.md](GPU.md)
- Electron/Tauri/Wails shims and migration tables: [MIGRATION.md](MIGRATION.md), [PORTING.md](PORTING.md); measurements: [BENCHMARKS.md](BENCHMARKS.md)

Remaining:
1. **MSI verification**: build and install the CPack WiX package on a machine with WiX installed.
2. **Comparative benchmarks**: run `boson_bench` against equivalent Electron, Tauri and Wails apps.
3. **Real-input verification**: physical drag-and-drop from Explorer, Snap Layouts hover, global shortcut key presses, toast clicks (including Web Notification toasts), real logoff / shutdown, and screen readers (all tested only with synthesized messages).
4. **Cross-platform backends**: intentionally out of scope for now (Windows only).
