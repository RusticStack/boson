#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <boson/ipc/controllers/auto_launch_controller.hpp>
#include <boson/ipc/controllers/clipboard_controller.hpp>
#include <boson/ipc/controllers/credential_controller.hpp>
#include <boson/ipc/controllers/drag_controller.hpp>
#include <boson/ipc/controllers/fs_controller.hpp>
#include <boson/ipc/controllers/gpu_controller.hpp>
#include <boson/ipc/controllers/log_controller.hpp>
#include <boson/ipc/controllers/menu_controller.hpp>
#include <boson/ipc/controllers/native_controller.hpp>
#include <boson/ipc/controllers/notification_controller.hpp>
#include <boson/ipc/controllers/power_controller.hpp>
#include <boson/ipc/controllers/process_controller.hpp>
#include <boson/ipc/controllers/safe_storage_controller.hpp>
#include <boson/ipc/controllers/screen_controller.hpp>
#include <boson/ipc/controllers/shell_controller.hpp>
#include <boson/ipc/controllers/shortcut_controller.hpp>
#include <boson/ipc/controllers/store_controller.hpp>
#include <boson/ipc/controllers/taskbar_controller.hpp>
#include <boson/ipc/controllers/terminal_controller.hpp>
#include <boson/ipc/controllers/titlebar_controller.hpp>
#include <boson/ipc/controllers/transfer_controller.hpp>
#include <boson/ipc/controllers/updater_controller.hpp>
#include <boson/ipc/controllers/webview_controller.hpp>
#include <boson/ipc/controllers/window_controller.hpp>
#include <boson/ipc/controllers/platform_events.hpp>
#include <boson/ipc/sync_bridge.hpp>
#include <boson/ipc/system_controller.hpp>
#include <boson/ipc/ts_generator.hpp>

#include "../../examples/demo/app_controller.hpp"
#include "../../examples/demo/demo_events.hpp"
#include "../../examples/demo/demo_settings.hpp"

namespace {

bool write_file(const std::filesystem::path& out_path, const std::string& content) {
    if (out_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(out_path.parent_path(), ec);
        if (ec) {
            std::cerr << "Failed to create directory: " << ec.message() << '\n';
            return false;
        }
    }

    std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        std::cerr << "Failed to open output file: " << out_path.string() << '\n';
        return false;
    }
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!ofs) {
        std::cerr << "Failed to write output file: " << out_path.string() << '\n';
        return false;
    }
    return true;
}

} // namespace

using DemoEventMaps = boson::ipc::TsGenerator::EventMaps<boson::demo::DemoEvents, boson::ipc::DesktopEvents, boson::ipc::MenuEvents,
                                                      boson::ipc::ShellIntegrationEvents, boson::ipc::PlatformEvents,
                                                      boson::ipc::StoreEvents<boson::demo::DemoSettings>,
                                                      boson::ipc::DragEvents,
                                                      boson::ipc::WindowManagerEvents,
                                                      boson::ipc::NativeEvents,
                                                      boson::ipc::GpuEvents,
                                                      boson::ipc::ProcessEvents,
                                                      boson::ipc::TerminalEvents>;

/// The controllers listed once; both outputs come from the same list.
template <boson::ipc::BosonController... Controllers>
struct Outputs {
    static std::string bindings() { return boson::ipc::TsGenerator::generate_with_events<DemoEventMaps, Controllers...>(); }
    static std::string method_params() { return boson::ipc::TsGenerator::generate_method_params<Controllers...>(); }
};

using AllControllers = Outputs<
    boson::ipc::SystemController,
    boson::demo::AppController,
    boson::ipc::AutoLaunchController,
    boson::ipc::ClipboardController,
    boson::ipc::CredentialController,
    boson::ipc::DragController,
    boson::ipc::DialogController,
    boson::ipc::FsController,
    boson::ipc::GpuController,
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
    boson::ipc::ShortcutController,
    boson::ipc::StoreController<boson::demo::DemoSettings>,
    boson::ipc::SyncHostController,
    boson::ipc::TaskbarController,
    boson::ipc::TerminalController,
    boson::ipc::TitlebarController,
    boson::ipc::TransferController,
    boson::ipc::UpdaterController,
    boson::ipc::WebViewController,
    boson::ipc::WindowController,
    boson::ipc::ZoomController>;

// Usage: boson_tsgen [bindings.d.ts [method-params.ts]]
//   bindings.d.ts     BosonApi / BosonEvents declarations (stdout when omitted)
//   method-params.ts  positional parameter names per method, used by frontend/src/compat/wails.ts
int main(int argc, char** argv) {
    const std::string dts = AllControllers::bindings();
    if (argc > 1) {
        if (!write_file(std::filesystem::path{argv[1]}, dts)) {
            return 1;
        }
        if (argc > 2 && !write_file(std::filesystem::path{argv[2]}, AllControllers::method_params())) {
            return 1;
        }
        return 0;
    }

    std::cout << dts;
    return 0;
}
