#pragma once

#include <windows.h>
#include <shobjidl.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::native {

/// @brief One entry of the file-type combo box. extensions are bare ("txt", "tar.gz") or "*" for all files;
/// a leading "*." or "." is accepted and ignored.
struct FileFilter {
    std::string name;
    std::vector<std::string> extensions;
};

struct FileDialogOptions {
    std::string title;
    bool allow_multiple = false;    ///< open only
    bool pick_folder = false;       ///< open only: folders instead of files
    std::string default_name;       ///< initial file name (save; open shows it in the edit box)
    /// Initial folder, or a full path (folder + file name) whose name part becomes default_name when that is empty.
    std::string default_path;
    std::vector<FileFilter> filters;
    std::uint32_t filter_index = 0; ///< 0-based index into filters selected initially
    std::string button_label;       ///< OK button text; empty = system default
    std::string default_extension;  ///< save: appended when the user types a name without extension
    bool show_hidden = false;       ///< FOS_FORCESHOWHIDDEN
    bool confirm_overwrite = true;  ///< save: FOS_OVERWRITEPROMPT
    bool add_to_recent = true;      ///< false = FOS_DONTADDTORECENT
    bool path_must_exist = true;    ///< FOS_PATHMUSTEXIST
    /// Automation / test hook, called once on the UI thread when the dialog window exists (first
    /// IFileDialogEvents::OnFolderChange). The hook may call dialog.SetFileName() and post WM_COMMAND IDOK /
    /// IDCANCEL to dialog_hwnd to drive the dialog without user input.
    std::function<void(IFileDialog& dialog, HWND dialog_hwnd)> on_shown;
};

struct FileDialogResult {
    bool cancelled = true;
    std::vector<std::string> paths;  ///< UTF-8 absolute file-system paths
    std::uint32_t filter_index = 0;  ///< 0-based filter selected when the dialog closed
    HRESULT hr = S_OK;               ///< failure other than cancellation (S_OK when cancelled)
};

enum class FileDialogKind : std::uint8_t {
    Open,
    Save,
    Folder,
};

/// @brief "*.txt;*.md" for a filter ("*.*" when it lists "*" or nothing).
[[nodiscard]] std::wstring filter_spec(const FileFilter& filter);

// The *_dialog functions run the modal loop on the calling thread (an STA UI thread with a message loop) and
// return when the user closes the dialog. Do not call them from inside a WebView2 event handler or a
// UiMarshal batch: the nested modal loop would stall the remaining work of that dispatch. Use
// show_file_dialog_async() instead, which starts the dialog from its own message dispatch.

[[nodiscard]] FileDialogResult open_file_dialog(HWND owner, const FileDialogOptions& options);
[[nodiscard]] FileDialogResult save_file_dialog(HWND owner, const FileDialogOptions& options);
[[nodiscard]] FileDialogResult pick_folder_dialog(HWND owner, const FileDialogOptions& options);
[[nodiscard]] FileDialogResult show_file_dialog(FileDialogKind kind, HWND owner, const FileDialogOptions& options);

/// @brief UI thread: shows the dialog from a fresh dispatch of the thread's message loop and calls done there
/// (on the same thread) with the result. Returns immediately.
void show_file_dialog_async(
    FileDialogKind kind,
    HWND owner,
    FileDialogOptions options,
    std::function<void(FileDialogResult)> done);

/// @brief UI thread: runs fn from its own dispatch of this thread's message loop (a posted message to a
/// lazily created message-only window), never nested inside the caller. Modal loops started by fn therefore
/// never hold back other queued work of the caller. No-op for an empty fn.
void defer_to_message_loop(std::function<void()> fn);

} // namespace boson::native
