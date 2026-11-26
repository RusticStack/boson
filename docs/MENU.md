# Menus, accelerators and tray

Native Win32 menus for Boson: an application menu bar built from a declarative `MenuItem` tree, context
menus, keyboard accelerators that also work while a WebView2 has focus, and a notification-area (tray) icon.
Everything is UI-thread code with no background threads; popups run the native modal menu loop, which keeps
pumping messages, so the UI never freezes.

| Header | What it gives you |
| --- | --- |
| `boson/menu/menu.hpp` | `MenuItem`, `Menu` (tree + HMENU), roles, `popup_menu()`, dark menu theme |
| `boson/menu/menu_bar.hpp` | `MenuBar`: a window's application menu (native bar or headless popups) |
| `boson/menu/accelerator.hpp` | `Accelerator` parsing, `AcceleratorTable` (message loop + WebView2) |
| `boson/native/tray.hpp` | `TrayIcon` (supersedes `SystemTray`, which is now an alias), `load_icon_from_image()` |
| `boson/ipc/controllers/menu_controller.hpp` | `MenuBackend` + `MenuController` (JS `Menu.*`), events |
| `frontend/src/menu.ts` | typed JS helpers, including an HTML title-bar menu |

## Menu items

```cpp
using namespace boson::menu;
std::vector<MenuItem> items{
    MenuItem::sub("&File", {
        MenuItem::action("open", "&Open...", "Ctrl+O", [](const MenuClick&) { open_file(); }),
        MenuItem::separator(),
        MenuItem::sub("Open &Recent", {MenuItem::action("recent.1", "notes.md")}, "recent"),
        MenuItem::from_role(MenuRole::Quit),               // "E&xit", closes the window
    }, "file"),
    MenuItem::sub("&Edit", {
        MenuItem::from_role(MenuRole::Undo),               // "&Undo  Ctrl+Z" -> WebView editing command
        MenuItem::from_role(MenuRole::Copy),
        MenuItem::from_role(MenuRole::Paste),
        MenuItem::from_role(MenuRole::SelectAll),
    }, "edit"),
    MenuItem::sub("&View", {
        MenuItem::checkbox("minimap", "&Minimap", true),
        MenuItem::radio("theme.dark", "&Dark", "theme", true),
        MenuItem::radio("theme.light", "&Light", "theme", false),
        MenuItem::from_role(MenuRole::ToggleDevTools),
    }, "view"),
};
```

Fields: `id`, `label` (`&` mnemonics, `&&` literal), `accelerator` (display text and shortcut), `type`
(`Normal | Separator | Checkbox | Radio | Submenu`, submenu inferred from children), `role`, `enabled`,
`checked`, `visible`, `radio_group` (empty = the run of adjacent radio items), `submenu`, `on_click`.

Roles fill in a default label and accelerator, and run when the item has no `on_click`:
`Undo Redo Cut Copy Paste PasteAndMatchStyle Delete SelectAll` (sent to the WebView's focused element as
Chromium editing commands through CDP `Input.dispatchKeyEvent`), `Reload ForceReload ToggleDevTools ResetZoom
ZoomIn ZoomOut` (WebView), `ToggleFullscreen Minimize Maximize Close ShowWindow HideWindow Quit` (window).
Targets come from a `RoleContext{window, webview, on_quit}`.

`Menu` owns the tree and its HMENU. `set_enabled / set_checked / set_label / set_accelerator` patch the live
HMENU in place; `set_visible`, `set_items` and `rebuild()` rebuild it (new command ids, `generation()`
increments). Checking a radio item unchecks its group. Callbacks may replace the whole tree.

## Application menu (`MenuBar`)

```cpp
boson::menu::MenuBar bar(window);                       // Auto: native bar, or popups for headless windows
bar.set_items(std::move(items));
bar.set_role_context({.window = &window, .webview = &webview});
bar.attach_webview(webview);                            // shortcuts typed inside the WebView reach the bar
bar.set_click_listener([](const MenuItem& item, const MenuClick& click) { /* every click */ });
bar.set_enabled("open", false);                         // dynamic updates, accelerators re-sync automatically
```

* **Native mode** (`SetMenu`): WM_COMMAND is handled through `Window::add_message_hook`. With a dark theme
  (default when `WindowConfig::dark_mode`) the bar strip is owner-drawn dark via the undocumented
  `WM_UAHDRAWMENU / WM_UAHDRAWMENUITEM` messages, and dropdowns use the uxtheme dark menu mode.
* **Popups mode** (`headless_titlebar` / `borderless` windows): no HMENU is attached. The HTML title bar draws
  its own buttons and calls `open_submenu(index, anchor_rect_client_px)`; the submenu drops below the button
  (`TPM_VERTICAL` + exclude rect). After `set_anchor_rects()` (one rect per entry), moving the mouse over
  another button switches menus (WH_MSGFILTER hook + `EndMenu`), Left/Right arrows move between menus, and
  Alt+mnemonic / F10 typed inside the WebView open the matching submenu.
* Item accelerators are bound in `bar.accelerators()` (an `AcceleratorTable`) except editing roles, which
  stay display-only because the WebView already handles Ctrl+C/V/X/Z/A natively. Add your own shortcuts to
  the same table; they survive `rebuild()`.

## Context menus

```cpp
boson::menu::Menu menu({MenuItem::from_role(MenuRole::Cut), MenuItem::from_role(MenuRole::Copy)});
auto chosen = boson::menu::popup_menu(menu, {
    .owner = window.hwnd(),
    .screen_point = std::nullopt,            // cursor
    .theme = boson::menu::MenuTheme::Dark,   // SetPreferredAppMode/AllowDarkModeForWindow/FlushMenuThemes
    .roles = {.window = &window, .webview = &webview},
});
if (chosen) { /* chosen->id, chosen->checked */ }
```

`popup_menu` uses `TrackPopupMenuEx(TPM_RETURNCMD)` with the given owner and returns `std::nullopt` when
dismissed, when another popup is already open on the thread, or when the menu was rebuilt while open.
`set_menu_theme(hwnd, theme)` returns false (and changes nothing) on Windows builds without the uxtheme
ordinals (before 1809); menus are then light.

## Accelerators

```cpp
boson::menu::AcceleratorTable keys(window);
keys.add("Ctrl+Shift+P", [] { show_command_palette(); });
keys.add(Accelerator{KeyModifiers::Ctrl, VK_OEM_PLUS}, [] { zoom_in(); });
keys.attach_webview(webview);     // ICoreWebView2Controller::add_AcceleratorKeyPressed, marks matches Handled
```

`Accelerator::parse` accepts `Ctrl/Control/Cmd/CmdOrCtrl`, `Alt/Option`, `Shift`, `Win/Super/Meta` plus a key:
letters, digits, `F1..F24`, `Enter Tab Esc Space Backspace Delete Insert Home End PageUp PageDown`, arrows,
`Plus Minus , . / ; ' [ ] \\ Backquote`, `Num0..Num9`, media keys. Keys posted to the window are caught by a
thread `WH_GETMESSAGE` hook before `TranslateMessage`, so `Window::run_message_loop` (and any custom loop)
needs no change and window.cpp is untouched. Keys typed in a focused WebView arrive through
AcceleratorKeyPressed. In both cases the callback is deferred with a `PostMessage`, never run inside the hook
or COM event, so it may open dialogs or menus. Auto-repeat of a bound chord is swallowed unless
`set_allow_repeat(true)`.

## Tray

```cpp
boson::native::TrayIcon tray(window.hwnd());
tray.set_icon_from_bytes(std::as_bytes(std::span{kTrayPng}));   // PNG/ICO bytes, e.g. from #embed
tray.set_tooltip("Boson");
tray.set_menu({MenuItem::from_role(MenuRole::ShowWindow), MenuItem::separator(), MenuItem::from_role(MenuRole::Quit)});
tray.set_role_context({.window = &window});
tray.set_event_callback([](const boson::native::TrayEvent& e) { /* LeftClick, RightClick, DoubleClick, ... */ });
tray.show();
tray.show_balloon({.title = "Done", .body = "Export finished"});
```

`TrayIcon` owns a hidden top-level window (message-only windows do not receive broadcasts), uses
`NOTIFYICON_VERSION_4` with `NIF_SHOWTIP`, re-adds the icon on `TaskbarCreated` (Explorer restart; the message
is allowed through UIPI for elevated processes), reloads byte-sourced icons on DPI / icon-metric changes, and
shows its menu on right click (optionally left click) after `SetForegroundWindow` as required for tray menus.
`load_icon_from_image(bytes, size)` decodes any WIC format into an alpha icon. `SystemTray` remains as an alias
with the old `show(owner, tooltip)` / `update_tooltip` / `hide` API.

## JavaScript (`Menu.*`)

Wiring (UI thread):

```cpp
boson::ipc::MenuBackend menu_backend(window, webview);          // owns the MenuBar and the lazy TrayIcon
boson::ipc::MenuController menu_controller(menu_backend, marshal);
boson::ipc::Router<boson::ipc::SystemController, boson::ipc::MenuController> router(system_controller, menu_controller);
boson::ipc::Bridge bridge(runner, marshal, router);
bridge.attach(webview);
menu_backend.set_bridge(&bridge);                               // enables "menu:click" / "tray:click"
menu_backend.set_quit_handler([&] { window.close(); });         // optional
```

Methods: `set_application_menu({items})`, `clear_application_menu()`, `get_application_menu()`,
`set_application_menu_anchors({rects})`, `popup_application_menu({id | index, rect?})`,
`popup_context_menu({items, x?, y?})` (CSS px of the page; omitted = cursor),
`update_menu_item({id, target: 'application'|'tray', enabled?, checked?, visible?, label?, accelerator?})`,
`tray_show({tooltip, icon_base64, menu, menu_on_left_click})`, `tray_hide()`, `tray_set_menu({items})`,
`tray_set_tooltip({tooltip})`, `tray_set_icon({icon_base64})`, `tray_show_balloon({title, body, level, silent})`.
Invalid types, roles or accelerators reject the Promise with the C++ error message.

Popup methods resolve with `{selected, id, checked}` when the user picks or dismisses. They hold one pool
thread while the menu is open (the UI thread keeps pumping inside the native menu loop), so size the
`Scheduler` accordingly if many popups can be pending.

Events: `MenuBackend` emits through `Bridge::emit` to every attached WebView:

* `"menu:click"` `{id, source: "application"|"context"|"tray"|"accelerator", checked}` for every click,
  whichever surface it came from (C++ `on_click` handlers still run first);
* `"tray:click"` `{button: "left"|"right"|"double"|"middle"|"keyboard"|"balloon-click"|"balloon-dismissed", x, y}`.

`boson::ipc::MenuEvents` is the event registry for `TsGenerator::generate_with_events`.

```ts
import { setApplicationMenu, attachTitlebarMenu, bindContextMenu, onMenuClick, showTray } from './menu';

await setApplicationMenu([
  { id: 'file', label: '&File', submenu: [{ id: 'open', label: '&Open...', accelerator: 'Ctrl+O' }, { role: 'quit' }] },
  { id: 'edit', label: '&Edit', submenu: [{ role: 'undo' }, { role: 'redo' }, { type: 'separator' }, { role: 'copy' }, { role: 'paste' }] },
]);
await attachTitlebarMenu(document.querySelector('#menubar')!);   // headless title bar: buttons + native dropdowns
bindContextMenu(document, (e) => (e.target as HTMLElement).closest('.file') ? [{ id: 'rename', label: 'Re&name' }] : null);
onMenuClick(({ id, source }) => console.log(id, source));
await showTray({ tooltip: 'Boson', menu: [{ role: 'showWindow' }, { role: 'quit' }] });
```

## Verification

Automated, on Windows 11 (build 26300) with the installed WebView2 runtime: `build\wt\boson_menu_tests.exe`
(327 checks, all passing), plus the existing `boson_tests` and `boson_integration_tests` still passing.

* Accelerator parsing/formatting; `AcceleratorTable` dispatch, repeat handling, enable/disable, removal from
  inside a callback, the WebView decision path with modifiers (thread keyboard state via `SetKeyboardState`),
  and the real message-loop path: `WM_KEYDOWN` posted to a window is consumed by the hook and never reaches the
  window procedure; two tables on one thread.
* `Menu`: HMENU structure and text (mnemonics, accelerator column), in-place enable/check/label/accelerator
  patches checked with `GetMenuState` / `GetMenuString`, radio groups (named and adjacent runs), disabled
  ancestors, rebuilds, a callback replacing the tree, roles on a window (Close/Quit/Minimize/HideWindow).
* Real `TrackPopupMenuEx` popups (context, menu-bar submenus, tray menu) driven by keys posted to the menu
  window from a thread timer: selection, dismissal, nested submenus, checkbox state, `dispatch=false`.
* `MenuBar` native: `GetMenu`, `WM_COMMAND` routing, accelerator binding and re-sync, user bindings kept on
  rebuild, clear. Popups mode on a headless window: anchored dropdowns, Left/Right switching between menus
  (wrapping past leaf entries), leaf entries, Alt+mnemonic from the WebView path.
* `TrayIcon` against a fake `Shell_NotifyIconW` (`set_shell_function`): add/version/modify/delete sequence,
  flags, tooltip truncation, icons, `TaskbarCreated` re-registration (including the stale-icon retry), every
  version-4 callback event, balloon reuse, menu on right/left click, the `SystemTray` alias.
* `load_icon_from_image` with WIC-encoded PNG, ICO containing PNG, ICO containing a 32bpp DIB, garbage.
* `MenuController` through the `Router` + `TaskRunner` + `UiMarshal` (pool thread -> UI thread): every method,
  error envelopes for bad templates, popup results, tray calls; `TsGenerator` output for the controller and
  `MenuEvents` (recursive `MenuItemTemplate`).
* With a real WebView2: `add_AcceleratorKeyPressed` registration/removal, the SelectAll role through CDP
  (input selection becomes 0..11), zoom roles, and a full JS round trip: `chrome.webview.postMessage` ->
  `Menu.popup_context_menu` -> native popup -> Promise response and a `"menu:click"` event in the page.
* `frontend/src/menu.ts` type-checks with `tsc --noEmit` (the repository's TypeScript from the main checkout).

Not verified automatically (needs real user input, the real shell, or visual inspection):

* Keys physically typed into a focused WebView reaching `AcceleratorKeyPressed` (CDP-injected keys do not go
  through the browser accelerator path and `SendInput` is off-limits in the shared test environment); only
  the handler logic and the registration are tested.
* Hover-switching between title-bar menus (needs a real mouse position in the menu loop's `MSG::pt`).
* The real notification area: `Shell_NotifyIconW` was replaced by a fake to avoid creating per-executable
  `HKCU\Control Panel\NotifyIconSettings` entries; an actual Explorer restart; tray menu dismissal behaviour
  that depends on `SetForegroundWindow` (disabled in tests); balloon display.
* The look of dark menus and of the owner-drawn dark menu bar (the paint paths run in tests, pixels were not
  inspected). The UAH messages and uxtheme ordinals 133/135/136 are undocumented; on builds that lack them Boson
  falls back to the light system rendering.
* Copy/Cut/Paste roles were not exercised to avoid touching the user's clipboard (they use the same CDP path
  as SelectAll, which is tested).
