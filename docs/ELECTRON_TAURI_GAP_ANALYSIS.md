# Boson vs. Electron & Tauri v2 — Gap Analysis & Roadmap

This document compares **Boson** against **Electron** and **Tauri v2** (based on their official documentation and plugin ecosystems) to identify what Boson already covers and what is still needed to fully replace them in production desktop applications.

---

## 1. Capability Comparison Matrix

| Area | Capability | Electron | Tauri v2 | Boson (Current) |
| :--- | :--- | :--- | :--- | :--- |
| **Windowing** | Multi-window creation & lifecycle | `BrowserWindow` + `getAllWindows()` | `WebviewWindow` / `WebviewWindowBuilder` | Multiple `Window`s (`quit_on_destroy`, `owner`), vetoable `CloseRequested`, full window event set |
| | Multiple WebViews per window | `WebContentsView` | `Window::add_child` (unstable) | `WebViewHost` (bounds, visibility, z-order, shared `Environment`), windowed or DirectComposition hosting |
| | Tab sleeping / suspension | `webContents.setBackgroundThrottling` | — | `try_suspend` / `resume`, `MemoryUsageTarget`, `SleepPolicy` |
| | Modal / child / owned windows | `parent` + `modal: true` | `WebviewWindowBuilder::parent()` | Missing |
| | Window state persistence | Community plugin | `tauri-plugin-window-state` | Missing |
| | Headless / custom titlebar | `titleBarStyle: 'hidden'` + `titleBarOverlay` | `decorations: false` + drag region | `headless_titlebar` + CSS `app-region: drag` + Snap Layouts on a custom maximize button |
| | Fluent 2 backdrops (Mica / Acrylic) | Third-party / limited | `window-vibrancy` / core effects | Built-in (`Mica`, `MicaAlt`, `Acrylic`, `Transparent`, `Translucent`) |
| | Display / Monitor DPI & bounds | `screen` module | `availableMonitors()` / `scaleFactor` | Per-Monitor V2, `WM_DPICHANGED`, DIP geometry API (`set_bounds`, `center`, min size); no monitor enumeration API yet |
| **IPC & Data** | Request / Response RPC | `ipcRenderer.invoke` / `ipcMain.handle` | `invoke()` / `#[tauri::command]` | `Router` + `TaskRunner` (`stdexec`) + `boson_tsgen` |
| | Backend → Frontend events | `webContents.send` / `ipcRenderer.on` | `app.emit` / `emit_to` / `listen()` | `Bridge::emit` / `emit_to` (any thread) + typed `boson.on` / `once`, origin allow-list |
| | Navigation & content events | `webContents` events | `on_navigation`, `on_page_load` | Full set: navigation (cancellable), title, history, favicon, new window (deferral), fullscreen, audio, crash, permissions, downloads |
| | Binary payloads & streaming | `MessageChannelMain` / `Buffer` | `ipc::Response` + `ipc::Channel` | Missing (JSON envelope only) |
| **OS Shell** | Native App & Context Menus | `Menu.buildFromTemplate()` / `popup()` | `tauri::menu` (`MenuBuilder`) | Missing |
| | System Tray (menu + click events) | `Tray` + `setContextMenu()` | `TrayIconBuilder` + events | Basic `SystemTray` C++ class (no menu/events) |
| | Global Keyboard Shortcuts | `globalShortcut.register()` | `tauri-plugin-global-shortcut` | Missing |
| | Clipboard (text / HTML / image) | `clipboard` | `tauri-plugin-clipboard-manager` | Missing |
| | Deep Links (`myapp://`) | `setAsDefaultProtocolClient()` | `tauri-plugin-deep-link` | Missing |
| | Single-Instance Lock | `requestSingleInstanceLock()` | `tauri-plugin-single-instance` | `app::single_instance` (argv + cwd forwarding) |
| | Relaunch / graceful quit | `app.relaunch()` / `app.quit()` | `tauri-plugin-process` | `app::relaunch` (`--boson-wait-pid`), `app::quit` waits for the browser process |
| | Native File Drag-and-Drop | `webUtils.getPathForFile()` | `onDragDropEvent` (`paths[]`) | Missing |
| | Power / Idle / Sleep Inhibit | `powerMonitor` + `powerSaveBlocker` | Community / Win32 `SetThreadExecutionState` | Missing |
| | Shell `openExternal` / `showItemInFolder` | `shell` module | `tauri-plugin-opener` + `shell` | Missing |
| | Auto-Launch on Login | `app.setLoginItemSettings()` | `tauri-plugin-autostart` | Missing |
| **Notifications** | Native Notifications | WinRT Toast (`toastXml`) + balloon | `tauri-plugin-notification` (WinRT Toast) | `Shell_NotifyIconW` (no WinRT Toast actions/buttons yet) |
| **Storage & Security** | Scoped Filesystem API | Node `fs` (unscoped) | `tauri-plugin-fs` (capability-scoped) | Missing |
| | Secure Credential Store | `safeStorage` (Win32 DPAPI) | `stronghold` / `keyring` | Missing |
| | Per-Window Capabilities / CSP | `contextIsolation` + `sandbox` + CSP | `capabilities/*.json` + `security.csp` | Missing |
| **Distribution** | Code Signing | Authenticode (`signtool`) | `signtool.exe` integration | Manual |
| | Installers (NSIS / MSI / MSIX) | Forge / `electron-builder` | Built-in `tauri bundler` | Raw `.exe` only |
| | Auto-Updater | `autoUpdater` / `electron-updater` | `tauri-plugin-updater` (Ed25519) | Missing |
| **Platforms** | OS Support | Windows, macOS, Linux | Windows, macOS, Linux (+ Mobile) | Windows 10/11 only |

---

## 2. Prioritized Roadmap to Full Parity

### P0 — Core App Foundations (Required by Almost Every Desktop App)
1. ~~**Event Emitter**~~: done (`Bridge::emit` / `emit_to`, typed `boson.on`).
2. **Multi-Window Manager (`WindowManager`)**: multiple windows and multiple WebViews per window (`WebViewHost`) are done. Remaining: Create/close/focus multiple `Window` + `WebView` instances by label, support parent/owned modal windows, and route `emit_to(label, ...)` + `SystemController` calls per window.
3. **Binary IPC / Streaming**: Fast binary transfer path (e.g. `boson://ipc/blob/<id>` custom protocol response or `PostWebMessageAsJson` + `SharedArrayBuffer`/chunked stream) to bypass JSON serialization for large files, audio/video, or language-server payloads.
4. **Deep Links**: single instance with argv forwarding is done (`app::single_instance`). What remains is protocol registration (`HKCU\Software\Classes\<scheme>`).

### P1 — Desktop OS Integration
5. **Native Menus & Tray Context Menus**: `HMENU` / `CreatePopupMenu` + `TrackPopupMenuEx` wired to `SystemTray` (`WM_APP + 2`) and context menu IPC.
6. **Shell, Clipboard & Global Shortcuts**: `ShellExecuteW` (`open_external`, `reveal_in_explorer`), `OpenClipboard`/`GetClipboardData` (text/HTML/CF_HDROP), `RegisterHotKey`, and `DragAcceptFiles` / `WM_DROPFILES` native path drop.
7. **WinRT Toast Notifications**: Upgrade `Notifier` with optional Windows App SDK / `ToastNotificationManager` support (action buttons, inline reply, images) when packaged with an AUMID, keeping `Shell_NotifyIconW` as the zero-config fallback for loose `.exe` builds.

### P2 — Security, Persistence & Packaging
8. **Secure Storage & Scoped FS**: Win32 DPAPI (`CryptProtectData` / `CryptUnprotectData`) + Windows Credential Manager (`CredWriteW` / `CredReadW`), plus path-scoped FS controller.
9. **Packaging & Auto-Updater**: CMake/CPack NSIS & WiX MSI installer targets, Authenticode signing script, and Ed25519-verified self-updater.
10. **Cross-Platform Backends**: Abstract `Window` / `WebView` behind a platform interface to add macOS (`WKWebView`) and Linux (`WebKitGTK`) backends.
