# Desktop basics

Clipboard, shell integration, global shortcuts, deep links, the screen API and window-state persistence.
Everything is plain Win32/COM (no extra dependencies). The C++ API is in `include/boson/desktop/`, JS
exposure in `include/boson/ipc/controllers/`, typed frontend helpers in `frontend/src/desktop.ts`.

```cpp
#include <boson/boson.hpp> // or the individual boson/desktop/*.hpp headers
```

Linked system libraries added to `boson_core`: `shcore` (GetDpiForMonitor) and `windowscodecs` (WIC).

---

## 1. Clipboard — `boson/desktop/clipboard.hpp`

`desktop::Clipboard` opens and closes the clipboard on every call, retrying for up to `open_timeout`
(500 ms) when another process holds it. It is safe on any thread but may wait, so call it from a
TaskRunner thread (the controller does). Reads return `std::nullopt` (not an error) when that kind of data is
absent. Errors are `std::expected<..., std::string>`.

| Data | Formats written | Read |
|---|---|---|
| text | `CF_UNICODETEXT` (UTF-8 in/out) | `read_text()` |
| HTML | `HTML Format` (CF_HTML, exact byte offsets, optional SourceURL), optional plain-text alternative in the same transaction | `read_html()` returns html, fragment, source_url |
| image | `PNG` (keeps alpha) + `CF_DIBV5` (32 bpp BI_BITFIELDS, alpha mask, bottom-up) | `read_image_png()`: `PNG` if present, else CF_DIBV5/CF_DIB converted with WIC |
| files | `CF_HDROP` (wide, absolute paths) + `Preferred DropEffect` = copy | `read_files()` |

```cpp
boson::desktop::Clipboard clipboard(window.hwnd());
clipboard.write_text("hello");
clipboard.write_html("<b>bold</b>", "bold");                // CF_HTML + CF_UNICODETEXT
clipboard.write_text(token, {.exclude_from_history = true}); // skipped by Win+V history / cloud clipboard
if (auto png = clipboard.read_image_png(); png && *png) { /* **png is PNG bytes */ }

boson::desktop::ClipboardData all;                           // several formats in one transaction
all.text = "alt"; all.html = "<p>alt</p>"; all.png = png_bytes;
clipboard.write(all);

auto names = clipboard.formats();                            // [{id, name}] e.g. "CF_UNICODETEXT", "HTML Format"
bool has_image = boson::desktop::Clipboard::has_format("image"); // no OpenClipboard needed
```

`exclude_from_history` adds `ExcludeClipboardContentFromMonitorProcessing`, `CanIncludeInClipboardHistory = 0`
and `CanUploadToCloudClipboard = 0`.

Pure helpers (also used by the tests): `build_cf_html`, `parse_cf_html` (falls back to the
`<!--StartFragment-->` markers when offsets are broken), `base64_encode` / `base64_decode` (accepts `data:` URLs),
`encode_png` / `decode_png` / `png_dimensions`, `png_to_dibv5`, `dib_to_png` (BITMAPINFOHEADER/V4/V5; 32 bpp BI_RGB
with a non-zero reserved byte is read as alpha, the way browsers write it; other depths go through WIC's BMP codec).

**Change listener.** `ClipboardWatcher` calls `AddClipboardFormatListener` on a private message-only window; the
callback runs on the constructing (UI) thread for every change by any process.

```cpp
auto watcher = boson::ipc::watch_clipboard(bridge); // emits "clipboard:changed" while it lives (UI thread)
```

## 2. Shell — `boson/desktop/shell.hpp`

```cpp
boson::desktop::open_external("https://example.com");           // ShellExecuteExW, allow-list checked first
boson::desktop::open_external(url, {.allowed_schemes = {"https", "vscode"}});
boson::desktop::open_path(L"C:\Users\me\report.pdf");          // default app / Explorer for folders
boson::desktop::show_item_in_folder(path);                       // SHOpenFolderAndSelectItems
boson::desktop::move_to_trash(path);                             // IFileOperation + FOFX_RECYCLEONDELETE, silent
auto downloads = boson::desktop::known_folder(boson::desktop::KnownFolder::Downloads); // SHGetKnownFolderPath
```

- `open_external` allows `http`, `https`, `mailto` by default. `validate_external_url` (no side effects) also
  rejects whitespace, quotes and control characters, drive paths (`C:\...` is not a scheme) and http(s) URLs
  without a host. Never add `file` or `ms-*` schemes for URLs coming from web content.
- `move_to_trash` verifies that the item is gone afterwards. On a volume without a Recycle Bin the shell deletes
  permanently, as Explorer does with confirmations suppressed.
- Known folders: home, desktop, documents, downloads, music, pictures, videos, appData (roaming), localAppData, temp,
  recent, startMenu, programs, startup, programFiles, public, savedGames, fonts, exe. `parse_known_folder` accepts
  those names case-insensitively, with `_`/`-` ignored.
- All functions initialise COM on the calling thread when needed (pool threads have no apartment).

## 3. Global shortcuts — `boson/desktop/shortcuts.hpp`

```cpp
boson::desktop::GlobalShortcuts shortcuts; // UI thread; owns a message-only window
auto id = shortcuts.add("CommandOrControl+Shift+K", [](const boson::desktop::ShortcutEvent& e) {
    // UI thread; e.accelerator == "Ctrl+Shift+K"
});
if (!id && id.error().kind == boson::desktop::ShortcutErrorKind::Conflict) { /* owned by another app */ }
shortcuts.remove(*id);            // or remove("Ctrl+Shift+K"), remove_all(); the destructor unregisters everything
```

- `RegisterHotKey` with `MOD_NOREPEAT`. The errors are `InvalidAccelerator`, `AlreadyRegistered` (this registry already
  has the same combination, in any spelling), `Conflict` (`ERROR_HOTKEY_ALREADY_REGISTERED`: another app or Windows owns it)
  and `SystemError`.
- Accelerator syntax follows Electron. Modifiers: `Command`/`Cmd`/`Control`/`Ctrl`/`CommandOrControl`/`CmdOrCtrl`
  (all Ctrl on Windows), `Alt`/`Option`, `AltGr` (Ctrl+Alt), `Shift`, `Super`/`Meta`/`Win`. Keys: `A`–`Z`, `0`–`9`,
  `F1`–`F24`, `Space Tab Backspace Delete Insert Enter/Return Up Down Left Right Home End PageUp PageDown Escape/Esc`,
  `Capslock Numlock Scrolllock PrintScreen Pause`, `VolumeUp VolumeDown VolumeMute MediaNextTrack MediaPreviousTrack MediaStop MediaPlayPause`,
  `num0`–`num9 numdec numadd numsub nummult numdiv`, `Plus` (or a trailing `++`), and any single punctuation character, which is
  resolved with the active keyboard layout (`VkKeyScanW`, adding Shift when the layout needs it).
  Case-insensitive; the key must come last.
- `parse_accelerator` and `format_accelerator` produce a canonical spelling (`Ctrl+Alt+Shift+Super+Key`) that parses back to
  the same combination.

## 4. Deep links — `boson/desktop/deep_link.hpp`

```cpp
// Per-user registration, no elevation: HKCUSoftwareClassesmyapp
//   (Default)="URL:My App"  "URL Protocol"=""  DefaultIcon="<exe>",0  shellopencommand="<exe>" "%1"
boson::desktop::register_protocol_client("myapp", {.description = L"My App"});
bool mine = boson::desktop::is_default_protocol_client("myapp"); // honours a UrlAssociations UserChoice
boson::desktop::unregister_protocol_client("myapp");             // only if it still points at this exe (or force)
```

Combined with `app::single_instance`, a link opened while the app runs reaches the running instance:

```cpp
const auto instance = boson::app::single_instance(L"My.App", boson::desktop::deep_link_forwarder(
    "myapp",
    [&](std::vector<std::string> links) { /* UI thread of the primary: route links, focus the window */ },
    [&](std::vector<std::wstring>, std::wstring) { /* a second launch without a link */ }));
if (!instance) return 0; // argv (including the URL) was forwarded to the primary

for (const auto& link : boson::desktop::extract_deep_links(boson::app::command_line_args(), "myapp")) {
    /* cold start: the URL is in this process's own argv */
}
```

`is_valid_custom_scheme` rejects names that break RFC 3986 syntax, single letters (drive letters) and schemes Windows or
browsers own (`http(s)`, `file`, `mailto`, `javascript`, `data`, `ms-*`, ...). Registration calls
`SHChangeNotify(SHCNE_ASSOCCHANGED)`.

## 5. Screen — `boson/desktop/screen.hpp`

```cpp
for (const auto& d : boson::desktop::displays()) { // primary first, then left-to-right
    // d.id / d.device_name ("\\.\DISPLAY1"), d.label ("DELL U2720Q"), d.bounds, d.work_area (physical px),
    // d.dpi, d.scale_factor, d.primary, d.refresh_rate (Hz), d.rotation (deg), d.bits_per_pixel
}
auto primary = boson::desktop::primary_display();
auto here = boson::desktop::display_nearest_point(boson::desktop::cursor_screen_point());
auto mine = boson::desktop::display_for_window(window.hwnd());
```

The data comes from `EnumDisplayMonitors`, `GetMonitorInfoW`, `GetDpiForMonitor(MDT_EFFECTIVE_DPI)`,
`EnumDisplaySettingsExW` (refresh, rotation, depth) and `QueryDisplayConfig` (friendly name). Coordinates are physical pixels
and need a per-monitor-aware process (the Boson manifest provides that).

**Change events.** `DisplayWatcher` listens to `WM_DISPLAYCHANGE` and `WM_SETTINGCHANGE` (including `SPI_SETWORKAREA`)
through a hidden top-level window, plus `WM_DPICHANGED` of an optional `core::Window` (via `add_message_hook`, never
consumed). It coalesces notifications (150 ms) and calls back only when the snapshot actually changed, with a
`diff_displays` result (added / removed / changed).

```cpp
auto screen_watch = boson::ipc::watch_screen(bridge, &window); // emits "screen:changed"
```

## 6. Window state persistence — `boson/desktop/window_state.hpp`

```cpp
window.create(instance, SW_HIDE);
boson::desktop::WindowStateKeeper state(window, {.app_name = L"My.App"}); // %LOCALAPPDATA%My.Appwindow-state-main.json
state.restore(show_cmd);   // saved place, or just show(show_cmd) on first run
```

- The saved data is the restored (non-maximized) rect in screen pixels, `maximized`, `fullscreen`, the monitor id and its DPI,
  read through `GetWindowPlacement`. Workspace coordinates are converted, and while the window is fullscreen the last windowed
  geometry is kept.
- Saving: debounced (400 ms) after Move / Resize / Maximize / Restore / ExitSizeMove / DpiChanged, and synchronously
  on CloseRequested and WM_DESTROY. Writes are atomic (temp file + `MoveFileExW`). Set `options.runner` to run the
  debounced writes on a TaskRunner thread.
- Restoring: `fit_window_state` fits the state to the monitors present now. It uses the saved monitor if it still exists,
  else the one with the most overlap, else the first. It rescales the size when the DPI differs, shrinks the window
  to the work area, and moves it so the title bar is reachable. The state is then applied with
  `SetWindowPlacement` (a second pass corrects the size after a cross-DPI move) and fullscreen is re-entered.
- Lower-level helpers: `load_window_state`, `save_window_state`, `default_window_state_path`, `current_monitor_areas`.

`examples/demo/main.cpp` uses it, with the state saved under `%LOCALAPPDATA%\Boson.Demo`.

## 7. JavaScript API

Register the controllers with the router and the TsGenerator (the lead integrates them into `tools/tsgen`):

```cpp
#include <boson/ipc/controllers/clipboard_controller.hpp>
#include <boson/ipc/controllers/shell_controller.hpp>
#include <boson/ipc/controllers/screen_controller.hpp>
#include <boson/ipc/controllers/shortcut_controller.hpp>
#include <boson/ipc/controllers/desktop_events.hpp>   // DesktopEvents for generate_with_events

boson::ipc::ClipboardController clipboard_ctl(window.hwnd());
boson::ipc::ShellController shell_ctl;                       // optional OpenExternalOptions
boson::ipc::ScreenController screen_ctl(window);
boson::desktop::GlobalShortcuts shortcuts;                   // UI thread
boson::ipc::Bridge* bridge_ptr = nullptr;                    // the Bridge is created after the Router
boson::ipc::ShortcutController shortcut_ctl(shortcuts, marshal, [&bridge_ptr](const boson::ipc::ShortcutTriggeredEvent& e) {
    if (bridge_ptr != nullptr) bridge_ptr->emit("shortcut:triggered", e);
});
boson::ipc::Router<boson::ipc::SystemController, boson::ipc::ClipboardController, boson::ipc::ShellController,
                   boson::ipc::ScreenController, boson::ipc::ShortcutController>
    router(system_ctl, clipboard_ctl, shell_ctl, screen_ctl, shortcut_ctl);
boson::ipc::Bridge bridge(runner, marshal, router);
bridge_ptr = &bridge;
// (ShortcutController(shortcuts, marshal, bridge) is the shorthand when a Bridge already exists.)
auto clip_watch = boson::ipc::watch_clipboard(bridge);
auto screen_watch = boson::ipc::watch_screen(bridge, &window);
```

| Method | Params | Result |
|---|---|---|
| `Clipboard.read_text` | – | `{text: string \| null}` |
| `Clipboard.write_text` | `{text, exclude_from_history}` | – |
| `Clipboard.read_html` / `write_html` | `{html, text?, source_url, exclude_from_history}` | `{available, html, fragment, source_url}` |
| `Clipboard.read_image` / `write_image` | `{png_base64, exclude_from_history}` | `{available, png_base64, width, height}` |
| `Clipboard.read_files` / `write_files` | `{paths}` | `{paths}` |
| `Clipboard.clear`, `formats`, `has` | `has: {format}` | `formats: {sequence_number, formats:[{id,name}]}`, `has: {value}` |
| `Shell.open_external` | `{url}` | – |
| `Shell.open_path` / `show_item_in_folder` / `move_to_trash` | `{path}` | – |
| `Shell.get_known_folder` | `{name}` | `{path}` |
| `Screen.get_all_displays` | – | `{displays: Display[]}` |
| `Screen.get_primary_display` / `get_window_display` | – | `Display` |
| `Screen.get_display_nearest_point` / `get_display_matching` | `{x,y}` / `{x,y,width,height}` | `Display` |
| `Screen.get_cursor_screen_point` | – | `{x,y}` |
| `Shortcut.register` | `{accelerator}` | `{ok, id, accelerator, error, message}`; error is `invalid` / `already_registered` / `conflict` / `system` |
| `Shortcut.unregister` | `{id}` | `{value}` |
| `Shortcut.unregister_all`, `is_registered` (`{accelerator}`), `list` | | `{value}` / `{shortcuts:[{id,accelerator}]}` |

Failures (invalid path, scheme not allowed, clipboard busy, ...) reject the Promise with the C++ message (code -32000).
The JS name `Shortcut.register` maps to C++ `register_shortcut` through `glz::meta`, because `register` is a keyword. A build with
C++26 reflection enabled exposes public member names, so there it would be `register_shortcut`.

Events:

| Event | Payload |
|---|---|
| `clipboard:changed` | `{sequence_number, has_text, has_html, has_image, has_files}` |
| `screen:changed` | `{reason: 'display-change'\|'work-area'\|'dpi'\|'settings', displays, added, removed, changed}` (ids) |
| `shortcut:triggered` | `{id, accelerator}` |

`frontend/src/desktop.ts` wraps all of this:

```ts
import { clipboard, shell, screen, shortcuts, ShortcutError } from './desktop';

await clipboard.writeText('hi', { excludeFromHistory: true });
await clipboard.writeImage(canvas.toDataURL('image/png'));
clipboard.onChanged((e) => e.has_image && refreshPreview());
await shell.openExternal('https://example.com');
const downloads = await shell.getPath('downloads');
const displays = await screen.getAllDisplays();
screen.onChanged((e) => relayout(e.displays));
try {
  const id = await shortcuts.register('CommandOrControl+Shift+K', () => openPalette());
} catch (e) {
  if (e instanceof ShortcutError && e.kind === 'conflict') showToast('Shortcut used by another app');
}
```

## Verification

Tested on this machine (Windows 11 Pro 26300, one 2560x1440 monitor at 100%, 180 Hz) by
`build/wt/boson_desktop_tests.exe`: **292/292 checks pass**. The build has zero warnings at /W4, `boson_tests` passes and
`boson_integration_tests` passes 108/108. `frontend` type-checks with `tsc --noEmit`.

Exercised for real:
- **Clipboard:** text (non-BMP UTF-8), HTML (+ alternative text), PNG image, a DIB-only image (as Paint leaves it),
  file lists, history exclusion formats, clear, formats(), has_format, and the change listener receiving
  WM_CLIPBOARDUPDATE. The JSON round trip goes through `Router`. The user's clipboard is saved before the test and restored
  afterwards; GDI-handle-only formats such as CF_BITMAP and metafiles are re-synthesised by Windows from the DIB data.
- **Pure helpers:** CF_HTML offsets with multibyte UTF-8, broken-offset fallback, base64 vectors, and WIC PNG↔DIBV5
  round trips with alpha preserved. Also 24-bit and alpha-less 32-bit DIBs.
- **Shell:** URL validation and the allow-list (`open_external` is only called with a rejected URL), known folders, error
  paths, and `move_to_trash` on a temp file. The test then finds the item in the Recycle Bin and deletes it permanently.
- **Shortcuts:** parser and canonical round trip; real `RegisterHotKey` of Ctrl+Alt+Shift+F23/F22, keys no keyboard has; duplicate
  detection; an OS-level conflict between two registries; unregister and re-register. Dispatch is tested by
  posting `WM_HOTKEY`. `ShortcutController` is tested through `Router` from a TaskRunner thread, marshalled to the UI thread.
- **Deep links:** register a unique `boson-test-<pid>` scheme under HKCU, verify the registry values and
  is_default_protocol_client, check that another program's registration is refused unless forced, then remove it. Also
  `extract_deep_links` and `deep_link_forwarder`.
- **Screen:** enumeration invariants, primary and nearest lookups, controller JSON, `diff_displays`, and that the watcher stays
  quiet when the configuration did not change.
- **Window state:** pure fitting cases (monitor removed with DPI rescale, oversized, off the top, off the right, unknown
  monitor), JSON load/save including invalid files, and a real hidden/no-activate window: debounced save after a move,
  final save on destroy, restore into a new window at the exact rect, and restore of a state from a missing monitor
  far off-screen.
- **tsgen:** `TsGenerator` emits all four controllers and the three events.

Not verified here:
- `open_external`, `open_path` and `show_item_in_folder` were never run with valid input (they open a browser or Explorer window,
  which would disturb parallel UI tests). They are thin wrappers over ShellExecuteExW and SHOpenFolderAndSelectItems.
- No physical key press of a global shortcut (SendInput is forbidden in the shared test environment). The OS
  registration and the WM_HOTKEY dispatch path are tested separately.
- A real `screen:changed` event: no monitor could be attached or removed, and no scale change made, on this
  single-monitor machine. The diff logic and the message plumbing are tested; multi-monitor and mixed-DPI restore are
  covered only by the pure `fit_window_state` tests.
- Launching the app through a registered `myapp://` link from the shell or a browser; only the registry contents and the argv
  helpers are tested.
- Maximized and fullscreen restore of a visible window: these would activate a window. They are covered by the
  placement logic and the JSON round trip only.
