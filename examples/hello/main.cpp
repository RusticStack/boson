// boson_hello: the smallest Boson app (one window, one page from memory, no IPC controllers). It is the
// reference for the single-executable build (docs/SHIPPING.md, "Single executable"):
//   cmake -DBOSON_SINGLE_EXE=ON [-DBOSON_HELLO_EMBED_BOOTSTRAPPER=ON] ... ; the exe imports only Windows DLLs.
#include <windows.h>
#include <objbase.h>

#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <boson/boson.hpp>
#include <boson/shipping/webview2_runtime.hpp>

#if defined(BOSON_EMBED_WEBVIEW2_BOOTSTRAPPER)
#include <boson_webview2_bootstrapper.hpp>
#endif

namespace {

constexpr std::string_view kPage =
    "<!doctype html><meta charset=utf-8><title>Boson</title>"
    "<body style=\"margin:0;display:grid;place-items:center;height:100vh;background:#111;color:#eee;font:20px system-ui\">"
    "<div>Hello from Boson</div>";

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev_instance*/, PWSTR /*cmd_line*/, int show_cmd) {
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }

    boson::shipping::RuntimeGuardOptions guard;
#if defined(BOSON_EMBED_WEBVIEW2_BOOTSTRAPPER)
    guard.embedded_bootstrapper = boson::shipping::embedded_bootstrapper();
#endif
    if (!boson::shipping::require_webview2_runtime(L"Boson Hello", guard)) {
        return 2;
    }

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson Hello";
    window_cfg.width = 640;
    window_cfg.height = 400;
    window_cfg.dark_mode = true;

    boson::core::Window window(std::move(window_cfg));
    if (!window.create(instance, show_cmd)) {
        return 1;
    }

    boson::exec::UiMarshal marshal([&window]() { window.post_invoke(); });
    window.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });

    const std::shared_ptr<boson::core::Environment> environment = boson::core::Environment::create({});
    boson::core::WebViewHost host(window, environment);

    boson::core::WebViewConfig webview_cfg;
    webview_cfg.initial_url = "boson://app/index.html";
    host.create(
        std::move(webview_cfg),
        boson::core::WebViewCallbacks{
            .asset_lookup = [](std::string_view path) -> std::optional<std::span<const std::byte>> {
                if (path == "/index.html" || path == "/") {
                    return std::as_bytes(std::span{kPage.data(), kPage.size()});
                }
                return std::nullopt;
            },
        });

    bool quitting = false;
    window.on(boson::core::WindowEventKind::CloseRequested, [&](boson::core::WindowEvent& e) {
        if (quitting) {
            return;
        }
        quitting = true;
        e.cancel = true;
        window.hide();
        boson::app::quit({environment});
    });

    const int code = boson::core::Window::run_message_loop();
    ::CoUninitialize();
    return code;
}
