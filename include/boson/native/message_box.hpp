#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::native {

enum class MessageBoxIcon : std::uint8_t {
    None,
    Info,
    Warning,
    Error,
    Question,
    Shield,
};

[[nodiscard]] std::optional<MessageBoxIcon> parse_message_box_icon(std::string_view name) noexcept;

/// Task dialog control id of buttons[i] is kMessageBoxButtonBaseId + i (TDM_CLICK_BUTTON in tests).
inline constexpr int kMessageBoxButtonBaseId = 1000;

struct MessageBoxOptions {
    std::string title;           ///< window caption; empty = executable name
    std::string message;         ///< main instruction (large text)
    std::string detail;          ///< content text under the main instruction
    MessageBoxIcon icon = MessageBoxIcon::None;
    std::vector<std::string> buttons; ///< left to right; empty = {"OK"}
    int default_id = 0;          ///< index of the default (Enter) button
    /// Index reported when the dialog is dismissed with Esc / Alt+F4 / the close box. -1 = automatic: the
    /// first button labelled "Cancel" or "No" (case-insensitive, '&' mnemonics ignored), else 0 (Electron's
    /// rule).
    int cancel_id = -1;
    std::string checkbox_label;  ///< "Don't ask again" style verification checkbox; empty = none
    bool checkbox_checked = false;
    std::string footer;
    bool command_links = false;  ///< buttons rendered as large command links (first line = label)
    /// Automation / test hook: called on the UI thread with the task dialog window once it exists
    /// (TDN_CREATED). Post TDM_CLICK_BUTTON / TDM_CLICK_VERIFICATION to it to drive the dialog.
    std::function<void(HWND dialog)> on_shown;
};

struct MessageBoxResult {
    int response = 0;            ///< index into buttons
    bool checkbox_checked = false;
    bool cancelled = false;      ///< dismissed via Esc / close (response = cancel id)
    bool used_fallback = false;  ///< TaskDialogIndirect unavailable: MessageBoxW was used
    HRESULT hr = S_OK;
};

/// @brief Resolved cancel index (see MessageBoxOptions::cancel_id).
[[nodiscard]] int resolve_cancel_id(const MessageBoxOptions& options) noexcept;

/// @brief true when TaskDialogIndirect (comctl32 v6) can be used. comctl32 v6 is activated at run time
/// through an activation context, so executables need no common-controls manifest dependency.
[[nodiscard]] bool task_dialog_available() noexcept;

/// @brief Modal TaskDialogIndirect owned by owner (may be null); falls back to MessageBoxW (no checkbox, at
/// most the standard OK / OK-Cancel / Yes-No / Yes-No-Cancel shapes) when comctl32 v6 is unavailable.
/// Runs a modal loop on the calling UI thread; prefer show_message_box_async() from event handlers.
[[nodiscard]] MessageBoxResult show_message_box(HWND owner, const MessageBoxOptions& options);

/// @brief UI thread: shows the dialog from a fresh dispatch of the message loop (defer_to_message_loop) and
/// calls done on the UI thread. Returns immediately.
void show_message_box_async(HWND owner, MessageBoxOptions options, std::function<void(MessageBoxResult)> done);

/// @brief Electron's dialog.showErrorBox: a modal error dialog with a single OK button.
void show_error_box(HWND owner, std::string_view title, std::string_view content);

} // namespace boson::native
