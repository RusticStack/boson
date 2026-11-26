# Boson Developer Usage Guide

This guide walks through building a full Windows desktop application with **Boson**.

---

## 1. Project Structure

```text
Boson/
├── CMakeLists.txt
├── cmake/
│   ├── EmbedAssets.cmake      # Generates constexpr asset fallback when #embed is unavailable
│   └── FetchWebView2.cmake    # Downloads Microsoft.Web.WebView2 NuGet & exposes WebView2::Static
├── include/boson/
│   ├── boson.hpp              # Umbrella header
│   ├── assets/asset_registry.hpp
│   ├── core/                  # Window, WebView, ProtocolHandler, inplace_vector, result
│   ├── exec/                  # Scheduler, TaskRunner, UiMarshal
│   └── ipc/                   # export, protocol_types, router, bridge, ts_generator
├── src/                       # Core implementations
├── examples/demo/             # Full demo app (AppController + wWinMain)
├── tools/tsgen/               # CLI tool for generating boson-bindings.d.ts
├── frontend/                  # Vite + TypeScript dashboard
└── tests/                     # CTest unit test suite
```

---

## 2. Integrating `boson_core` in CMake

Link your `WIN32` executable against `boson_core`:

```cmake
add_executable(my_app WIN32
    src/main.cpp
)
target_link_libraries(my_app PRIVATE boson_core)
```

`boson_core` automatically propagates:
- `/std:c++latest /Zc:__cplusplus /permissive- /EHsc /bigobj /utf-8 /W4`
- `NOMINMAX`, `WIN32_LEAN_AND_MEAN`, `UNICODE`, `_UNICODE`
- `glaze::glaze`, `STDEXEC::stdexec`, `WebView2::Static`, and required Win32/COM system libraries (`shlwapi`, `dwmapi`, `ole32`, `version`, etc.)
- Release LTO (`CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE`) and linker stripping (`/OPT:REF`, `/OPT:ICF`, `/DEBUG:NONE`).

---

## 3. Defining C++ IPC Controllers

A controller is a plain C++ class/struct marked with `BOSON_CONTROLLER("Name")` and a `glz::meta<T>` specialization listing exported methods:

```cpp
#include <boson/ipc/export.hpp>
#include <glaze/glaze.hpp>
#include <stdexcept>
#include <string>

struct GreetRequest {
    std::string name;
};

struct GreetResponse {
    std::string message;
};

class GreeterController {
public:
    BOSON_CONTROLLER("Greeter");

    [[nodiscard]] GreetResponse hello(GreetRequest req) const {
        if (req.name.empty()) {
            throw std::invalid_argument("name must not be empty");
        }
        return GreetResponse{.message = "Hello, " + req.name + "!"};
    }

    void ping() const {}
};

template <>
struct glz::meta<GreeterController> {
    using T = GreeterController;
    static constexpr auto value = glz::object(
        "hello", &T::hello,
        "ping", &T::ping
    );
};
```

### Supported Method Signatures

- `Ret method()` — 0 arguments, returns JSON-serializable `Ret`
- `Ret method(Arg)` — 1 Glaze-reflectable struct or primitive argument, returns `Ret`
- `void method()` — 0 arguments, returns `null` / `Promise<void>`
- `void method(Arg)` — 1 argument, returns `Promise<void>`

Both `const` and non-`const` member functions are supported.

### Exception & Error Handling

Any C++ exception thrown inside a controller method is caught by `boson::exec::TaskRunner` before crossing the COM/ABI boundary and returned to JavaScript as:

```json
{
  "id": 1,
  "status": "error",
  "error": {
    "code": -32000,
    "message": "name must not be empty"
  }
}
```

On the frontend, `boson.call(...)` rejects the Promise with a `BosonIpcError(message, code)`.

---

## 4. Wiring `Window`, `WebView`, `TaskRunner`, and `Bridge`

In `wWinMain`:

```cpp
#include <windows.h>
#include <objbase.h>
#include <boson/boson.hpp>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    boson::core::WindowConfig win_cfg;
    win_cfg.title = L"My Boson App";
    win_cfg.width = 1280;
    win_cfg.height = 800;
    win_cfg.dark_mode = true;
    win_cfg.headless_titlebar = true; // custom HTML/TS titlebar with native snap & resize
    win_cfg.backdrop = boson::core::BackdropType::Mica; // Mica, MicaAlt, Acrylic, Transparent, Translucent, None

    boson::core::Window window(std::move(win_cfg));
    if (!window.create(instance, show_cmd)) {
        return 1;
    }

    boson::exec::Scheduler scheduler;
    boson::exec::UiMarshal marshal([&window]() { window.post_invoke(); });
    window.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });

    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::WebViewConfig wv_cfg;
    wv_cfg.initial_url = "boson://app/index.html";
    wv_cfg.transparent_background = window.is_transparent_backdrop();
    // wv_cfg.dev_url = "http://localhost:5173"; // enable for Vite HMR

    boson::core::WebView webview(std::move(wv_cfg));

    boson::ipc::SystemController system(window, webview, marshal);
    GreeterController greeter;
    boson::ipc::Router<boson::ipc::SystemController, GreeterController> router(system, greeter);

    boson::ipc::Bridge bridge(webview, runner, marshal, router);

    window.set_resize_callback([&webview](int w, int h) { webview.resize(w, h); });

    if (!webview.initialize(
            window.hwnd(),
            [](std::string_view path) { return boson::assets::AssetRegistry::find(path); },
            bridge.make_message_callback(),
            [&window, &webview]() {
                const RECT rc = window.client_rect();
                webview.resize(rc.right - rc.left, rc.bottom - rc.top);
            })) {
        return 1;
    }

    const int exit_code = window.run_message_loop();
    ::CoUninitialize();
    return exit_code;
}
```

---

## 5. Generating TypeScript Bindings

`boson::ipc::TsGenerator::generate<Controllers...>()` inspects your reflected controllers and emits a `.d.ts` file containing all request/response interfaces and the `BosonApi` map:

```cpp
#include <boson/ipc/ts_generator.hpp>
std::string dts = boson::ipc::TsGenerator::generate<GreeterController>();
```

Build and run `boson_tsgen`:

```powershell
.\build\release\boson_tsgen.exe frontend\src\generated\boson-bindings.d.ts frontend\src\generated\boson-method-params.ts
```

The second (optional) file lists each method's positional parameter names (the request struct's members in
order); the Wails shim uses it so generated `Service.Method(a, b)` calls need no manual mapping (see PORTING.md).

Then call your backend from TypeScript with full type safety:

```ts
import { boson, BosonIpcError } from './boson';
import type { GreetRequest, GreetResponse } from './generated/boson-bindings';

try {
  const res = await boson.call<GreetResponse, GreetRequest>('Greeter.hello', { name: 'Boson' });
  console.log(res.message);
} catch (err) {
  if (err instanceof BosonIpcError) {
    console.error(`IPC Error ${err.code}: ${err.message}`);
  }
}
```

---

## 6. Asset Bundling & Vite HMR

- **Production (`boson://app/*`)**: Build `frontend/dist/` (`bun run build`) before configuring CMake. `cmake/EmbedAssets.cmake` packages all files in `frontend/dist/` into read-only `constexpr` byte arrays (or uses `#embed` when supported). `ProtocolHandler` serves them directly from memory via `SHCreateMemStream` with zero disk I/O.
- **Development (HMR)**: Start Vite (`cd frontend && bun run dev`) and launch the app with `--dev` (defaults to `http://localhost:5173`) or `--dev http://localhost:PORT` / `BOSON_DEV_URL`.

---

## 7. Fluent Backdrops, Headless Titlebar, Notifications & Dialogs

When `boson::ipc::SystemController` is registered in `Router`, your frontend can call `Boson.*` directly:

- **Headless Titlebar**:
  - `Boson.start_drag()` (call on `mousedown` of your custom titlebar drag element)
  - `Boson.minimize()`, `Boson.maximize_or_restore()`, `Boson.close()`, `Boson.is_maximized()`
- **Fluent Backdrops & Window State**:
  - `Boson.set_backdrop({ backdrop: 'mica' | 'mica_alt' | 'acrylic' | 'transparent' | 'translucent' | 'none' })`
  - `Boson.set_opacity({ opacity: 0.85 })`
  - `Boson.set_dark_mode({ enabled: true })`
  - `Boson.set_always_on_top({ enabled: true })`
  - `Boson.set_fullscreen({ enabled: true })`
  - `Boson.set_title({ title: 'New Title' })`
- **Native Notifications & File Dialogs**:
  - `Boson.notify({ title: 'Done', body: 'Build succeeded', level: 'info', silent: false })`
  - `Boson.open_file_dialog({ title: 'Open', allow_multiple: true, pick_folder: false })`
  - `Boson.save_file_dialog({ title: 'Save', default_name: 'out.json' })`

Ensure your CSS uses `html, body { background: transparent; }` when using `mica`, `mica_alt`, `acrylic`, or `transparent` backdrops so the DWM material shows through WebView2.

---

## 8. Production Release Builds

```powershell
cd frontend && bun run build && cd ..
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/release'
```

This produces a single executable (`WebView2LoaderStatic.lib` embedded, no `WebView2Loader.dll` needed) with LTO and debug symbols stripped: `boson_demo.exe` is 1,337,344 bytes with its frontend embedded (MSVC 14.52, measured 2026-11-26). It links the MSVC runtime dynamically, so target machines need the Visual C++ 2015-2026 redistributable (`MSVCP140.dll`, `MSVCP140_ATOMIC_WAIT.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`). Configure with `-DBOSON_STATIC_CRT=ON` to link it statically (`/MT`) instead: 1,586,688 bytes, no runtime dependency beyond Windows itself.

---

## 9. Shared Environment & Browser Arguments

`core::Environment` owns one `ICoreWebView2Environment`, which means one browser process tree. Every WebView created from it shares that process tree, its cache and its cookies.

```cpp
boson::core::EnvironmentConfig cfg;
cfg.user_data_folder = L"C:\\Users\\me\\AppData\\Local\\MyApp\\WebView2";
cfg.additional_browser_arguments = {L"--autoplay-policy=no-user-gesture-required"};
cfg.enable_features  = {L"msWebView2EnableDraggableRegions"};
cfg.disable_features = {L"msSmartScreenProtection"};   // see COMPOSITION_DRM_SPIKE.md for the DRM flags
cfg.language = L"pt-PT";
cfg.enable_tracking_prevention = true;      // optionals left unset keep the runtime default
auto env = boson::core::Environment::create(cfg);

env->when_ready([](ICoreWebView2Environment* e) { /* e == nullptr when creation failed */ });
env->process_infos();            // [{pid, kind: browser|renderer|gpu|utility|...}]
env->browser_process_id();
env->set_browser_process_exited_callback([](const boson::core::BrowserProcessExit& e) {});
```

`build_browser_arguments()` merges every `--enable-features=` and `--disable-features=` it finds, whether in `additional_browser_arguments` or in the two vectors, into one switch each. Chromium honours only the last occurrence of each switch, so passing several would silently drop features. A feature that appears in both lists ends up disabled.

**Environment options are fixed for the lifetime of a browser process.** WebView2 starts one browser process per user data folder. A second environment with *different* options on the same folder fails with `HRESULT_FROM_WIN32(ERROR_INVALID_STATE)` (`0x8007139F`). To change arguments at runtime, use one of these:

- `env->recreate(new_cfg, on_done)` closes every controller, waits for `BrowserProcessExited`, creates the new environment, and re-creates each registered WebView in place at its last URL. `on_ready` fires again for each WebView. It takes about 0.5 s in the integration test.
- `boson::app::relaunch()` restarts the whole process (see §15).

## 10. Multiple WebViews per Window

`core::WebViewHost` owns the WebViews of one `Window`. It handles:

- layout on `WM_SIZE` and `WM_DPICHANGED`;
- `NotifyParentWindowPositionChanged` on every move;
- hiding controllers while the window is minimized (`WebView::set_keep_visible_when_minimized(true)` opts a WebView out);
- z-order.

```cpp
boson::core::WebViewHost host(window, env);
host.set_default_asset_lookup([](std::string_view p) { return boson::assets::AssetRegistry::find(p); });

auto& header  = host.create({.initial_url = "boson://app/header.html"});
auto& content = host.create({.initial_url = "https://example.com", .serve_boson_assets = false});

host.set_layout([&](boson::core::WebViewHost&, const boson::core::LayoutContext& ctx) {
    const int h = ctx.px(40);                                  // 40 DIP header
    header.set_bounds({0, 0, ctx.width(), h});
    content.set_bounds({0, h, ctx.width(), ctx.height()});
});

content.set_visible(false);   // hidden WebViews are throttled
header.bring_to_front();
host.destroy(content);        // safe from inside content's own callbacks
content.set_keep_visible_when_minimized(true);   // media apps: keep playing while the window is minimized
```

A hidden controller (`IsVisible = false`) is a background page to the runtime. On runtime 158, hidden PlayReady SL3000
video is paused (the only PlayReady level tested this way), and its audio stops with it. Widevine video keeps playing. Media apps should
call `set_keep_visible_when_minimized(true)` on the WebView that plays, and should not hide it with `set_visible(false)`
while it plays; see [COMPOSITION_DRM_SPIKE.md](COMPOSITION_DRM_SPIKE.md#fullscreen-background-playback-and-screen-sharing).
PlayReady audio is not reported by `IsDocumentPlayingAudio`, so `SleepPolicy` treats a hidden PlayReady tab as silent.

In windowed mode each WebView lives inside a `BosonWebViewContainer` child HWND. The container is what provides z-order and bounds. Third-party content should set `settings.web_messages_enabled = false` and should not be attached to a `Bridge`.

## 11. Backend → Frontend Events

```cpp
struct TabsUpdated { std::vector<Tab> tabs; std::uint64_t active; };
struct AppEvents { TabsUpdated tabs_updated; };
template <> struct glz::meta<AppEvents> {
    static constexpr auto value = glz::object("tabs:updated", &AppEvents::tabs_updated);
};

boson::ipc::Bridge bridge(runner, marshal, router);
bridge.attach(header);                                    // origins: boson://app + dev_url
bridge.attach(other, {.allowed_origins = {"https://app.example.com"}});
bridge.emit("tabs:updated", TabsUpdated{...});            // any thread, all attached WebViews
bridge.emit_to(header.id(), "tabs:updated", TabsUpdated{...});
```

On the TypeScript side, `boson.on('tabs:updated', e => ...)` and `boson.once(...)` are typed from the `BosonEvents` interface. `TsGenerator::generate_with_events<AppEvents, Controllers...>()` emits that interface. Messages from origins that are not allowed are dropped and logged. RPC replies go back only to the WebView that sent the request.

## 12. Navigation, Page Control and Content Events

`WebView` exposes:

- navigation: `navigate`, `navigate_to_string`, `go_back`/`go_forward`, `can_go_*`, `reload`, `stop`, `source`, `document_title`;
- scripts: `execute_script` and `add_script_on_document_created`, each available as a callback or as a `CallbackSender` for `std::execution`;
- page control: `set_zoom_factor`, `set_muted`, `open_devtools`, `open_task_manager`, `get_favicon_png`;
- settings: `apply_settings(WebViewSettings)`.

Never `sync_wait` a WebView sender on the UI thread. It completes from the message loop.

`webview.events()` holds one `std::function` per WebView2 event:

- title, source, history, navigation starting (cancellable) and completed, favicon;
- new window (with a deferral), fullscreen, audio, muted;
- process failed, `window.close()`, permissions, downloads.

Opening `window.open` in a Boson WebView:

```cpp
tab.events().on_new_window_requested = [&](boson::core::NewWindowRequest& req) {
    auto deferral = std::make_shared<boson::core::NewWindowDeferral>(req.defer());
    auto& popup = host.create({.initial_url = ""});          // must not navigate first
    popup.set_ready_callback([deferral, &popup] { deferral->complete_with(popup); });
};
```

A `NewWindowDeferral` that is destroyed without being completed falls back to the default popup, so the page never hangs.

## 13. Suspend vs. Destroy

| | Suspend (`try_suspend` / `SleepPolicy`) | Destroy (`host.destroy`) |
|---|---|---|
| Page state (DOM, JS heap, scroll, forms) | kept | lost |
| Renderer memory | mostly kept; trimmed with `MemoryUsageTarget::Low` | released |
| Timers / network | paused | gone |
| Cost to bring back | instant (`set_visible(true)` / `resume()`) | full reload |
| Use for | background tabs the user will return to | closed tabs, long-idle tabs under memory pressure |

`host.set_sleep_policy(SleepPolicy{.hidden_timeout = 5min})` suspends WebViews that have been hidden for that long and are not playing audio. `set_sleep_exempt()` opts a WebView out, for example your UI shell. A WebView must be hidden before it can be suspended.

## 14. Window Events, DPI and Geometry

```cpp
window.on(WindowEventKind::CloseRequested, [](WindowEvent& e) { e.cancel = true; }); // veto
window.on(WindowEventKind::DpiChanged, [](WindowEvent& e) { /* e.dpi */ });
window.set_min_size(760, 520);    // DIPs
window.set_bounds(rect); window.center(); window.show(); window.hide(); window.is_minimized();
```

The available events are Resize, Move, Minimize, Maximize, Restore, Focus, Blur, DpiChanged, CloseRequested, Destroyed, EnterSizeMove, ExitSizeMove, MaximizeButtonHover and MaximizeButtonPress. Width, height and minimum size are in DIPs. `WM_DPICHANGED` applies the suggested rect. `quit_on_destroy = false` keeps secondary windows from ending the message loop.

## 15. App Regions and Snap Layouts

CSS `app-region: drag` (and `no-drag`) works when `settings.non_client_region_support_enabled = true` (runtime support via `ICoreWebView2Settings9`). Set `EnvironmentConfig::enable_draggable_regions` as well, for older runtimes.

For Windows 11 Snap Layouts on a custom maximize button, call `boson.registerMaximizeButton(el)` from TypeScript, or `window.set_maximize_button_rect(rect)` from C++. Boson then places an invisible layered child window over that rect, above the WebView's HWNDs. It answers `HTMAXBUTTON` and drives hover, press and maximize itself, and reports `MaximizeButtonHover`/`Press` so the page can style the button. Layered child windows require the Windows 10 `supportedOS` manifest. `boson_core` adds `cmake/boson.manifest` to every executable that links it.

## 16. Composition Hosting

`WebViewHost(window, env, HostingMode::Composition)` renders WebViews through `ICoreWebView2CompositionController` into a DirectComposition tree. WebViews can then overlap with real alpha (`set_opacity`, `set_corner_radius`, transparent backgrounds), which is how `boson_browser --composition` draws a translucent modal over a playing video. The host forwards mouse, wheel and cursor input and maps CSS app-regions to `WM_NCHITTEST`.

Windowed hosting stays the default. PlayReady SL3000 playback was measured in both hosting modes on one discrete-GPU machine (no iGPU machine was tested); see [Composition & DRM spike](COMPOSITION_DRM_SPIKE.md) for the results, the caveats and the `EnvironmentConfig` flags that matter. `webview::probe_drm` / `webview.probeDrm()` report which key system and robustness a WebView actually gets ([WebView features](WEBVIEW_FEATURES.md#drm-capability-probe-bosonwebviewdrmhpp)).

## 17. Lifecycle: Relaunch, Single Instance, Graceful Quit

```cpp
int WINAPI wWinMain(...) {
    boson::app::wait_for_previous_instance();          // first line: pairs with relaunch()
    auto instance = boson::app::single_instance(L"MyCompany.MyApp",
        [&](std::vector<std::wstring> argv, std::wstring cwd) { /* open argv in this instance */ });
    if (!instance) return 0;                            // forwarded to the primary
    ...
    window.on(WindowEventKind::CloseRequested, [&](WindowEvent& e) {
        e.cancel = true; window.hide();
        boson::app::quit({env});                         // closes controllers, waits for the browser process, then WM_QUIT
    });
}

boson::app::relaunch({.extra_args = {L"--screenshare"}, .remove_args = {L"--screenshare"}}, {env});
```

`relaunch` shuts the environment down, starts the executable again with `--boson-wait-pid=<pid>`, and exits. The new process waits for that PID, so it never races the old browser process for the user data folder.

## 18. Tests and Examples

- `boson_tests`: unit tests (no WebView2 runtime needed).
- `boson_integration_tests` runs against the installed runtime and opens two small windows. It covers the shared environment and process info, multi-WebView layout, navigation and history, scripts, the bridge (replies, worker-thread emits, origin filtering), suspend/resume and `SleepPolicy`, minimize visibility and `set_keep_visible_when_minimized`, `window.open` into a Boson WebView, Snap Layouts hit testing, composition hosting with app-region, `Environment::recreate` and graceful shutdown. `ctest -L integration` runs only this test.
- `boson_browser [--composition] [--screenshare] [url...]` is a multi-tab browser built on all of the above. Set `BOSON_BROWSER_LOG=<file>` to capture its event log.

## 19. Platform Modules

Each module has its own guide with API, examples and an honest verification section. Every JS-facing module is a controller you add to your `Router`; `boson_tsgen` generates types for all of them (`TsGenerator::EventMaps<...>` combines event maps, `ipc::AllBosonEvents` lists the built-in ones).

| Module | Guide | Controllers (JS name) | Events |
|---|---|---|---|
| Windows by label (multi-window, owned / modal, close veto, state per label) | [WINDOWS.md](WINDOWS.md) | `Window` | `window:created`, `window:closed`, `window:focus`, `window:blur`, `window:moved`, `window:resized`, `window:maximized`, `window:minimized`, `window:restored`, `window:shown`, `window:hidden`, `window:close-requested` |
| Menus, tray, accelerators | [MENU.md](MENU.md) | `Menu` | `menu:click`, `tray:click` |
| Clipboard, shell, shortcuts, deep links, screen, window state | [DESKTOP.md](DESKTOP.md) | `Clipboard`, `Shell`, `Screen`, `Shortcut` | `clipboard:changed`, `screen:changed`, `shortcut:triggered` |
| File drop, binary IPC, streaming | [TRANSFER.md](TRANSFER.md) | `Transfer` | — |
| Terminals (ConPTY, xterm.js, node-pty shim) | [TERMINAL.md](TERMINAL.md) | `Terminal` | `terminal:exit`, `terminal:title`, `terminal:cwd` |
| Print, PDF, capture, find, cookies, profiles | [WEBVIEW_FEATURES.md](WEBVIEW_FEATURES.md) | `WebView` | `webview:find` |
| `<boson-webview>` element, guests, CDP / Playwright endpoint | [WEBVIEW_ELEMENT.md](WEBVIEW_ELEMENT.md) | `Guest` | `guest:*` |
| Capabilities, CSP, safe storage, credentials, scoped FS | [SECURITY.md](SECURITY.md) | `Fs`, `SafeStorage`, `Credential` | `fs:changed` |
| Persistent settings (`Store<T>`) | [STORE.md](STORE.md) | `Store` (name configurable) | `store:changed` |
| Taskbar, jump lists, toasts, power, auto-launch | [SHELL_INTEGRATION.md](SHELL_INTEGRATION.md) | `Taskbar`, `Notification`, `Power`, `AutoLaunch` | `taskbar:*`, `notification:*`, `power:*` |
| Child processes, sidecars, named pipes, bundled Node | [PROCESS.md](PROCESS.md) | `Process` | `process:stdout`, `process:stderr`, `process:exit`, `process:exec`, `process:message` |
| Packaging, signing, updater, crash reporting, logging | [SHIPPING.md](SHIPPING.md) | `Updater`, `Log` | `updater:*` |
| HTTP client, ZIP, hashing, staged installs, known folders, SQLite / ZephyrDB | [NATIVE_SERVICES.md](NATIVE_SERVICES.md) | `Http`, `Db` | `http:progress`, `http:done` |
| Titlebar overlay, accessibility, theme | [TITLEBAR.md](TITLEBAR.md) | `Titlebar` | `titlebar:overlay`, `theme:changed` |
| Dialogs, message boxes, permissions, web notifications, zoom, browser args, metrics, locale / system preferences, session end, close-to-tray | [NATIVE.md](NATIVE.md) | `Dialog`, `Permissions`, `Zoom`, `Native` | `permission:*`, `zoom:changed`, `system:*`, `session:*`, `tray:window-hidden`, `web-notification:*` |
| Native GPU surfaces (D3D11/D3D12, 2D renderer, Vulkan interop) | [GPU.md](GPU.md) | `Gpu` | `gpu:device_lost` |
| Electron / Tauri / Wails shims | [PORTING.md](PORTING.md), [MIGRATION.md](MIGRATION.md) | `ElectronIpc` (dynamic) | `ipc:<channel>` |
| Sync IPC (host object), boot values, native drag-out | [SYNC_IPC.md](SYNC_IPC.md) | `SyncHost` (sync only), `Drag` | `drag:ended` |

Test executables: `boson_menu_tests`, `boson_desktop_tests`, `boson_transfer_tests`, `boson_webview_features_tests`, `boson_webview_element_tests`, `boson_security_tests`, `boson_shell_integration_tests`, `boson_shipping_tests` (+ `boson_signing_test`), `boson_titlebar_tests`, `boson_porting_tests` (+ `boson_compat_typecheck`), `boson_store_tests`, `boson_window_tests`, `boson_native_tests`, `boson_native_services_tests`, `boson_sync_tests`, `boson_process_tests`, `boson_terminal_tests` (+ `boson_terminal_demo_selftest`), `boson_gpu_tests`. Most open small windows and drive the real WebView2 runtime; run them with `ctest`.
