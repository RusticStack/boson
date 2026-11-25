#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace boson::native {

struct FileDialogOptions {
    std::string title;
    bool allow_multiple = false;
    bool pick_folder = false;
    std::string default_name;
};

struct FileDialogResult {
    bool cancelled = true;
    std::vector<std::string> paths;
};

[[nodiscard]] FileDialogResult open_file_dialog(HWND owner, const FileDialogOptions& options);
[[nodiscard]] FileDialogResult save_file_dialog(HWND owner, const FileDialogOptions& options);

} // namespace boson::native
