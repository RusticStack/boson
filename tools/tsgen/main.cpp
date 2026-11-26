#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <boson/ipc/system_controller.hpp>
#include <boson/ipc/ts_generator.hpp>

#include "../../examples/demo/app_controller.hpp"
#include "../../examples/demo/demo_events.hpp"

int main(int argc, char** argv) {
    const std::string dts = boson::ipc::TsGenerator::generate_with_events<
        boson::demo::DemoEvents,
        boson::ipc::SystemController,
        boson::demo::AppController>();

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
