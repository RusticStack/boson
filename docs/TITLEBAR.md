# Titlebar: native caption buttons, window menu, accessibility and OS theme

Boson's headless titlebar (`WindowConfig::headless_titlebar`) gives the page the whole window. The
`boson::titlebar` module adds what a custom titlebar needs on Windows 10/11:

- **Native caption buttons** drawn by Boson, like Electron's `titleBarOverlay`: minimize, maximize/restore
  and close, with Windows 11 visuals, Snap Layouts, top-edge resizing, inactive dimming, dark/light/high-contrast
  and RTL support, per-monitor DPI and a UI Automation tree.
- **The window menu** on right-click over drag regions and on Alt+Space, Alt+F4 inside WebViews, and working
  Restore / Move / Size / Minimize / Maximize / Close items for headless windows.
- **Frame fixes**: native left/right/bottom resize borders, a top resize grip above the WebView, and maximized
  windows clamped to the monitor work area (with auto-hide taskbar handling).
- **The OS theme in JS**: dark mode, the accent palette, high contrast and system colours (`theme:changed`).
- **An accessible HTML-button mode**: the existing Snap Layouts overlay (`registerMaximizeButton`) is a UIA
  Button named "Maximize" / "Restore".

Headers: `<boson/titlebar/titlebar.hpp>` (everything) and `<boson/ipc/controllers/titlebar_controller.hpp>` (the
JS controller). `<boson/boson.hpp>` includes both. TypeScript helpers are in `frontend/src/titlebar.ts`.

## C++ setup

```cpp
boson::core::Window window(cfg);                // cfg.headless_titlebar = true
window.create(instance, show_cmd);
// ... WebViewHost host(window, env); WebView& webview = host.create(...);

boson::titlebar::Titlebar titlebar(window);     // destroy before the Window
titlebar.attach_webview(webview);               // once the WebView is ready (Alt+Space / Alt+F4)

// Optional: caption buttons drawn by Boson.
titlebar.set_overlay(boson::titlebar::TitlebarOverlayOptions{
    .color = boson::titlebar::Rgba{0x20, 0x20, 0x20, 0xFF},
    .symbol_color = boson::titlebar::Rgba{0xFF, 0xFF, 0xFF, 0xFF},
    .height = 40,                               // DIPs
});

// JS access and events.
boson::ipc::TitlebarController titlebar_controller(window, webview, marshal, titlebar);
boson::ipc::Router<boson::ipc::SystemController, boson::ipc::TitlebarController> router(system_controller, titlebar_controller);
boson::ipc::Bridge bridge(runner, marshal, router);
bridge.attach(webview);
boson::ipc::connect_titlebar_events(titlebar, bridge); // "titlebar:overlay", "theme:changed"
```

`examples/demo/main.cpp` wires up the Titlebar, the controller and the events this way. It keeps its HTML
caption buttons, so it does not turn the overlay on.

### `Titlebar` (`include/boson/titlebar/titlebar.hpp`)

| Member | Purpose |
|---|---|
| `Titlebar(Window&, TitlebarOptions = {})` | `system_menu_on_right_click`, `system_keys`, `top_resize_grip` (all on by default) |
| `set_overlay(std::optional<TitlebarOverlayOptions>)` | Shows or updates the native caption buttons; `std::nullopt` hides them |
| `overlay_info()` / `on_overlay_changed(fn)` | Geometry in DIPs: the free titlebar area (the `env(titlebar-area-*)` equivalent) and the buttons rect |
| `theme()` / `theme_watcher()` | The current `ThemeInfo`; register `on_changed` listeners |
| `attach_webview(WebView&)` / `detach_webview` | Registers `AcceleratorKeyPressed`: Alt+Space shows the window menu, Alt+F4 closes. Call it again after `Environment::recreate` |
| `show_system_menu(POINT screen)` / `show_system_menu_at_caption()` | Shows the window menu, with item states matching the window |
| `handle_system_key(SystemKeyAction)` | Runs an Alt+Space or Alt+F4 action |
| `set_menu_tracker(MenuTracker)` | Replaces `TrackPopupMenuEx` (for tests or custom menus) |
| `caption_buttons()`, `top_grip_hwnd()` | Access to the parts |

`TitlebarOverlayOptions` fields:

- `color`: the background, transparent by default.
- `symbol_color` and `hover_color`.
- `height`: DIPs, 32 by default.
- `button_width`: DIPs, 46 by default.
- `scheme`: `System`, `Light` or `Dark`.
- `rtl`: defaults to the window's `WS_EX_LAYOUTRTL`.

### Native caption buttons (`CaptionButtons`)

- The buttons are a per-pixel-alpha layered child window (`UpdateLayeredWindow`) at the top-right of the client
  area. In RTL it sits at the top-left, with Close first. It is tagged `BosonKeepOnTop`, so
  `WebView::bring_to_front` and new WebView containers keep it above the WebView HWNDs.
- Glyphs come from **Segoe Fluent Icons** on Windows 11 and **Segoe MDL2 Assets** on Windows 10: E921 minimize,
  E922 maximize, E923 restore and E8BB close. They are drawn at 10 DIP with grayscale antialiasing. Vector glyphs
  are drawn when neither font exists.
- Colours follow the WinUI caption resources:
  - Hover: `#0FFFFFFF` on dark, `#09000000` on light.
  - Pressed: `#0AFFFFFF` / `#06000000`.
  - Close: `#C42B1C`. When pressed, the fill is at 90 % and the glyph is white at 70 %.
  - Inactive window: glyphs at 36 % alpha.
  - **Contrast themes**: only system colours (`ButtonFace`, `ButtonText`, `Highlight`, `HighlightText`,
    `GrayText`). Custom colours are ignored, as on the native caption.
- Hit testing returns `HTMINBUTTON`, `HTMAXBUTTON` (Windows 11 **Snap Layouts**, the same mechanism as the HTML
  overlay) or `HTCLOSE`. In the top `SM_CYSIZEFRAME` pixels of a restored window it returns `HTTOP` /
  `HTTOPRIGHT` and forwards the press to the top-level window, so resizing still works over the buttons.
  Right-click opens the window menu.
- The buttons are laid out again on `Resize`, `DpiChanged` (using the DPI from the event), `Maximize` /
  `Restore` (the glyph and accessible name switch to Restore), `Focus` / `Blur` and theme changes. They are
  hidden while the window is fullscreen.

### Window menu, keyboard and frame

- Right-click on a headless window (`WM_NCRBUTTONUP` with `HTCAPTION`) shows the window menu at the cursor. This
  works on `titlebar_height` areas, on the caption buttons, and on CSS `app-region: drag` under composition
  hosting (`WebViewHost` maps it to `HTCAPTION`). Double-clicking the caption still maximizes (DefWindowProc).
- Alt+Space while the top-level window has focus (`SC_KEYMENU`) shows the menu under the titlebar: at the client
  origin plus the caption height, mirrored in RTL.
- While a WebView has focus, `attach_webview` catches Alt+Space and Alt+F4 through `AcceleratorKeyPressed`. It
  marks them handled and posts the action, so no modal loop runs inside the WebView2 callback.
- `update_system_menu_state` enables Restore, Move, Size, Minimize and Maximize according to the window state.
  Headless windows keep `WS_OVERLAPPEDWINDOW`, so DefWindowProc runs all of them, including keyboard Move and Size.
- `WindowConfig::native_resize_borders` is new and defaults to `true`. It keeps the invisible left, right and
  bottom resize borders of headless windows non-client, as a native Windows 11 frame does (and as Windows
  Terminal does), so they work above WebView HWNDs:
  - The client area is narrower by 2 x (`SM_CXSIZEFRAME` + `SM_CXPADDEDBORDER`) and shorter by one frame.
  - Layered (Translucent) windows are not affected.
- The **top resize grip** is an invisible, alpha-1 layered child strip, `SM_CYSIZEFRAME` high, across the top
  of a restored headless window. It answers `HTTOP` / `HTTOPLEFT` / `HTTOPRIGHT` and forwards the press.
  Double-clicking it maximizes the window vertically.
- **Maximized windows**: `WM_NCCALCSIZE` strips the off-screen frame and clamps the client area to the monitor
  work area. If an edge of the monitor hosts an auto-hide taskbar (`ABM_GETAUTOHIDEBAREX`), a 2 px strip is left
  so that taskbar can still be revealed.

### Theme (`theme.hpp`)

`query_theme()` reads:

- `AppsUseLightTheme`, `SystemUsesLightTheme` and `EnableTransparency`.
- DWM `ColorPrevalence` and `AccentColor`.
- The Explorer `AccentPalette` (Light1-3 / Dark1-3).
- `SPI_GETHIGHCONTRAST` and `GetSysColor`.

`ThemeWatcher` reads these again on `WM_SETTINGCHANGE`, `WM_THEMECHANGED`, `WM_SYSCOLORCHANGE` and
`WM_DWMCOLORIZATIONCOLORCHANGED`, coalesced over 40 ms, and notifies listeners only when a value changed.

## JavaScript

The `Titlebar` controller (`TitlebarController`):

| Method | Params | Result |
|---|---|---|
| `Titlebar.set_overlay` | `{enabled, color, symbol_color, hover_color, height, button_width, theme, rtl}` | Overlay info |
| `Titlebar.get_overlay` | none | Overlay info |
| `Titlebar.get_theme` | none | Theme info |
| `Titlebar.show_system_menu` | `{x, y}` (CSS px of the page) or `{at_cursor: true}` | none |

A malformed colour is rejected with a JSON error (`Invalid CSS colour for color: ...`).

Events:

- `titlebar:overlay`: `{visible, titlebar_area:{x,y,width,height}, buttons:{x,y,width,height}, dpi, rtl, maximized, active}`.
  Values are in DIPs, which are CSS px at zoom 1.
- `theme:changed`: `{dark_mode, system_dark_mode, high_contrast, high_contrast_scheme, transparency_effects,
  accent_on_title_bars, accent, accent_light[3], accent_dark[3], colors:{window, window_text, highlight,
  highlight_text, button_face, button_text, gray_text, hotlight}}`. Colours are `#rrggbb`.

```ts
import { setTitlebarOverlay, watchTitlebarOverlay, watchTheme, enableDragRegionSystemMenu } from './titlebar';

await setTitlebarOverlay({ color: '#00000000', symbol_color: '#ffffff', height: 36 });
watchTitlebarOverlay();       // --boson-titlebar-area-{x,y,width,height}, --boson-caption-buttons-width on <html>
watchTheme();                 // data-boson-theme, data-boson-high-contrast, --boson-accent*, --boson-sys-*
enableDragRegionSystemMenu(); // page-level fallback: contextmenu on app-region: drag -> window menu
```

```css
#titlebar { app-region: drag; height: var(--boson-titlebar-area-height, 32px);
            padding-inline-end: var(--boson-caption-buttons-width, 0px); }
```

## Accessibility

- **Native buttons**: the overlay HWND serves a UIA fragment tree through `WM_GETOBJECT` /
  `UiaReturnRawElementProvider`.
  - The root is a `TitleBar` named after the window title.
  - It has three `Button` children with the Invoke pattern and AutomationIds `Minimize`, `Maximize` and `Close`.
    Like native caption buttons, they are not keyboard focusable. They report screen bounding rectangles.
  - Names are **localized** from the OS window menu strings, for example "Minimizar / Maximizar / Restaurar /
    Fechar" on Portuguese Windows.
  - While the window is maximized, Maximize is named Restore. A Name property-changed event is raised when UIA
    clients are listening.
  - Invoke posts the action and never blocks.
- **HTML-button mode**: `BosonCaptionOverlay` (from `set_maximize_button_rect` / `registerMaximizeButton`)
  exposes the same tree with one Button, named "Maximize" or "Restore". Mark your HTML button
  `aria-hidden="true"` if screen readers should not announce it twice.
- Providers are disconnected (`UiaDisconnectProvider`) when their overlay is destroyed.

## Comparison

The Electron, Tauri and Wails columns come from their public documentation as I understand it. None of them was
run on this machine. "?" marks items I could not confirm.

| Capability | Boson | Electron `titleBarStyle:'hidden'` + `titleBarOverlay` | Tauri v2 `decorations: false` | Wails v3 frameless |
|---|---|---|---|---|
| Native-drawn caption buttons over web content | Yes (layered child, Win11 visuals) | Yes (Window Controls Overlay) | No, you draw HTML buttons | No, you draw HTML buttons (?) |
| Configurable colour / symbol colour / height | Yes, plus hover colour and button width, changeable at runtime | Yes (`color`, `symbolColor`, `height`; `setTitleBarOverlay`) | n/a | n/a |
| Snap Layouts on maximize | Yes, in native and HTML modes | Yes for the overlay (?) | Not built in (community plugins) | ? |
| Titlebar-area geometry for the page | `titlebar:overlay` event and CSS variables | `env(titlebar-area-*)`, `navigator.windowControlsOverlay` | n/a | n/a |
| Inactive-window dimming of the buttons | Yes | Yes (?) | n/a | n/a |
| High-contrast palette for the buttons | Yes (system colours) | ? | n/a | n/a |
| RTL mirroring of the buttons | Yes (from `WS_EX_LAYOUTRTL` or explicit) | ? | n/a | n/a |
| UIA names / Invoke for caption buttons | Yes, localized, in both modes | ? | Only what your HTML exposes | Only what your HTML exposes |
| Right-click on drag region shows the window menu | Yes on HTCAPTION areas, plus a page fallback helper | `system-context-menu` event (Windows) | Not built in (?) | ? |
| Alt+Space while web content has focus | Yes (`AcceleratorKeyPressed`) | ? | ? | ? |
| Resize borders on a frameless window | Native side/bottom frames plus a top grip | Yes | Yes (built-in undecorated resizing) | Yes (?) |
| Maximized window clamped to the work area / auto-hide taskbar | Yes / yes (2 px reveal strip) | Yes (?) | Yes (?) / ? | ? |
| OS theme in JS | Dark mode, accent palette, high contrast, system colours | `nativeTheme` (dark, high contrast), `systemPreferences.getAccentColor` | `theme()` / `onThemeChanged` (light/dark) | Theme-change events (?) |

## Verification

All of the following ran in `boson_titlebar_tests` on this machine (Windows 11 Pro 10.0.26300, a 96 DPI
monitor, Portuguese UI, WebView2 runtime installed). 278 of 278 checks passed.

- **Pure logic**:
  - CSS colour parsing, menu-string stripping and system-key classification.
  - The `WM_NCCALCSIZE` math: work-area clamp, auto-hide edges, a secondary monitor at negative coordinates,
    restored frames.
  - Caption layout and hit testing at 96, 144 and 192 DPI: left-to-right, RTL in a mirrored parent, and RTL in a
    non-mirrored parent.
  - Palettes: dark, light, forced, custom and high contrast.
  - Pixel composition: background, glyph, close-hover red, pressed, inactive alpha.
- **Real windows** (`SW_SHOWNOACTIVATE`):
  - Headless hit testing on every border and corner, the caption and the client area.
  - Native side and bottom frame sizes.
  - The top grip: geometry and hit codes, hidden when maximized.
  - A maximized client area equal to the monitor work area.
  - System menu item states.
  - Double-clicking the caption maximizes; `SC_RESTORE` and `SC_MINIMIZE` work.
  - The right-click menu opens at the cursor and runs the chosen command.
  - The Alt+Space (`SC_KEYMENU`) menu position, and the posted WebView system-key actions (menu, close).
  - Native frame hit testing is unchanged.
- **Caption buttons**:
  - Placement and hit codes: the buttons, `HTTOP` / `HTTOPRIGHT`, and no resize band when maximized.
  - Rendered glyphs (Segoe Fluent Icons was found), close hover red, inactive dimming.
  - Press and release maximizes; releasing elsewhere cancels. Right-click opens the window menu.
  - Simulated `WM_DPICHANGED` at 144 and 192 DPI: button size, window size, grip height, DIP info.
  - RTL placement and hit codes.
  - Theme changes through `WM_SETTINGCHANGE`: the high-contrast palette is applied, and unchanged settings are
    not announced again.
  - Hidden while fullscreen.
- **UI Automation**, through `IUIAutomation` on a worker thread:
  - Native buttons: the TitleBar root, three Buttons, lookup by AutomationId, localized names, bounds, not
    focusable, and Invoke on Maximize, Restore (the name changes), Minimize and Close.
  - HTML overlay: the single Maximize button was found, invoked, and renamed to Restore.
- **WebView2**, composition hosting:
  - A CSS `app-region: drag` area hit-tests as `HTCAPTION`, and right-click opens the window menu.
  - `attach_webview` succeeds.
  - Over the bridge: `Titlebar.get_overlay`, `set_overlay` (shown, height 40), `get_theme`, the invalid-colour
    error and `show_system_menu` all work, and the `titlebar:overlay` and `theme:changed` events reach the page.
- `boson_tests` and `boson_integration_tests` (108/108) still pass, and `frontend/src/titlebar.ts` type-checks.

Not verified:

- **The Snap Layouts flyout itself, and real mouse hover or press.** The tests send window messages; no real
  input was injected.
- **Real Alt+Space and Alt+F4 keystrokes inside a WebView.** The `AcceleratorKeyPressed` handler is registered,
  and the classifier and the posted actions are tested, but no keys were injected.
- **Right-click on a CSS drag region under windowed hosting.** It is not tested whether the WebView2 child HWND
  forwards the click as `WM_NCRBUTTONUP` / `HTCAPTION`. `enableDragRegionSystemMenu()` is the page-level
  fallback.
- **Real DPI changes** (only simulated through `WM_DPICHANGED`), **a real auto-hide taskbar** (only the math),
  **a real contrast theme** (only an injected `ThemeInfo`), and **Windows 10** with the Segoe MDL2 fallback.
- **Keyboard Move and Size from the window menu.** They enter modal loops and were not driven.
- **Screen readers** (Narrator, NVDA) were not run; only the UIA tree was inspected.
- **RTL under a real RTL Windows display language**, and the resize-corner codes in a mirrored window.
- **Composition hosting.** DirectComposition content is drawn above child windows, so the native caption buttons
  are not visible over composition-hosted WebViews, although hit testing still works. Use the buttons with
  windowed hosting.
- **The demo** was built but not launched.
