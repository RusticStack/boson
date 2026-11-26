// Boson multi-tab browser example (issues #2, #3, #4, #6, #7, #8, #9, #10, #11).
//
//   boson_browser.exe [--composition] [--screenshare] [url ...]
//
// One shared Environment; a 40-DIP chrome WebView (shell_html.hpp) on top and one content WebView per tab.
// window.open becomes a new tab, page fullscreen fills the window, hidden tabs sleep after five minutes,
// a second launch forwards its URLs here, and "screenshare mode" re-creates the browser process with
// --disable-direct-composition so DRM video can be shared in Discord/Teams (in place, or by relaunching the
// executable). A minimized window keeps playing (WebView::set_keep_visible_when_minimized).
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boson/boson.hpp>

#include "bench_driver.hpp"
#include "browser.hpp"
#include "shell_html.hpp"

namespace boson::browser {

namespace {

HANDLE g_console = INVALID_HANDLE_VALUE;

template <typename... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    std::string line = std::format(fmt, std::forward<Args>(args)...);
    line += '\n';
    OutputDebugStringA(line.c_str());
    if (g_console != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_console, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
}

constexpr std::string_view kNewTabHtml = R"(<!doctype html><html><head><meta charset="utf-8"><title>New tab</title>
<style>html,body{margin:0;height:100%;background:#202124;color:#e8eaed;font:14px "Segoe UI",sans-serif;display:flex;align-items:center;justify-content:center}
a{color:#8ab4f8;display:block;margin:6px 0}</style></head><body><div><h1>Boson Browser</h1>
<a href="https://www.youtube.com/">YouTube (fullscreen, audio, sleeping tabs)</a>
<a href="https://bitmovin.com/demos/drm">DRM test page (PlayReady / Widevine)</a>
<a href="https://webrtc.github.io/samples/src/content/getusermedia/gum/">getUserMedia (permission prompt)</a>
<a href="https://developer.mozilla.org/en-US/docs/Web/API/Window/open" target="_blank">target=_blank (new tab)</a>
</div></body></html>)";

std::optional<std::span<const std::byte>> as_bytes(std::string_view s) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/shell.html") return as_bytes(kShellHtml);
    if (path == "/newtab.html") return as_bytes(kNewTabHtml);
    if (path == "/bench_tab.html") return as_bytes(bench::kTabHtml);
    return std::nullopt;
}

std::string url_encode(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(c);
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out += std::format("%{:02X}", u);
        }
    }
    return out;
}

/// @brief Address-bar input to a URL: schemes pass through, "host.tld/..." gets https://, the rest is a search.
std::string normalize_url(std::string_view input) {
    while (!input.empty() && input.front() == ' ') input.remove_prefix(1);
    while (!input.empty() && input.back() == ' ') input.remove_suffix(1);
    if (input.empty()) return "boson://app/newtab.html";
    if (input.find("://") != std::string_view::npos || input.starts_with("about:") || input.starts_with("data:")) {
        return std::string(input);
    }
    if (input.find(' ') == std::string_view::npos &&
        (input.find('.') != std::string_view::npos || input.starts_with("localhost"))) {
        return "https://" + std::string(input);
    }
    return "https://duckduckgo.com/?q=" + url_encode(input);
}

} // namespace

struct Tab {
    core::WebView* webview = nullptr;
    TabInfo info;
};

/// @brief Optional per-tab observers (benchmark mode). A set message hook also enables web messages on tabs.
struct TabHooks {
    std::function<void(std::uint64_t id, bool ok)> navigated;
    std::function<void(std::uint64_t id, std::string_view json)> message;
    std::function<void(std::uint64_t id)> slept;
};

/// @brief UI-thread browser state: tabs, layout, fullscreen, modal and screenshare mode.
class Browser final : public bench::TabOps {
public:
    Browser(core::Window& window, core::WebViewHost& host, exec::UiMarshal& marshal, bool screenshare)
        : m_window(window), m_host(host), m_marshal(marshal), m_screenshare(screenshare) {}

    void set_bridge(ipc::Bridge* bridge) { m_bridge = bridge; }
    void set_shell(core::WebView* shell) { m_shell = shell; }
    void set_tab_hooks(TabHooks hooks) { m_hooks = std::move(hooks); }
    exec::UiMarshal& marshal() { return m_marshal; }

    void layout(const core::LayoutContext& ctx) {
        const int header = m_fullscreen ? 0 : ctx.px(40);
        const RECT content{0, header, ctx.width(), ctx.height()};
        for (Tab& tab : m_tabs) {
            tab.webview->set_bounds(content);
        }
        if (m_shell != nullptr) {
            // While the settings modal is open the chrome covers the whole window, above the active tab.
            m_shell->set_visible(!m_fullscreen);
            m_shell->set_bounds(RECT{0, 0, ctx.width(), m_modal ? ctx.height() : header});
            m_shell->bring_to_front();
        }
    }

    core::WebView& open_tab(std::string url, bool activate, core::NewWindowDeferral* deferral = nullptr) {
        core::WebViewConfig cfg;
        cfg.initial_url = deferral != nullptr ? std::string{} : normalize_url(url);
        cfg.start_visible = false;
        // Third-party content never reaches the bridge; only the benchmark's own tab page talks to the host.
        cfg.settings.web_messages_enabled = static_cast<bool>(m_hooks.message);
        cfg.settings.non_client_region_support_enabled = false;
        std::shared_ptr<core::NewWindowDeferral> pending;
        if (deferral != nullptr) {
            pending = std::make_shared<core::NewWindowDeferral>(std::move(*deferral));
        }
        // on_ready fires asynchronously (and again after Environment::recreate), so it is bound after
        // create() returns, when the WebView's id is known.
        core::WebView& webview = m_host.create(std::move(cfg));
        if (m_hooks.message) {
            webview.set_message_callback([this, id = webview.id()](std::string_view json, std::string_view) { m_hooks.message(id, json); });
        }
        webview.set_ready_callback([this, pending, id = webview.id()]() mutable {
            if (pending && pending->pending()) {
                if (Tab* tab = find(id)) {
                    pending->complete_with(*tab->webview);
                }
                pending.reset();
            }
            if (Tab* tab = find(id)) {
                tab->info.crashed = false;
            }
            publish();
        });
        m_tabs.push_back(Tab{.webview = &webview, .info = TabInfo{.id = webview.id(), .url = std::move(url), .loading = true}});
        wire_events(webview);
        m_host.relayout();
        if (activate || m_active == 0) {
            activate_tab(webview.id());
        } else {
            publish();
        }
        log("[tab {}] opened", webview.id());
        return webview;
    }

    void close_tab(std::uint64_t id) {
        const auto it = std::ranges::find_if(m_tabs, [id](const Tab& t) { return t.info.id == id; });
        if (it == m_tabs.end()) return;
        const std::size_t index = static_cast<std::size_t>(it - m_tabs.begin());
        core::WebView* webview = it->webview;
        m_tabs.erase(it);
        if (m_fullscreen_tab == id) set_fullscreen(id, false);
        m_host.destroy(*webview);
        log("[tab {}] closed", id);
        if (m_tabs.empty()) {
            open_tab({}, true);
            return;
        }
        if (m_active == id) {
            m_active = 0;
            activate_tab(m_tabs[std::min(index, m_tabs.size() - 1)].info.id);
        } else {
            publish();
        }
    }

    void activate_tab(std::uint64_t id) {
        Tab* next = find(id);
        if (next == nullptr) return;
        for (Tab& tab : m_tabs) {
            if (tab.info.id != id) tab.webview->set_visible(false);
        }
        next->webview->set_visible(true); // also resumes a sleeping tab
        next->info.sleeping = false;
        m_active = id;
        if (m_shell != nullptr) m_shell->bring_to_front();
        if (!m_modal) next->webview->focus();
        publish();
    }

    void navigate(std::string_view input) {
        if (Tab* tab = active()) {
            if (tab->info.crashed) tab->webview->reload();
            tab->webview->navigate(normalize_url(input));
        }
    }

    void back() { if (Tab* t = active()) t->webview->go_back(); }
    void forward() { if (Tab* t = active()) t->webview->go_forward(); }
    void reload() { if (Tab* t = active()) t->webview->reload(); }

    void toggle_mute(std::uint64_t id) {
        if (Tab* tab = find(id)) {
            tab->webview->set_muted(!tab->webview->is_muted().value_or(false));
        }
    }

    void set_modal(bool enabled) {
        m_modal = enabled;
        m_host.relayout();
        if (m_shell != nullptr && enabled) m_shell->focus();
        publish();
    }

    void on_slept(core::WebView& webview) {
        if (Tab* tab = find(webview.id())) {
            tab->info.sleeping = true;
            log("[tab {}] sleeping (hidden, no audio)", tab->info.id);
            publish();
            if (m_hooks.slept) m_hooks.slept(tab->info.id);
        }
    }

    void open_forwarded(const std::vector<std::wstring>& args) {
        bool opened = false;
        for (const std::wstring& arg : args) {
            if (!arg.starts_with(L"--")) {
                open_tab(core::wide_to_utf8(arg), true);
                opened = true;
            }
        }
        if (!opened) open_tab({}, true);
        m_window.show(SW_RESTORE);
        m_window.focus();
    }

    void set_screenshare_in_place(bool enabled) {
        const auto& env = m_host.environment();
        core::EnvironmentConfig cfg = env->config();
        apply_screenshare(cfg, enabled);
        log("[env] re-creating the browser process (screenshare {})", enabled ? "on" : "off");
        env->recreate(cfg, [this, enabled](bool ok) {
            // The new browser pid is only known once a WebView exists again (on_ready).
            log("[env] recreate {}", ok ? "succeeded" : "FAILED");
            if (ok) m_screenshare = enabled;
            publish();
        });
    }

    /// @brief Restarts the executable with the flag flipped and the open tabs passed back as URLs.
    void relaunch(bool enabled) {
        app::RelaunchOptions options;
        options.remove_args = {L"--screenshare"};
        options.keep_original_args = true;
        // Drop original positional URLs; the current tabs replace them.
        for (const std::wstring& arg : app::command_line_args()) {
            if (!arg.starts_with(L"--")) options.remove_args.push_back(arg);
        }
        if (enabled) options.extra_args.emplace_back(L"--screenshare");
        for (std::wstring& url : tab_urls()) options.extra_args.push_back(std::move(url));
        log("[app] relaunching (screenshare {}) with {} tab(s)", enabled ? "on" : "off", tab_urls().size());
        m_window.hide();
        app::relaunch(std::move(options), {m_host.environment()});
    }

    /// Screen sharing (Discord, Teams, OBS) of DRM video. Measured on runtime 158 (docs/COMPOSITION_DRM_SPIKE.md):
    /// - PlayReady video (SL3000 and SL2000) is never capturable.
    /// - Widevine is drawn into a display-only DirectComposition overlay that capture skips.
    /// --disable-direct-composition fixes both: PlayReady becomes unavailable, so sites fall back to Widevine,
    /// and Widevine is composited into the normal swap chain, where DXGI and Windows.Graphics.Capture see it.
    /// The GPU stays on (unlike --disable-gpu).
    static void apply_screenshare(core::EnvironmentConfig& cfg, bool enabled) {
        auto& args = cfg.additional_browser_arguments;
        std::erase(args, std::wstring{L"--disable-direct-composition"});
        if (enabled) {
            args.emplace_back(L"--disable-direct-composition");
        }
    }

    [[nodiscard]] BrowserState state() const {
        BrowserState s;
        s.active = m_active;
        s.hosting = m_host.hosting() == core::HostingMode::Composition ? "composition" : "windowed";
        s.screenshare_mode = m_screenshare;
        s.modal = m_modal;
        for (const Tab& tab : m_tabs) {
            s.tabs.push_back(tab.info);
            if (tab.info.id == m_active) {
                s.can_go_back = tab.webview->can_go_back();
                s.can_go_forward = tab.webview->can_go_forward();
            }
        }
        return s;
    }

    [[nodiscard]] ProcessList processes() const {
        ProcessList list;
        for (const core::ProcessInfo& info : m_host.environment()->process_infos()) {
            list.processes.push_back(ProcessEntry{.pid = info.pid, .kind = std::string(core::to_string(info.kind))});
        }
        return list;
    }

    [[nodiscard]] std::vector<std::wstring> tab_urls() const {
        std::vector<std::wstring> urls;
        for (const Tab& tab : m_tabs) {
            if (!tab.info.url.empty() && !tab.info.url.starts_with("boson://")) {
                urls.push_back(core::utf8_to_wide(tab.info.url));
            }
        }
        return urls;
    }

    [[nodiscard]] bool screenshare() const noexcept { return m_screenshare; }

    // bench::TabOps
    core::WebView& bench_open_tab(std::string url) override { return open_tab(std::move(url), true); }
    void bench_activate_tab(std::uint64_t id) override { activate_tab(id); }
    void bench_close_tab(std::uint64_t id) override { close_tab(id); }
    [[nodiscard]] std::vector<std::uint64_t> bench_tab_ids() const override {
        std::vector<std::uint64_t> ids;
        for (const Tab& tab : m_tabs) ids.push_back(tab.info.id);
        return ids;
    }
    [[nodiscard]] std::uint64_t bench_active_tab() const override { return m_active; }

private:
    Tab* find(std::uint64_t id) {
        const auto it = std::ranges::find_if(m_tabs, [id](const Tab& t) { return t.info.id == id; });
        return it == m_tabs.end() ? nullptr : &*it;
    }
    Tab* active() { return find(m_active); }

    void publish() {
        if (m_bridge != nullptr) m_bridge->emit("tabs:updated", state());
    }

    void set_fullscreen(std::uint64_t id, bool fullscreen) {
        if (fullscreen) {
            m_fullscreen_tab = id;
            activate_tab(id);
        } else if (m_fullscreen_tab == id) {
            m_fullscreen_tab = 0;
        }
        m_fullscreen = m_fullscreen_tab != 0;
        m_window.set_fullscreen(m_fullscreen);
        m_host.relayout();
    }

    void wire_events(core::WebView& webview) {
        const std::uint64_t id = webview.id();
        // A minimized browser keeps playing media: with the controller hidden, the runtime pauses
        // hardware-DRM (PlayReady) video, and its audio with it. Hidden tabs are still hidden by set_visible.
        webview.set_keep_visible_when_minimized(true);
        auto& ev = webview.events();
        ev.on_title_changed = [this, id](std::string_view title) {
            if (Tab* t = find(id)) { t->info.title = title; publish(); }
        };
        ev.on_source_changed = [this, id](std::string_view uri, bool is_new_document) {
            log("[tab {}] source {} (new document: {})", id, uri, is_new_document);
            if (Tab* t = find(id)) { t->info.url = uri; publish(); }
        };
        ev.on_history_changed = [this, id](bool back, bool fwd) {
            log("[tab {}] history back={} forward={}", id, back, fwd);
            if (id == m_active) publish();
        };
        ev.on_navigation_starting = [this, id](core::NavigationStartingEvent& e) {
            log("[tab {}] navigation starting {} (user: {}, redirect: {})", id, e.uri, e.is_user_initiated, e.is_redirected);
            if (Tab* t = find(id)) { t->info.loading = true; publish(); }
        };
        ev.on_navigation_completed = [this, id](const core::NavigationCompletedEvent& e) {
            log("[tab {}] navigation completed ok={} web_error={} http={}", id, e.is_success, e.web_error_status, e.http_status_code);
            if (Tab* t = find(id)) { t->info.loading = false; publish(); }
            if (m_hooks.navigated) m_hooks.navigated(id, e.is_success);
        };
        ev.on_favicon_changed = [id](std::string_view uri) { log("[tab {}] favicon {}", id, uri); };
        ev.on_new_window_requested = [this, id](core::NewWindowRequest& req) {
            log("[tab {}] new window {} -> new tab", id, req.uri());
            core::NewWindowDeferral deferral = req.defer();
            open_tab(req.uri(), true, &deferral);
        };
        ev.on_fullscreen_changed = [this, id](bool fullscreen) {
            log("[tab {}] fullscreen {}", id, fullscreen);
            set_fullscreen(id, fullscreen);
        };
        ev.on_audio_changed = [this, id](bool playing) {
            log("[tab {}] audio {}", id, playing ? "playing" : "stopped");
            if (Tab* t = find(id)) { t->info.audio = playing; publish(); }
        };
        ev.on_muted_changed = [this, id](bool muted) {
            log("[tab {}] muted {}", id, muted);
            if (Tab* t = find(id)) { t->info.muted = muted; publish(); }
        };
        ev.on_process_failed = [this, id](const core::ProcessFailedEvent& e) {
            log("[tab {}] process failed kind={} reason={} exit={} {}", id, static_cast<int>(e.kind), e.reason, e.exit_code, e.process_description);
            if (e.kind == core::ProcessFailedKind::RenderProcessExited || e.kind == core::ProcessFailedKind::RenderProcessUnresponsive) {
                if (Tab* t = find(id)) { t->info.crashed = true; t->info.loading = false; publish(); }
            }
        };
        ev.on_window_close_requested = [this, id]() {
            log("[tab {}] window.close()", id);
            close_tab(id);
        };
        ev.on_permission_requested = [id](core::PermissionRequest& req) {
            log("[tab {}] permission {} for {} (user: {}) -> default prompt", id, static_cast<int>(req.kind()), req.uri(), req.is_user_initiated());
        };
        ev.on_download_starting = [id](core::DownloadStartingEvent& e) {
            log("[tab {}] download {} ({}, {} bytes) -> {}", id, e.uri(), e.mime_type(), e.total_bytes().value_or(-1),
                core::wide_to_utf8(e.result_file_path()));
        };
    }

    core::Window& m_window;
    core::WebViewHost& m_host;
    exec::UiMarshal& m_marshal;
    ipc::Bridge* m_bridge = nullptr;
    core::WebView* m_shell = nullptr;
    std::vector<Tab> m_tabs;
    std::uint64_t m_active = 0;
    std::uint64_t m_fullscreen_tab = 0;
    bool m_fullscreen = false;
    bool m_modal = false;
    bool m_screenshare = false;
    TabHooks m_hooks;
};

BrowserState BrowserController::get_state() const {
    return m_browser.marshal().invoke_sync([this]() { return m_browser.state(); });
}
void BrowserController::new_tab(UrlRequest req) const {
    m_browser.marshal().post_task([this, url = std::move(req.url)]() { m_browser.open_tab(url, true); });
}
void BrowserController::close_tab(TabRequest req) const {
    m_browser.marshal().post_task([this, id = req.id]() { m_browser.close_tab(id); });
}
void BrowserController::activate_tab(TabRequest req) const {
    m_browser.marshal().post_task([this, id = req.id]() { m_browser.activate_tab(id); });
}
void BrowserController::toggle_mute(TabRequest req) const {
    m_browser.marshal().post_task([this, id = req.id]() { m_browser.toggle_mute(id); });
}
void BrowserController::navigate(UrlRequest req) const {
    m_browser.marshal().post_task([this, url = std::move(req.url)]() { m_browser.navigate(url); });
}
void BrowserController::back() const {
    m_browser.marshal().post_task([this]() { m_browser.back(); });
}
void BrowserController::forward() const {
    m_browser.marshal().post_task([this]() { m_browser.forward(); });
}
void BrowserController::reload() const {
    m_browser.marshal().post_task([this]() { m_browser.reload(); });
}
void BrowserController::set_modal(EnabledRequest req) const {
    m_browser.marshal().post_task([this, enabled = req.enabled]() { m_browser.set_modal(enabled); });
}
void BrowserController::set_screenshare_mode(EnabledRequest req) const {
    m_browser.marshal().post_task([this, enabled = req.enabled]() { m_browser.set_screenshare_in_place(enabled); });
}
void BrowserController::relaunch(EnabledRequest req) const {
    m_browser.marshal().post_task([this, enabled = req.enabled]() { m_browser.relaunch(enabled); });
}
ProcessList BrowserController::process_info() const {
    return m_browser.marshal().invoke_sync([this]() { return m_browser.processes(); });
}

} // namespace boson::browser

namespace {

class ComApartmentGuard {
public:
    ComApartmentGuard() noexcept : m_hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartmentGuard() {
        if (SUCCEEDED(m_hr)) ::CoUninitialize();
    }
    ComApartmentGuard(const ComApartmentGuard&) = delete;
    ComApartmentGuard& operator=(const ComApartmentGuard&) = delete;
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }

private:
    HRESULT m_hr = E_FAIL;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev*/, PWSTR /*cmd_line*/, int show_cmd) {
    using namespace boson;
    using browser::log;

    // Event log: the parent console when started from a terminal, or the file named by BOSON_BROWSER_LOG.
    wchar_t log_path[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"BOSON_BROWSER_LOG", log_path, MAX_PATH) > 0) {
        browser::g_console = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    } else if (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE) {
        browser::g_console = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    app::wait_for_previous_instance();

    const ComApartmentGuard com_guard;
    if (!com_guard.ok()) return 1;

    const std::vector<std::wstring> args = app::command_line_args();
    const auto has = [&args](std::wstring_view flag) { return std::ranges::find(args, flag) != args.end(); };

    // Benchmark mode (boson_bench --scenario browser): scripted scenario, no single-instance forwarding.
    std::optional<std::filesystem::path> bench_dir;
    if (has(L"--bench")) {
        wchar_t dir[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"BOSON_BENCH_DIR", dir, MAX_PATH) > 0) {
            bench_dir = std::filesystem::path{dir};
        } else {
            log("[bench] --bench needs BOSON_BENCH_DIR (run it through boson_bench --scenario browser)");
        }
    }

    browser::Browser* browser_ptr = nullptr;
    std::unique_ptr<app::SingleInstance> instance_guard;
    if (!bench_dir) {
        instance_guard = app::single_instance(L"Boson.Browser", [&browser_ptr](std::vector<std::wstring> forwarded, std::wstring /*cwd*/) {
            log("[app] second instance forwarded {} argument(s)", forwarded.size());
            if (browser_ptr != nullptr) browser_ptr->open_forwarded(forwarded);
        });
        if (!instance_guard) return 0;
    }

    const bool composition = has(L"--composition");
    const bool screenshare = has(L"--screenshare");
    std::vector<std::string> start_urls;
    for (const std::wstring& arg : args) {
        if (!arg.starts_with(L"--")) start_urls.push_back(core::wide_to_utf8(arg));
    }

    core::WindowConfig window_cfg;
    window_cfg.title = L"Boson Browser";
    window_cfg.width = 1280;
    window_cfg.height = 860;
    window_cfg.min_width = 640;
    window_cfg.min_height = 420;
    window_cfg.headless_titlebar = true;
    window_cfg.titlebar_height = 0; // the chrome WebView owns the caption via CSS app-region
    window_cfg.backdrop = core::BackdropType::Mica;
    window_cfg.dark_mode = true;
    core::Window window(std::move(window_cfg));
    if (!window.create(instance, show_cmd)) return 1;

    boson::exec::Scheduler scheduler;
    boson::exec::UiMarshal marshal([&window]() { window.post_invoke(); });
    window.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    core::EnvironmentConfig env_cfg;
    env_cfg.enable_draggable_regions = true;
    env_cfg.additional_browser_arguments = {L"--autoplay-policy=no-user-gesture-required"};
    browser::Browser::apply_screenshare(env_cfg, screenshare);
    const std::shared_ptr<core::Environment> environment = core::Environment::create(env_cfg);
    environment->set_browser_process_exited_callback([](const core::BrowserProcessExit& e) {
        log("[env] browser process {} exited (kind {})", e.pid, static_cast<int>(e.kind));
    });
    environment->set_process_infos_changed_callback([&environment]() {
        log("[env] {} browser-tree processes", environment->process_infos().size());
    });

    core::WebViewHost host(window, environment, composition ? core::HostingMode::Composition : core::HostingMode::Windowed);
    host.set_default_asset_lookup(&browser::lookup);
    log("[app] hosting: {}", host.hosting() == core::HostingMode::Composition ? "composition" : "windowed");

    browser::Browser browser(window, host, marshal, screenshare);
    browser_ptr = &browser;
    host.set_layout([&browser](core::WebViewHost&, const core::LayoutContext& ctx) { browser.layout(ctx); });

    core::WebViewConfig shell_cfg;
    shell_cfg.initial_url = "boson://app/shell.html";
    shell_cfg.transparent_background = true;
    shell_cfg.settings.non_client_region_support_enabled = true;
    shell_cfg.settings.default_context_menus_enabled = false;
    shell_cfg.settings.zoom_control_enabled = false;
    if (bench_dir) browser::bench::Driver::configure_shell(shell_cfg);
    core::WebView& shell = host.create(std::move(shell_cfg));
    host.set_sleep_exempt(shell, true);
    browser.set_shell(&shell);

    ipc::SystemController system_controller(window, shell, marshal);
    browser::BrowserController browser_controller(browser);
    browser::bench::BenchController bench_controller(marshal);
    ipc::Router<ipc::SystemController, browser::BrowserController, browser::bench::BenchController> router(
        system_controller, browser_controller, bench_controller);
    ipc::Bridge bridge(runner, marshal, router);
    bridge.attach(shell);
    browser.set_bridge(&bridge);

    std::optional<browser::bench::Driver> bench_driver;
    if (bench_dir) {
        browser::bench::Driver& driver = bench_driver.emplace(*bench_dir, window, host, browser);
        driver.set_bridge(&bridge);
        bench_controller.set_driver(&driver);
        bench_controller.set_bridge(&bridge);
        browser.set_tab_hooks(browser::TabHooks{
            .navigated = [&driver](std::uint64_t id, bool ok) { driver.on_tab_navigated(id, ok); },
            .message = [&driver](std::uint64_t id, std::string_view json) { driver.on_tab_message(id, json); },
            .slept = [&driver](std::uint64_t id) { driver.on_tab_slept(id); },
        });
        start_urls = {driver.next_tab_url()};
    }

    window.on(core::WindowEventKind::MaximizeButtonHover, [&bridge](core::WindowEvent& e) {
        bridge.emit("window:maximize-button", browser::MaxButtonEvent{.hovered = e.flag});
    });

    host.set_sleep_policy(core::SleepPolicy{});
    host.set_on_slept([&browser](core::WebView& webview) { browser.on_slept(webview); });

    if (start_urls.empty()) start_urls.emplace_back();
    for (const std::string& url : start_urls) browser.open_tab(url, true);

    bool quitting = false;
    window.on(core::WindowEventKind::CloseRequested, [&](core::WindowEvent& e) {
        if (quitting) return;
        quitting = true;
        e.cancel = true;
        window.hide();
        app::quit({environment});
    });

    return core::Window::run_message_loop();
}
