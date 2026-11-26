// boson_native_example: the native platform pieces end to end (docs/NATIVE.md).
//  - Dialog.*: open / save / folder pickers and a task dialog with a "don't ask again" checkbox
//  - Permissions: per-origin policy persisted to %LOCALAPPDATA%\Boson.NativeExample\permissions.json, with
//    the page's JS prompt hook answering through a native message box
//  - Web Notifications -> Windows toasts (click focuses the window and fires notification.onclick)
//  - Zoom.*, Native.metrics / locale / system_preferences, shutdown blocking, close-to-tray
//  - browser_args: --max-active-webgl-contexts=32 and --js-flags
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <boson/boson.hpp>

namespace {

constexpr std::string_view kPage = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Boson native example</title>
<style>
body{font:14px "Segoe UI",sans-serif;margin:16px;background:#1f1f1f;color:#eee}
button{margin:3px;padding:6px 10px}pre{background:#111;padding:8px;max-height:280px;overflow:auto}
[data-boson-reduced-motion="true"] *{transition:none!important;animation:none!important}
</style></head><body>
<h3>Boson native platform pieces</h3>
<div>
<button onclick="show(call('Dialog.open',{title:'Open',multiple:true,filters:[{name:'Text',extensions:['txt','md']},{name:'All',extensions:['*']}]}))">Open…</button>
<button onclick="show(call('Dialog.save',{title:'Save',default_name:'notes.txt',filters:[{name:'Text',extensions:['txt']}]}))">Save…</button>
<button onclick="show(call('Dialog.pick_folder',{title:'Pick a folder'}))">Folder…</button>
<button onclick="show(call('Dialog.message_box',{title:'Boson',message:'Save changes?',detail:'Two files changed.',type:'warning',buttons:['Save','Don\'t save','Cancel'],checkbox_label:'Don\'t ask again'}))">Message box</button>
<button onclick="show(call('Dialog.error_box',{title:'Something failed',content:'Details here.'}))">Error box</button>
</div><div>
<button onclick="notify()">Web notification</button>
<button onclick="show(call('Permissions.list'))">Permission rules</button>
<button onclick="show(call('Permissions.clear',{origin:''}))">Forget permissions</button>
<button onclick="show(call('Zoom.zoom_in'))">Zoom +</button>
<button onclick="show(call('Zoom.zoom_out'))">Zoom −</button>
<button onclick="show(call('Zoom.reset'))">Zoom reset</button>
</div><div>
<button onclick="show(call('Native.metrics'))">App metrics</button>
<button onclick="show(call('Native.locale'))">Locale</button>
<button onclick="show(call('Native.system_preferences'))">System preferences</button>
<button onclick="show(call('Native.set_shutdown_block_reason',{reason:'Unsaved changes'}))">Block shutdown</button>
<button onclick="show(call('Native.set_shutdown_block_reason',{reason:''}))">Unblock</button>
<button onclick="show(call('Native.hide_to_tray'))">Hide to tray</button>
</div>
<pre id="out">WebGL contexts allowed: try creating 20 — see console.</pre>
<script>
let next = 1; const pending = new Map(); const listeners = {};
const wv = window.chrome.webview;
wv.addEventListener('message', (e) => {
  const m = typeof e.data === 'string' ? JSON.parse(e.data) : e.data;
  if (m.type === 'event') { (listeners[m.event] || []).forEach(f => f(m.data)); log(m.event, m.data); return; }
  const p = pending.get(m.id); if (!p) return; pending.delete(m.id);
  m.status === 'success' ? p.resolve(m.data) : p.reject(m.error);
});
function call(method, params) { const id = next++; wv.postMessage({ id, method, params: params || {} });
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject })); }
function on(event, f) { (listeners[event] = listeners[event] || []).push(f); }
function log(label, data) { const o = document.getElementById('out'); o.textContent = label + ' ' + JSON.stringify(data, null, 1) + '\n' + o.textContent; }
function show(p) { p.then(r => log('result', r), e => log('error', e)); }
async function notify() {
  const perm = await Notification.requestPermission();
  if (perm !== 'granted') { log('notification permission', perm); return; }
  const n = new Notification('Build finished', { body: 'boson_native_example: 0 errors', tag: 'build' });
  n.onclick = () => log('page', 'notification.onclick');
}
// JS prompt hook: "ask" requests come here; answer with a native task dialog.
on('permission:prompt', async (p) => {
  const r = await call('Dialog.message_box', { title: 'Permission request', message: p.origin + ' wants ' + p.kind,
    type: 'question', buttons: ['Allow', 'Block'], cancel_id: 1, checkbox_label: 'Remember this decision' });
  await call('Permissions.respond', { id: p.id, decision: r.response === 0 ? 'allow' : 'deny', remember: r.checkbox_checked });
});
call('Permissions.set_prompt_handler', { enabled: true });
call('Native.system_preferences').then(p => document.documentElement.dataset.bosonReducedMotion = String(p.reduced_motion));
on('system:preferences-changed', p => document.documentElement.dataset.bosonReducedMotion = String(p.reduced_motion));
</script></body></html>)HTML";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/index.html") {
        return std::span<const std::byte>{reinterpret_cast<const std::byte*>(kPage.data()), kPage.size()};
    }
    return std::nullopt;
}

std::filesystem::path app_data_dir() {
    PWSTR raw = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw != nullptr) {
        dir = std::filesystem::path(raw) / L"Boson.NativeExample";
    }
    CoTaskMemFree(raw);
    return dir;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }
    // Unpackaged toasts need a registered AppUserModelID (HKCU, no shortcut needed).
    boson::shell::set_app_user_model_id(L"Boson.NativeExample");
    if (!boson::shell::is_app_user_model_id_registered(L"Boson.NativeExample")) {
        boson::shell::register_app_user_model_id({.app_user_model_id = L"Boson.NativeExample", .display_name = L"Boson native example",
                                                  .icon_path = {}, .icon_background_color = {}, .activator_clsid = std::nullopt});
    }

    boson::core::Window window(boson::core::WindowConfig{.title = L"Boson native example", .width = 1000, .height = 720});
    if (!window.create(instance, show_cmd)) {
        return 1;
    }

    boson::exec::Scheduler scheduler;
    boson::exec::UiMarshal marshal([&window]() { window.post_invoke(); });
    window.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    const std::filesystem::path data = app_data_dir();
    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = (data / L"WebView2").wstring();
    boson::native::browser_args::set_max_active_webgl_contexts(env_cfg, 32);
    boson::native::browser_args::add_js_flags(env_cfg, {L"--max-old-space-size=4096"});
    const auto environment = boson::core::Environment::create(env_cfg);

    boson::core::WebViewHost host(window, environment);
    boson::core::WebViewConfig cfg;
    cfg.initial_url = "boson://app/index.html";
    boson::core::WebView& webview = host.create(std::move(cfg), boson::core::WebViewCallbacks{.asset_lookup = &lookup});

    // Permissions: notifications ask (JS hook -> native dialog), clipboard reads denied, the rest WebView2's UI.
    boson::native::PermissionManager permissions(data / L"permissions.json");
    permissions.set_fallback(boson::native::PermissionDecision::Default);
    permissions.set_default(boson::core::PermissionKind::Notifications, boson::native::PermissionDecision::Ask);
    permissions.set_default(boson::core::PermissionKind::ClipboardRead, boson::native::PermissionDecision::Deny);
    permissions.attach(webview);

    boson::native::WebNotificationHost notifications(marshal);
    notifications.attach(webview, window.hwnd());

    boson::native::TrayIcon tray(window.hwnd());
    tray.set_tooltip("Boson native example");
    boson::native::CloseToTray close_to_tray(window, tray, {.enabled = true, .restore_on_click = true, .hide_tray_when_restored = false,
                                                            .first_hide_title = "Still running", .first_hide_body = "Boson is in the tray."});
    tray.set_menu({boson::menu::MenuItem::action("show", "Show"), boson::menu::MenuItem::separator(), boson::menu::MenuItem::action("quit", "Quit")});
    tray.set_menu_listener([&close_to_tray](const boson::menu::MenuItem&, const boson::menu::MenuClick& click) {
        if (click.id == "show") {
            close_to_tray.restore();
        } else if (click.id == "quit") {
            close_to_tray.quit();
        }
    });
    close_to_tray.install_tray_callback();

    boson::ipc::DialogController dialog_controller(window, marshal);
    boson::ipc::PermissionController permission_controller(permissions, marshal);
    boson::ipc::ZoomController zoom_controller(webview, marshal);
    boson::ipc::NativeController native_controller(window, marshal, {environment});
    native_controller.set_close_to_tray(&close_to_tray);
    boson::ipc::Router<boson::ipc::DialogController, boson::ipc::PermissionController, boson::ipc::ZoomController,
                       boson::ipc::NativeController>
        router(dialog_controller, permission_controller, zoom_controller, native_controller);
    boson::ipc::Bridge bridge(runner, marshal, router);
    bridge.attach(webview);
    connect_events(permission_controller, bridge);
    connect_events(zoom_controller, bridge);
    connect_events(native_controller, bridge);
    connect_web_notifications(native_controller, notifications);

    bool quitting = false;
    window.on(boson::core::WindowEventKind::CloseRequested, [&](boson::core::WindowEvent& e) {
        if (e.cancel || quitting) {
            return; // close-to-tray already cancelled it
        }
        quitting = true;
        e.cancel = true;
        window.hide();
        boson::app::quit({environment});
    });

    const int code = boson::core::Window::run_message_loop();
    CoUninitialize();
    return code;
}
