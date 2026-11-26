// boson_kitchen_sink: bench_app plus EVERY module of the framework linked, constructed and registered with the Router,
// to measure what an app that uses all of Boson costs while idle (docs/PERFORMANCE.md, "Memory footprint").
//
// (Original bench_app protocol, unchanged:)
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

#include "../bench_app/bench_page.hpp"
#include "../demo/demo_settings.hpp"
#include <boson/ipc/controllers/db_controller.hpp>
#include <boson/ipc/controllers/drag_controller.hpp>
#include <boson/ipc/controllers/http_controller.hpp>
#include <boson/ipc/controllers/log_controller.hpp>
#include <boson/ipc/controllers/process_controller.hpp>
#include <boson/ipc/controllers/screen_controller.hpp>
#include <boson/ipc/controllers/shell_controller.hpp>
#include <boson/ipc/controllers/shortcut_controller.hpp>
#include <boson/ipc/controllers/store_controller.hpp>
#include <boson/ipc/controllers/terminal_controller.hpp>
#include <boson/ipc/controllers/updater_controller.hpp>
#include <boson/ipc/controllers/window_controller.hpp>
#include <boson/db/database.hpp>
#include <boson/process/process.hpp>
#include <boson/shipping/crash_reporter.hpp>
#include <boson/shipping/logger.hpp>
#include <boson/shipping/updater.hpp>
#include <boson/store/store.hpp>
#include <boson/terminal/terminal_host.hpp>
#include <boson/transfer/binary_transport.hpp>
#include <boson/transfer/drag_out.hpp>
#include <boson/transfer/stream_server.hpp>
#include <boson/app/window_manager.hpp>
#include <boson/security/scoped_fs.hpp>
#include <boson/security/fs_scope.hpp>

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


namespace {

namespace fs = std::filesystem;

[[nodiscard]] fs::path sink_dir() {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / L"boson_kitchen_sink";
    fs::create_directories(dir, ec);
    return dir;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev_instance*/, PWSTR /*cmd_line*/, int show_cmd) {
    // Crash reporter first, like a shipping app (a writer thread + SetUnhandledExceptionFilter).
    boson::shipping::CrashReporter crash_reporter({.app_name = L"Boson Kitchen Sink", .app_version = "0.1.0"});
    crash_reporter.install();

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
    const fs::path dir = sink_dir();

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson Kitchen Sink";
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
    webview_cfg.scripts_on_document_created.push_back(boson::transfer::BinaryTransport::page_script());
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

    // ---- every service of the framework, constructed the way an app that uses it would ----------------------
    boson::shipping::LoggerConfig log_cfg;
    log_cfg.directory = dir / L"logs";
    log_cfg.mirror_to_debugger = false;
    const auto logger = std::make_shared<boson::shipping::Logger>(std::move(log_cfg));

    auto settings = std::make_shared<boson::store::Store<boson::demo::DemoSettings>>(boson::store::StoreOptions{
        .path = dir / L"settings.json",
    });

    boson::db::DatabaseOptions db_options;
    auto db_opened = boson::db::Database::open(std::move(db_options));
    std::shared_ptr<boson::db::Database> database;
    if (db_opened) {
        database = std::make_shared<boson::db::Database>(std::move(*db_opened));
    }

    auto fs_scope = std::make_shared<boson::security::FsScope>();
    auto scoped_fs = std::make_shared<boson::security::ScopedFs>(fs_scope);

    boson::titlebar::Titlebar titlebar(window);
    boson::desktop::Clipboard clipboard(window.hwnd());
    boson::desktop::GlobalShortcuts shortcuts;
    boson::desktop::DisplayWatcher display_watcher([](const boson::desktop::ScreenChange&) {}, &window);
    boson::native::PermissionManager permissions(dir / L"permissions.json");
    boson::native::TrayIcon tray(window.hwnd());
    boson::process::ProcessManager processes;
    boson::transfer::BinaryTransport transport(runner, marshal);
    boson::transfer::StreamServer streams(runner, marshal);
    boson::terminal::TerminalHost terminals(runner, transport);
    boson::transfer::DragOut drag_out(marshal, window.hwnd());
    boson::app::WindowManagerOptions wm_options;
    wm_options.environment = environment;
    wm_options.instance = instance;
    wm_options.app_name = L"Boson Kitchen Sink";
    wm_options.state_directory = dir;
    wm_options.runner = &runner;
    boson::app::WindowManager window_manager(std::move(wm_options));
    boson::webview::GuestManager guests(host);

    boson::shipping::UpdaterConfig updater_cfg;
    updater_cfg.manifest_url = "https://127.0.0.1:9/manifest.json";
    updater_cfg.current_version = "0.1.0";
    updater_cfg.app_id = L"Boson Kitchen Sink";
    updater_cfg.work_dir = dir / L"updates";
    updater_cfg.logger = logger;
    boson::shipping::Updater updater(std::move(updater_cfg));

    boson::ipc::MenuBackend menu_backend(window, webview);

    // ---- controllers ------------------------------------------------------------------------------------------
    boson::compat::ElectronIpc electron_ipc;
    electron_ipc.handle("bench:ping", [](const boson::compat::IpcEvent&, boson::compat::ElectronIpc::Json args) {
        return args.is_array() && !args.get_array().empty() ? args.get_array().front() : boson::compat::ElectronIpc::Json{};
    });

    boson::ipc::SystemController system_controller(window, webview, marshal);
    boson::ipc::TitlebarController titlebar_controller(window, webview, marshal, titlebar);
    boson::ipc::StoreController<boson::demo::DemoSettings> store_controller(settings);
    boson::ipc::AutoLaunchController auto_launch_controller(L"Boson Kitchen Sink");
    boson::ipc::ClipboardController clipboard_controller(window.hwnd());
    boson::ipc::CredentialController credential_controller(boson::security::CredentialStore("boson.kitchen_sink"));
    boson::ipc::DialogController dialog_controller(window, marshal);
    boson::ipc::FsController fs_controller(scoped_fs);
    boson::ipc::LogController log_controller(logger);
    boson::ipc::MenuController menu_controller(menu_backend, marshal);
    boson::ipc::NativeController native_controller(window, marshal, {environment});
    boson::ipc::NotificationController notification_controller(window, marshal, L"Boson.KitchenSink");
    boson::ipc::PermissionController permission_controller(permissions, marshal);
    boson::ipc::PowerController power_controller(window);
    boson::ipc::ProcessController process_controller(processes);
    boson::ipc::SafeStorageController safe_storage_controller;
    boson::ipc::ScreenController screen_controller(window);
    boson::ipc::ShellController shell_controller;
    boson::ipc::TaskbarController taskbar_controller(window, marshal);
    boson::ipc::TerminalController terminal_controller(terminals);
    boson::ipc::TransferController transfer_controller(transport, webview, &streams);
    boson::ipc::WebViewController webview_controller(webview, marshal);
    boson::ipc::WindowController window_controller(window_manager, marshal);
    boson::ipc::ZoomController zoom_controller(webview, marshal);
    boson::ipc::GuestController guest_controller(guests, marshal);
    boson::ipc::SyncHostController sync_host_controller;
    boson::ipc::DragController drag_controller(drag_out, nullptr);
    std::optional<boson::ipc::DbController> db_controller;
    if (database) {
        db_controller.emplace(database);
    }

    using Router = boson::ipc::Router<
        boson::bench_app::BenchController,
        boson::compat::ElectronIpc,
        boson::ipc::SystemController,
        boson::ipc::TitlebarController,
        boson::ipc::StoreController<boson::demo::DemoSettings>,
        boson::ipc::AutoLaunchController,
        boson::ipc::ClipboardController,
        boson::ipc::CredentialController,
        boson::ipc::DialogController,
        boson::ipc::FsController,
        boson::ipc::LogController,
        boson::ipc::MenuController,
        boson::ipc::NativeController,
        boson::ipc::NotificationController,
        boson::ipc::PermissionController,
        boson::ipc::PowerController,
        boson::ipc::ProcessController,
        boson::ipc::SafeStorageController,
        boson::ipc::ScreenController,
        boson::ipc::ShellController,
        boson::ipc::TaskbarController,
        boson::ipc::TerminalController,
        boson::ipc::TransferController,
        boson::ipc::WebViewController,
        boson::ipc::WindowController,
        boson::ipc::ZoomController,
        boson::ipc::GuestController,
        boson::ipc::SyncHostController,
        boson::ipc::DragController>;
    Router* router_ptr = nullptr;
    boson::ipc::Bridge bridge(runner, marshal, [&router_ptr, &runner](std::string_view raw_json, boson::ipc::Bridge::ReplyFn reply) {
        if (router_ptr != nullptr) {
            router_ptr->dispatch_async(raw_json, runner, std::move(reply));
        }
    });
    boson::bench_app::BenchController bench_controller(window, marshal, bridge, auto_mode);
    boson::ipc::ShortcutController shortcut_controller(shortcuts, marshal, bridge);
    boson::ipc::HttpController http_controller(bridge, runner, scoped_fs);
    boson::ipc::UpdaterController updater_controller(updater, bridge, runner);

    Router router(
        bench_controller, electron_ipc, system_controller, titlebar_controller, store_controller, auto_launch_controller,
        clipboard_controller, credential_controller, dialog_controller, fs_controller, log_controller, menu_controller,
        native_controller, notification_controller, permission_controller, power_controller, process_controller,
        safe_storage_controller, screen_controller, shell_controller, taskbar_controller, terminal_controller,
        transfer_controller, webview_controller, window_controller, zoom_controller, guest_controller,
        sync_host_controller, drag_controller);
    router_ptr = &router;
    electron_ipc.set_bridge(&bridge);
    bridge.attach(webview);
    window_manager.set_bridge(&bridge);

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
