#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <boson/boson.hpp>

#include "app_controller.hpp"
#include "demo_events.hpp"

namespace {

class ComApartmentGuard {
public:
    ComApartmentGuard() noexcept
        : m_hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}

    ~ComApartmentGuard() {
        if (SUCCEEDED(m_hr)) {
            ::CoUninitialize();
        }
    }

    ComApartmentGuard(const ComApartmentGuard&) = delete;
    ComApartmentGuard& operator=(const ComApartmentGuard&) = delete;

    [[nodiscard]] bool ok() const noexcept {
        return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT m_hr = E_FAIL;
};

struct LaunchOptions {
    bool headless_titlebar = true;
    boson::core::BackdropType backdrop = boson::core::BackdropType::Mica;
    std::optional<std::string> dev_url;
};

[[nodiscard]] std::optional<std::string> get_env_utf8(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return std::nullopt;
    }
    std::wstring buf(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, buf.data(), needed);
    if (written == 0) {
        return std::nullopt;
    }
    buf.resize(static_cast<std::size_t>(written));
    std::string out = boson::core::wide_to_utf8(buf);
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] LaunchOptions parse_launch_options() {
    LaunchOptions opts;
    opts.dev_url = get_env_utf8(L"BOSON_DEV_URL");

    const std::vector<std::wstring> args = boson::app::command_line_args();
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring_view arg{args[i]};
        if (arg == L"--native-frame") {
            opts.headless_titlebar = false;
        } else if (arg == L"--borderless") {
            opts.headless_titlebar = true;
        } else if (arg.starts_with(L"--backdrop=")) {
            opts.backdrop = boson::ipc::parse_backdrop_type(boson::core::wide_to_utf8(arg.substr(11)));
        } else if (arg == L"--dev") {
            if (i + 1 < args.size() && !args[i + 1].starts_with(L"-")) {
                opts.dev_url = boson::core::wide_to_utf8(args[++i]);
            } else if (!opts.dev_url.has_value()) {
                opts.dev_url = "http://localhost:5173";
            }
        } else if (arg.starts_with(L"--dev=")) {
            opts.dev_url = boson::core::wide_to_utf8(arg.substr(6));
        }
    }
    return opts;
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE /*prev_instance*/,
    PWSTR /*cmd_line*/,
    int show_cmd) {
    // A relaunch() must not race the previous process for the WebView2 user data folder.
    boson::app::wait_for_previous_instance();

    const ComApartmentGuard com_guard;
    if (!com_guard.ok()) {
        return 1;
    }

    boson::core::Window* window_ptr = nullptr;
    const auto instance_guard = boson::app::single_instance(
        L"Boson.Demo",
        [&window_ptr](std::vector<std::wstring> /*args*/, std::wstring /*cwd*/) {
            if (window_ptr != nullptr) {
                window_ptr->focus();
            }
        });
    if (!instance_guard) {
        return 0; // forwarded to the running instance
    }

    const LaunchOptions opts = parse_launch_options();

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson — C++26 WebView2";
    window_cfg.width = 1220;
    window_cfg.height = 840;
    window_cfg.min_width = 760;
    window_cfg.min_height = 520;
    window_cfg.headless_titlebar = opts.headless_titlebar;
    window_cfg.backdrop = opts.backdrop;
    window_cfg.dark_mode = true;

    boson::core::Window window(std::move(window_cfg));
    if (!window.create(instance, show_cmd)) {
        return 1;
    }
    window_ptr = &window;

    boson::exec::Scheduler scheduler;
    boson::exec::UiMarshal marshal([&window]() {
        window.post_invoke();
    });
    window.set_invoke_callback([&marshal]() {
        marshal.drain_on_ui_thread();
    });

    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::EnvironmentConfig env_cfg;
    env_cfg.enable_draggable_regions = true; // CSS app-region on runtimes without ICoreWebView2Settings9
    const std::shared_ptr<boson::core::Environment> environment = boson::core::Environment::create(env_cfg);

    boson::core::WebViewHost host(window, environment);

    boson::core::WebViewConfig webview_cfg;
    webview_cfg.dev_url = opts.dev_url;
    webview_cfg.initial_url = "boson://app/index.html";
    webview_cfg.transparent_background = window.is_transparent_backdrop();
    webview_cfg.settings.non_client_region_support_enabled = true;

    boson::core::WebView& webview = host.create(
        std::move(webview_cfg),
        boson::core::WebViewCallbacks{
            .asset_lookup = [](std::string_view path) { return boson::assets::AssetRegistry::find(path); },
        });

    boson::ipc::SystemController system_controller(window, webview, marshal);
    boson::demo::AppController app_controller;
    boson::ipc::Router<boson::ipc::SystemController, boson::demo::AppController> router(
        system_controller,
        app_controller);

    boson::ipc::Bridge bridge(runner, marshal, router);
    bridge.attach(webview);

    // ---- Backend -> frontend events ---------------------------------------------------------------------
    const auto emit_window_state = [&window, &bridge](bool focused) {
        bridge.emit("window:state", boson::demo::WindowStateEvent{
            .maximized = window.is_maximized(),
            .minimized = window.is_minimized(),
            .focused = focused,
            .dpi = window.dpi(),
        });
    };
    using boson::core::WindowEventKind;
    window.on(WindowEventKind::Maximize, [&](boson::core::WindowEvent&) { emit_window_state(true); });
    window.on(WindowEventKind::Restore, [&](boson::core::WindowEvent&) { emit_window_state(true); });
    window.on(WindowEventKind::Minimize, [&](boson::core::WindowEvent&) { emit_window_state(false); });
    window.on(WindowEventKind::Focus, [&](boson::core::WindowEvent&) { emit_window_state(true); });
    window.on(WindowEventKind::Blur, [&](boson::core::WindowEvent&) { emit_window_state(false); });
    window.on(WindowEventKind::DpiChanged, [&](boson::core::WindowEvent&) { emit_window_state(true); });

    bool max_hovered = false;
    bool max_pressed = false;
    const auto emit_max_button = [&]() {
        bridge.emit("window:maximize-button", boson::demo::MaximizeButtonEvent{.hovered = max_hovered, .pressed = max_pressed});
    };
    window.on(WindowEventKind::MaximizeButtonHover, [&](boson::core::WindowEvent& e) {
        max_hovered = e.flag;
        emit_max_button();
    });
    window.on(WindowEventKind::MaximizeButtonPress, [&](boson::core::WindowEvent& e) {
        max_pressed = e.flag;
        emit_max_button();
    });

    // A UI timer kicks a background task each second; the task emits from its worker thread.
    std::atomic<std::uint64_t> tick_seq{0};
    boson::core::UiTimer tick_timer;
    tick_timer.start(
        std::chrono::seconds{1},
        [&runner, &bridge, &tick_seq]() {
            runner.spawn([&bridge, &tick_seq]() {
                const auto now = std::chrono::steady_clock::now().time_since_epoch();
                bridge.emit("app:tick", boson::demo::TickEvent{
                    .seq = tick_seq.fetch_add(1, std::memory_order_relaxed) + 1,
                    .timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(now).count(),
                    .worker_thread_id = static_cast<std::uint32_t>(::GetCurrentThreadId()),
                });
            });
        },
        true);

    // Graceful shutdown: close controllers and wait for the browser process before WM_QUIT.
    bool quitting = false;
    window.on(WindowEventKind::CloseRequested, [&](boson::core::WindowEvent& e) {
        if (quitting) {
            return;
        }
        quitting = true;
        e.cancel = true;
        tick_timer.stop();
        window.hide();
        boson::app::quit({environment});
    });

    return boson::core::Window::run_message_loop();
}
