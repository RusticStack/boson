// boson_bench_app: a minimal Boson app used by tools/bench (boson_bench) to measure startup, idle and IPC.
//
// Protocol with the harness (all optional, the app also runs standalone):
//   BOSON_BENCH_DIR=<dir>         set by boson_bench; the app writes <dir>/ready (QueryPerformanceCounter value
//                                 taken when the page reports its first frame), <dir>/load_result.json and
//                                 <dir>/load_done.
//   "BosonBench.StartLoad"        registered window message posted by boson_bench --load-signal; starts the
//                                 IPC load test in the page.
//   --auto                        run the load test right after the first frame, then close (manual runs;
//                                 the result goes to BOSON_BENCH_DIR or %TEMP%\boson_bench_app_result.json).
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

#include "bench_page.hpp"

namespace boson::bench_app {

struct PingRequest {
    std::uint64_t seq = 0;
};

struct PingResponse {
    std::uint64_t seq = 0;
};

struct DataRequest {
    std::string data;
};

struct SizeResponse {
    std::uint64_t bytes = 0;
};

struct DownloadRequest {
    std::uint64_t bytes = 0;
};

struct DataResponse {
    std::string data;
};

[[nodiscard]] inline std::optional<std::filesystem::path> bench_dir() {
    const DWORD needed = ::GetEnvironmentVariableW(L"BOSON_BENCH_DIR", nullptr, 0);
    if (needed == 0) {
        return std::nullopt;
    }
    std::wstring buf(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = ::GetEnvironmentVariableW(L"BOSON_BENCH_DIR", buf.data(), needed);
    if (written == 0 || written >= needed) {
        return std::nullopt;
    }
    buf.resize(written);
    return std::filesystem::path{buf};
}

/// Writes text to dir/name via a temp file + rename so the harness never sees a partial file.
inline bool write_atomically(const std::filesystem::path& dir, std::wstring_view name, std::string_view text) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path final_path = dir / name;
    std::filesystem::path tmp_path = final_path;
    tmp_path += L".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(tmp_path, final_path, ec);
    return !ec;
}

class BenchController {
public:
    BOSON_CONTROLLER("Bench");

    BenchController(core::Window& window, exec::UiMarshal& marshal, ipc::Bridge& bridge, bool auto_mode)
        : m_window(window), m_marshal(marshal), m_bridge(bridge), m_auto(auto_mode), m_dir(bench_dir()) {}

    [[nodiscard]] PingResponse ping(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }

    [[nodiscard]] SizeResponse upload(DataRequest req) const noexcept { return SizeResponse{.bytes = req.data.size()}; }

    [[nodiscard]] DataResponse download(DownloadRequest req) const {
        constexpr std::uint64_t kMax = 256ull * 1024 * 1024;
        if (req.bytes > kMax) {
            throw std::invalid_argument("download: at most 256 MiB");
        }
        return DataResponse{.data = std::string(static_cast<std::size_t>(req.bytes), 'y')};
    }

    [[nodiscard]] DataResponse echo(DataRequest req) const { return DataResponse{.data = std::move(req.data)}; }

    /// Called by the page after its second requestAnimationFrame, i.e. once the first frame was produced.
    void ready() {
        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);
        if (m_ready_sent.exchange(true)) {
            return;
        }
        if (m_dir) {
            write_atomically(*m_dir, L"ready", std::to_string(now.QuadPart));
        }
        if (m_auto) {
            m_bridge.emit_all("bench:start");
        }
    }

    void report(glz::raw_json result) {
        const std::filesystem::path dir = m_dir.value_or(std::filesystem::temp_directory_path());
        const std::wstring_view name = m_dir ? std::wstring_view{L"load_result.json"} : std::wstring_view{L"boson_bench_app_result.json"};
        write_atomically(dir, name, result.str);
        if (m_dir) {
            write_atomically(*m_dir, L"load_done", "1");
        }
        if (m_auto) {
            m_marshal.post_task([this]() { m_window.close(); });
        }
    }

private:
    core::Window& m_window;
    exec::UiMarshal& m_marshal;
    ipc::Bridge& m_bridge;
    bool m_auto = false;
    std::optional<std::filesystem::path> m_dir;
    std::atomic<bool> m_ready_sent{false};
};

class ComApartmentGuard {
public:
    ComApartmentGuard() noexcept : m_hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartmentGuard() {
        if (SUCCEEDED(m_hr)) {
            ::CoUninitialize();
        }
    }
    ComApartmentGuard(const ComApartmentGuard&) = delete;
    ComApartmentGuard& operator=(const ComApartmentGuard&) = delete;
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }

private:
    HRESULT m_hr = E_FAIL;
};

} // namespace boson::bench_app

template <>
struct glz::meta<boson::bench_app::BenchController> {
    using T = boson::bench_app::BenchController;
    static constexpr auto value = glz::object(
        "ping", &T::ping,
        "upload", &T::upload,
        "download", &T::download,
        "echo", &T::echo,
        "ready", &T::ready,
        "report", &T::report);
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev_instance*/, PWSTR /*cmd_line*/, int show_cmd) {
    const boson::bench_app::ComApartmentGuard com_guard;
    if (!com_guard.ok()) {
        return 1;
    }

    bool auto_mode = false;
    for (const auto& arg : boson::app::command_line_args()) {
        if (arg == L"--auto") {
            auto_mode = true;
        }
    }

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson Bench App";
    window_cfg.width = 900;
    window_cfg.height = 640;
    window_cfg.backdrop = boson::core::BackdropType::None;
    window_cfg.dark_mode = true;

    boson::core::Window window(std::move(window_cfg));
    if (!window.create(instance, show_cmd)) {
        return 1;
    }

    boson::exec::Scheduler scheduler;
    boson::exec::UiMarshal marshal([&window]() { window.post_invoke(); });
    window.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    const std::shared_ptr<boson::core::Environment> environment = boson::core::Environment::create({});
    boson::core::WebViewHost host(window, environment);

    boson::core::WebViewConfig webview_cfg;
    webview_cfg.initial_url = "boson://app/index.html";
    boson::core::WebView& webview = host.create(
        std::move(webview_cfg),
        boson::core::WebViewCallbacks{
            .asset_lookup = [](std::string_view path) -> std::optional<std::span<const std::byte>> {
                if (path == "/index.html" || path == "/") {
                    return std::as_bytes(std::span{boson::bench_app::kPageHtml.data(), boson::bench_app::kPageHtml.size()});
                }
                return std::nullopt;
            },
        });

    boson::compat::ElectronIpc electron_ipc;
    electron_ipc.handle("bench:ping", [](const boson::compat::IpcEvent&, boson::compat::ElectronIpc::Json args) {
        return args.is_array() && !args.get_array().empty() ? args.get_array().front() : boson::compat::ElectronIpc::Json{};
    });

    // The controller needs the bridge (for --auto) and the bridge needs the router: build the bridge from a
    // dispatch callback that forwards to the router declared right after it.
    boson::ipc::Router<boson::bench_app::BenchController, boson::compat::ElectronIpc>* router_ptr = nullptr;
    boson::ipc::Bridge bridge(runner, marshal, [&router_ptr, &runner](std::string_view raw_json, boson::ipc::Bridge::ReplyFn reply) {
        if (router_ptr != nullptr) {
            router_ptr->dispatch_async(raw_json, runner, std::move(reply));
        }
    });
    boson::bench_app::BenchController bench_controller(window, marshal, bridge, auto_mode);
    boson::ipc::Router<boson::bench_app::BenchController, boson::compat::ElectronIpc> router(bench_controller, electron_ipc);
    router_ptr = &router;
    electron_ipc.set_bridge(&bridge);
    bridge.attach(webview);

    const UINT start_load_message = ::RegisterWindowMessageW(L"BosonBench.StartLoad");
    window.add_message_hook([&bridge, start_load_message](UINT msg, WPARAM, LPARAM) -> std::optional<LRESULT> {
        if (start_load_message != 0 && msg == start_load_message) {
            bridge.emit_all("bench:start");
            return 0;
        }
        return std::nullopt;
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

    return boson::core::Window::run_message_loop();
}
