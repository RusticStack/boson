// Terminal demo: split panes of native ConPTY terminals rendered by xterm.js + WebGL (examples/terminal_demo/web).
//
//   boson_terminal_demo.exe                    cmd.exe and PowerShell side by side
//   boson_terminal_demo.exe --selftest=out.png  drives the page (echo in every pane, hidden-pane replay), saves a
//                                              capture and exits 0 / 1 (used to verify the demo end to end)
//
// Wiring: BinaryTransport (shared-buffer output) + TerminalHost (PTYs, flow control, fast input path) +
// TerminalController / TransferController through the Bridge. Assets are compiled in (generated header).
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <boson/boson.hpp>

#include "terminal_demo_assets.hpp"

namespace {

class ComApartment {
public:
    ComApartment() noexcept : m_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(m_hr)) {
            CoUninitialize();
        }
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }

private:
    HRESULT m_hr;
};

std::optional<std::span<const std::byte>> find_asset(std::string_view path) {
    if (path.empty() || path == "/") {
        path = "/index.html";
    }
    for (const auto& asset : boson::terminal_demo::kAssets) {
        if (asset.path == path) {
            return std::as_bytes(asset.data);
        }
    }
    return std::nullopt;
}

void log_line(std::string_view text) {
    OutputDebugStringA(std::string(text).append("\n").c_str());
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    const ComApartment com;
    if (!com.ok()) {
        return 1;
    }
    std::wstring selftest_png;
    bool selftest = false;
    for (const auto& arg : boson::app::command_line_args()) {
        if (arg.starts_with(L"--selftest")) {
            selftest = true;
            if (arg.size() > 11 && arg[10] == L'=') {
                selftest_png = arg.substr(11);
            }
        }
    }
    // A WIN32 app has no console: when the parent gave no stdout (run from Explorer / cmd), attach to its console.
    const HANDLE inherited_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (selftest && (inherited_out == nullptr || inherited_out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* out = nullptr;
        static_cast<void>(freopen_s(&out, "CONOUT$", "w", stdout));
    }

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson terminals";
    window_cfg.width = 1280;
    window_cfg.height = 760;
    window_cfg.min_width = 640;
    window_cfg.min_height = 400;
    window_cfg.dark_mode = true;
    boson::core::Window window(std::move(window_cfg));
    if (!window.create(instance, selftest ? SW_SHOWNOACTIVATE : show_cmd)) {
        return 1;
    }

    boson::exec::Scheduler scheduler;
    boson::exec::UiMarshal marshal([&window]() { window.post_invoke(); });
    window.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::EnvironmentConfig env_cfg;
    const std::shared_ptr<boson::core::Environment> environment = boson::core::Environment::create(env_cfg);
    boson::core::WebViewHost host(window, environment);

    boson::transfer::BinaryTransport transport(runner, marshal);
    boson::terminal::TerminalHost terminals(runner, transport);
    boson::ipc::TerminalController terminal_controller(terminals);

    boson::core::WebView* webview_ptr = nullptr;
    boson::core::WebViewConfig webview_cfg;
    webview_cfg.initial_url = selftest ? "boson://app/index.html#selftest" : "boson://app/index.html";
    // The transfer runtime must exist before the page scripts run (it survives reloads this way too).
    webview_cfg.scripts_on_document_created.push_back(boson::transfer::BinaryTransport::page_script());
    boson::core::WebView& webview = host.create(
        std::move(webview_cfg),
        boson::core::WebViewCallbacks{
            .asset_lookup = &find_asset,
            .on_ready = [&]() {
                if (webview_ptr != nullptr) {
                    transport.attach(*webview_ptr, {.inject_page_runtime = false});
                    terminals.attach(*webview_ptr);
                }
            },
        });
    webview_ptr = &webview;

    boson::ipc::TransferController transfer_controller(transport, webview);
    boson::ipc::Router<boson::ipc::TerminalController, boson::ipc::TransferController> router(terminal_controller, transfer_controller);
    boson::ipc::Bridge bridge(runner, marshal, router);
    bridge.attach(webview);
    terminals.set_bridge(&bridge);

    int exit_code = 0;
    bool quitting = false;
    const auto quit = [&](int code) {
        if (quitting) {
            return;
        }
        quitting = true;
        exit_code = code;
        terminals.set_bridge(nullptr);
        window.hide();
        boson::app::quit({environment}, std::chrono::seconds{3}, code);
    };

    // --selftest: the page reports {"demo_selftest":{"ok":..,...}}; capture the window, then quit.
    boson::core::UiTimer selftest_timeout;
    if (selftest) {
        webview.add_web_message_observer([&](const boson::core::WebView::WebMessage& m) {
            if (!m.json.starts_with(R"({"demo_selftest")")) {
                return false;
            }
            log_line(std::string("selftest: ").append(m.json));
            const bool ok = m.json.find(R"("ok":true)") != std::string_view::npos;
            // Quit outside the WebView2 callback: quit() closes the controller and waits for the browser process.
            if (selftest_png.empty()) {
                marshal.post_task([&quit, ok]() { quit(ok ? 0 : 1); });
            } else {
                boson::webview::capture_preview_to_file(webview, boson::webview::ImageFormat::Png, selftest_png, [&marshal, &quit, ok](HRESULT hr) {
                    marshal.post_task([&quit, ok, hr]() { quit(ok && SUCCEEDED(hr) ? 0 : 1); });
                });
            }
            return true;
        });
        selftest_timeout.start(std::chrono::seconds{60}, [&]() {
            log_line("selftest: timed out");
            quit(2);
        });
    }

    window.on(boson::core::WindowEventKind::CloseRequested, [&](boson::core::WindowEvent& e) {
        e.cancel = true;
        quit(0);
    });

    const int loop_code = boson::core::Window::run_message_loop();
    return selftest ? exit_code : loop_code;
}
