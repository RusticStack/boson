// Multi-window demo: a main window that pops out a dashboard (own WebView, same browser process), opens a
// modal settings dialog, sends messages to the dashboard by label, and a dashboard that asks before closing
// and hides to the tray-style hidden state instead of closing. Closing the last window quits.
//
//   boson_windows_demo.exe            interactive
//   boson_windows_demo.exe --smoke    pops the dashboard out automatically, checks it, exits (0 = ok)
#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boson/boson.hpp>

namespace {

// Tiny promise-based client of the Boson IPC protocol (the bundled frontend uses frontend/src/windows.ts).
constexpr std::string_view kClient = R"JS(
<script>
const pending = new Map(); let nextId = 1; const listeners = {};
chrome.webview.addEventListener('message', (e) => {
  const d = e.data;
  if (d && d.type === 'event') { (listeners[d.event] || []).forEach((f) => f(d.data)); return; }
  const p = pending.get(d.id); if (!p) return; pending.delete(d.id);
  d.status === 'success' ? p.resolve(d.data) : p.reject(new Error(d.error.message));
});
const rpc = (method, params = {}) => new Promise((resolve, reject) => {
  const id = nextId++; pending.set(id, { resolve, reject });
  chrome.webview.postMessage({ id, method, params });
});
const on = (event, f) => { (listeners[event] ||= []).push(f); };
const log = (text) => { const li = document.createElement('li'); li.textContent = text; document.getElementById('log').prepend(li); };
const label = window.__BOSON_WINDOW__ ? window.__BOSON_WINDOW__.label : '?';
</script>
<style>
  body { font: 14px system-ui; margin: 0; padding: 16px; background: #1e1f24; color: #e7e7ea; }
  button { margin: 4px 6px 4px 0; padding: 6px 12px; border-radius: 6px; border: 1px solid #555; background: #2d2f36; color: inherit; }
  ul { padding-left: 18px; max-height: 240px; overflow: auto; color: #b9bcc6; }
</style>
)JS";

const std::string kMain = std::string("<!doctype html><html><head><title>Boson windows</title>") + std::string(kClient) + R"HTML(</head><body>
<h2>Main window</h2>
<button id="pop">Pop out dashboard</button>
<button id="send">Send to dashboard</button>
<button id="settings">Settings (modal)</button>
<button id="list">List windows</button>
<ul id="log"></ul>
<script>
let n = 0;
document.getElementById('pop').onclick = () => rpc('Window.create', {
  label: 'dashboard', url: 'dashboard.html', title: 'Dashboard (pop-out)', width: 520, height: 420,
  persist_state: true, hide_on_close: true,
}).then((w) => log('created ' + w.label), (e) => rpc('Window.show', { label: 'dashboard', focus: true }).then(() => log('shown again: ' + e.message)));
document.getElementById('send').onclick = () => rpc('Window.emit', { target: 'dashboard', event: 'demo:message', payload: { n: ++n, from: label } })
  .then(() => log('sent #' + n));
document.getElementById('settings').onclick = () => rpc('Window.create', {
  label: 'settings', url: 'settings.html', title: 'Settings', width: 360, height: 240, modal: true,
}).catch((e) => log(e.message));
document.getElementById('list').onclick = () => rpc('Window.list').then((r) => log(r.windows.map((w) => w.label + (w.visible ? '' : ' (hidden)')).join(', ')));
['created', 'closed', 'shown', 'hidden'].forEach((k) => on('window:' + k, (e) => log('window:' + k + ' ' + e.label)));
if (window.__BOSON_SMOKE__) { setTimeout(() => { document.getElementById('pop').click(); }, 50); }
</script></body></html>)HTML";

const std::string kDashboard = std::string("<!doctype html><html><head><title>Dashboard</title>") + std::string(kClient) + R"HTML(</head><body>
<h2>Dashboard</h2>
<p>Pop-out window <b id="me"></b>. Closing asks first and then hides (hide_on_close); the main window can show it again.</p>
<label><input type="checkbox" id="dirty" checked> unsaved changes</label>
<ul id="log"></ul>
<script>
document.getElementById('me').textContent = label;
on('demo:message', (m) => log('message #' + m.n + ' from ' + m.from));
rpc('Window.set_close_guard', { label: '', enabled: true });
on('window:close-requested', (e) => {
  const allow = !document.getElementById('dirty').checked || confirm('Discard unsaved changes?');
  rpc('Window.confirm_close', { label: '', request_id: e.request_id, allow });
});
</script></body></html>)HTML";

const std::string kSettings = std::string("<!doctype html><html><head><title>Settings</title>") + std::string(kClient) + R"HTML(</head><body>
<h3>Settings (modal)</h3><p>The main window is disabled until this closes.</p>
<button onclick="rpc('Window.close', { label: '' })">Done</button><ul id="log"></ul>
</body></html>)HTML";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    const auto bytes = [](const std::string& s) { return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()); };
    if (path == "/index.html") return bytes(kMain);
    if (path == "/dashboard.html") return bytes(kDashboard);
    if (path == "/settings.html") return bytes(kSettings);
    return std::nullopt;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }
    const std::vector<std::wstring> args = boson::app::command_line_args();
    const bool smoke = !args.empty() && args.front() == L"--smoke";

    boson::core::WebViewConfig page;
    page.initial_url = "boson://app/index.html";
    page.settings.non_client_region_support_enabled = true;

    // One shared Environment (created with the first WebView), state files under %LOCALAPPDATA%\Boson Windows Demo
    // (a temp folder for --smoke, so unattended runs leave the user's saved placement alone).
    wchar_t temp[MAX_PATH + 1]{};
    GetTempPathW(MAX_PATH, temp);
    boson::app::WindowManager wm({
        .instance = instance,
        .asset_lookup = &lookup,
        .default_webview = page,
        .app_name = L"Boson Windows Demo",
        .state_directory = smoke ? std::filesystem::path(temp) / L"boson-windows-demo-smoke" : std::filesystem::path{},
    });
    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&wm]() { wm.wake_ui(); }); // survives any window closing
    wm.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    // Pages may create exactly these labels.
    auto policy = boson::security::CapabilityPolicy::make_shared(boson::security::CapabilityConfig{
        .default_deny = false,
        .capabilities = {boson::security::Capability{
            .identifier = "demo-windows", .description = "pop-outs", .origins = {"boson://app"}, .allow = {}, .deny = {},
            .windows = {"dashboard", "settings"}}},
    });
    if (!policy) {
        return 1;
    }
    boson::ipc::WindowController window_controller(wm, marshal, {.policy = *policy});
    boson::ipc::SystemController system_controller(marshal, boson::ipc::sender_resolver(wm)); // Boson.* -> calling window
    system_controller.set_quit_handler([&wm]() { wm.quit(); });
    boson::ipc::Router<boson::ipc::WindowController, boson::ipc::SystemController> router(window_controller, system_controller);
    boson::ipc::Bridge bridge(runner, marshal, router);
    wm.set_bridge(&bridge);

    boson::app::ManagedWindowConfig main_cfg;
    main_cfg.label = "main";
    main_cfg.window.title = L"Boson — multi-window demo";
    main_cfg.window.width = 720;
    main_cfg.window.height = 520;
    main_cfg.persist_state = !smoke;
    main_cfg.focus = !smoke;
    main_cfg.webview = page;
    if (smoke) {
        main_cfg.webview->scripts_on_document_created.emplace_back(L"window.__BOSON_SMOKE__ = true;");
    }
    if (!wm.create(std::move(main_cfg))) {
        return 1;
    }

    int exit_code = 0;
    boson::core::UiTimer smoke_timer;
    if (smoke) {
        const auto started = std::chrono::steady_clock::now();
        smoke_timer.start(std::chrono::milliseconds{100}, [&]() {
            boson::app::ManagedWindow* dashboard = wm.find("dashboard");
            const bool ready = dashboard != nullptr && dashboard->main_webview() != nullptr && dashboard->main_webview()->is_ready();
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (ready || elapsed > std::chrono::seconds{30}) {
                smoke_timer.stop();
                const auto env = wm.environment();
                std::size_t browsers = 0;
                for (const auto& info : env->process_infos()) {
                    browsers += info.kind == boson::core::ProcessKind::Browser ? 1 : 0;
                }
                exit_code = (ready && browsers == 1 && env->webview_count() == 2) ? 0 : 2;
                std::printf("smoke: dashboard %s, %zu browser process(es), %zu WebViews -> %s\n", ready ? "ready" : "missing",
                            browsers, env->webview_count(), exit_code == 0 ? "OK" : "FAIL");
                std::fflush(stdout);
                wm.quit(exit_code);
            }
        }, true);
    }

    const int loop_exit = boson::core::Window::run_message_loop();
    wm.set_bridge(nullptr);
    CoUninitialize();
    return smoke ? exit_code : loop_exit;
}
