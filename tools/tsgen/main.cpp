#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <boson/ipc/controllers/auto_launch_controller.hpp>
#include <boson/ipc/controllers/clipboard_controller.hpp>
#include <boson/ipc/controllers/credential_controller.hpp>
#include <boson/ipc/controllers/fs_controller.hpp>
#include <boson/ipc/controllers/log_controller.hpp>
#include <boson/ipc/controllers/menu_controller.hpp>
#include <boson/ipc/controllers/notification_controller.hpp>
#include <boson/ipc/controllers/power_controller.hpp>
#include <boson/ipc/controllers/safe_storage_controller.hpp>
#include <boson/ipc/controllers/screen_controller.hpp>
#include <boson/ipc/controllers/shell_controller.hpp>
#include <boson/ipc/controllers/shortcut_controller.hpp>
#include <boson/ipc/controllers/taskbar_controller.hpp>
#include <boson/ipc/controllers/titlebar_controller.hpp>
#include <boson/ipc/controllers/transfer_controller.hpp>
#include <boson/ipc/controllers/updater_controller.hpp>
#include <boson/ipc/controllers/webview_controller.hpp>
#include <boson/ipc/controllers/platform_events.hpp>
#include <boson/ipc/system_controller.hpp>
#include <boson/ipc/ts_generator.hpp>

#include "../../examples/demo/app_controller.hpp"
#include "../../examples/demo/demo_events.hpp"

int main(int argc, char** argv) {
    using namespace boson::ipc;
    const std::string dts = TsGenerator::generate_with_events<
        TsGenerator::EventMaps<boson::demo::DemoEvents, DesktopEvents, MenuEvents, ShellIntegrationEvents, PlatformEvents>,
        SystemController,
        boson::demo::AppController,
        AutoLaunchController,
        ClipboardController,
        CredentialController,
        FsController,
        LogController,
        MenuController,
        NotificationController,
        PowerController,
        SafeStorageController,
        ScreenController,
        ShellController,
        ShortcutController,
        TaskbarController,
        TitlebarController,
        TransferController,
        UpdaterController,
        WebViewController>();

    if (argc > 1) {
        const std::filesystem::path out_path{argv[1]};
        if (out_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(out_path.parent_path(), ec);
            if (ec) {
                std::cerr << "Failed to create directory: " << ec.message() << '\n';
                return 1;
            }
        }

        std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            std::cerr << "Failed to open output file: " << out_path.string() << '\n';
            return 1;
        }
        ofs.write(dts.data(), static_cast<std::streamsize>(dts.size()));
        if (!ofs) {
            std::cerr << "Failed to write output file: " << out_path.string() << '\n';
            return 1;
        }
        return 0;
    }

    std::cout << dts;
    return 0;
}
