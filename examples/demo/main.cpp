#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <boson/boson.hpp>

#include "app_controller.hpp"

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

    int argc = 0;
    LPWSTR* raw_argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!raw_argv) {
        return opts;
    }
    const auto argv_guard = std::unique_ptr<LPWSTR, decltype(&::LocalFree)>(raw_argv, &::LocalFree);

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg{argv_guard.get()[i]};
        if (arg == L"--native-frame") {
            opts.headless_titlebar = false;
        } else if (arg == L"--borderless") {
            opts.headless_titlebar = true;
        } else if (arg.starts_with(L"--backdrop=")) {
            opts.backdrop = boson::ipc::parse_backdrop_type(boson::core::wide_to_utf8(arg.substr(11)));
        } else if (arg == L"--dev") {
            if (i + 1 < argc && argv_guard.get()[i + 1][0] != L'-') {
                opts.dev_url = boson::core::wide_to_utf8(argv_guard.get()[++i]);
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
    const ComApartmentGuard com_guard;
    if (!com_guard.ok()) {
        return 1;
    }

    const LaunchOptions opts = parse_launch_options();

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson — C++26 WebView2";
    window_cfg.width = 1220;
    window_cfg.height = 840;
    window_cfg.headless_titlebar = opts.headless_titlebar;
    window_cfg.backdrop = opts.backdrop;
    window_cfg.dark_mode = true;

    boson::core::Window window(std::move(window_cfg));
    if (!window.create(instance, show_cmd)) {
        return 1;
    }

    boson::exec::Scheduler scheduler;
    boson::exec::UiMarshal marshal([&window]() {
        window.post_invoke();
    });
    window.set_invoke_callback([&marshal]() {
        marshal.drain_on_ui_thread();
    });

    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::WebViewConfig webview_cfg;
    webview_cfg.dev_url = opts.dev_url;
    webview_cfg.initial_url = "boson://app/index.html";
    webview_cfg.transparent_background = window.is_transparent_backdrop();

    boson::core::WebView webview(std::move(webview_cfg));

    boson::ipc::SystemController system_controller(window, webview, marshal);
    boson::demo::AppController app_controller;
    boson::ipc::Router<boson::ipc::SystemController, boson::demo::AppController> router(
        system_controller,
        app_controller);

    boson::ipc::Bridge bridge(webview, runner, marshal, router);

    window.set_resize_callback([&webview](int width, int height) {
        webview.resize(width, height);
    });

    const bool initialized = webview.initialize(
        window.hwnd(),
        [](std::string_view path) {
            return boson::assets::AssetRegistry::find(path);
        },
        bridge.make_message_callback(),
        [&window, &webview]() {
            const RECT rc = window.client_rect();
            webview.resize(rc.right - rc.left, rc.bottom - rc.top);
        });

    if (!initialized) {
        return 1;
    }

    return window.run_message_loop();
}
