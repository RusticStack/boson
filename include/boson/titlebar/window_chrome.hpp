#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace boson::titlebar {

enum class CaptionButton : std::uint8_t {
    Minimize = 0,
    Maximize = 1,
    Close = 2,
};

inline constexpr std::size_t kCaptionButtonCount = 3;

// ---- System window menu ----------------------------------------------------------------------------------

/// @brief Enables/disables Restore, Move, Size, Minimize, Maximize and Close in menu for the current state
/// and style of hwnd (what DefWindowProc does before it shows the menu itself), and makes Close the default.
void update_system_menu_state(HWND hwnd, HMENU menu) noexcept;

/// @brief Receives the prepared menu and TrackPopupMenuEx flags; returns the chosen SC_* command or 0.
using MenuTracker = std::function<UINT(HWND hwnd, HMENU menu, UINT flags, POINT screen_pt)>;

/// @brief Shows hwnd's system menu at screen_pt (TrackPopupMenuEx, RTL-aware) and posts the chosen command
/// as WM_SYSCOMMAND. tracker replaces TrackPopupMenuEx (tests). Returns the command (0 = dismissed).
UINT show_system_menu(HWND hwnd, POINT screen_pt, const MenuTracker& tracker = {});

/// @brief Localized caption-button name, taken from the OS system menu strings ("Mi&nimize" -> "Minimize",
/// "&Close\tAlt+F4" -> "Close"), so it follows the Windows display language. Maximize becomes the
/// "Restore" string while maximized. Falls back to English.
[[nodiscard]] std::wstring localized_caption_name(HWND hwnd, CaptionButton button, bool maximized);

/// @brief Strips accelerator markers and the shortcut column from a menu string.
[[nodiscard]] std::wstring strip_menu_text(std::wstring_view text);

// ---- Keyboard -------------------------------------------------------------------------------------------

enum class SystemKeyAction {
    None,
    SystemMenu, ///< Alt+Space
    Close,      ///< Alt+F4
};

/// @brief Classifies a key reported by WebView2 AcceleratorKeyPressed (kind is COREWEBVIEW2_KEY_EVENT_KIND).
/// Only system-key-down events without auto-repeat map to an action.
[[nodiscard]] SystemKeyAction classify_system_key(int key_event_kind, UINT virtual_key, bool was_key_down) noexcept;

// ---- Frame metrics --------------------------------------------------------------------------------------

enum AutoHideEdge : unsigned {
    kAutoHideNone = 0,
    kAutoHideLeft = 1u << 0,
    kAutoHideTop = 1u << 1,
    kAutoHideRight = 1u << 2,
    kAutoHideBottom = 1u << 3,
};

/// Pixels left uncovered on an edge that hosts an auto-hide taskbar, so the taskbar can still be revealed.
inline constexpr int kAutoHideTaskbarReveal = 2;

/// @brief Client rect of a maximized headless window: the proposed window rect minus the resize frame,
/// clamped to the monitor work area, minus kAutoHideTaskbarReveal on auto-hide taskbar edges.
[[nodiscard]] RECT compute_maximized_client_rect(
    RECT proposed, RECT work_area, RECT monitor, int frame_x, int frame_y, unsigned auto_hide_edges) noexcept;

/// @brief Client rect of a restored headless window. keep_side_frames keeps the invisible left/right/bottom
/// resize borders non-client (native resizing outside the content, like a standard Windows 11 frame); the
/// top edge is always client so the app draws its own titlebar there.
[[nodiscard]] RECT compute_restored_client_rect(RECT proposed, int frame_x, int frame_y, bool keep_side_frames) noexcept;

/// @brief Auto-hide taskbar edges on monitor (SHAppBarMessage ABM_GETAUTOHIDEBAREX).
[[nodiscard]] unsigned query_auto_hide_edges(HMONITOR monitor) noexcept;

/// @brief Resize frame thickness (SM_CXSIZEFRAME/SM_CYSIZEFRAME + SM_CXPADDEDBORDER) at dpi.
[[nodiscard]] SIZE resize_frame_size(UINT dpi) noexcept;

/// @brief WM_NCCALCSIZE helper for headless windows (used by core::Window).
void adjust_headless_client_rect(HWND hwnd, RECT& rect, bool maximized, bool keep_side_frames, UINT dpi) noexcept;

/// @brief Height in px of the top resize band of a restored headless window (matches Window's hit test).
[[nodiscard]] int top_resize_band(UINT dpi) noexcept;

/// @brief true while hwnd has neither WS_CAPTION nor WS_THICKFRAME (core::Window strips both for fullscreen
/// before it resizes, so this is already true during the WM_SIZE of the transition).
[[nodiscard]] inline bool has_fullscreen_style(HWND hwnd) noexcept {
    return hwnd != nullptr && (GetWindowLongPtrW(hwnd, GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME)) == 0;
}

/// @brief true for HTLEFT..HTBOTTOMRIGHT.
[[nodiscard]] constexpr bool is_sizing_hit(LRESULT code) noexcept {
    return code >= HTLEFT && code <= HTBOTTOMRIGHT;
}

} // namespace boson::titlebar
