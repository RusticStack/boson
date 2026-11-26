#pragma once

#include <windows.h>
#include <shellapi.h>

#include <boson/menu/menu.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace boson::native {

struct NotificationOptions;

struct IconDeleter {
    void operator()(HICON icon) const noexcept;
};
using UniqueIcon = std::unique_ptr<std::remove_pointer_t<HICON>, IconDeleter>;

/// @brief Decodes PNG / ICO / BMP / JPEG / GIF bytes (anything WIC decodes) into a size_px x size_px icon with
/// alpha. For multi-image ICO files the frame closest to size_px (preferring larger ones) is used.
/// Initializes COM on the calling thread when needed. Returns nullptr on failure.
[[nodiscard]] UniqueIcon load_icon_from_image(std::span<const std::byte> bytes, int size_px);

/// @brief Small-icon size (SM_CXSMICON) for dpi; dpi 0 = the system DPI.
[[nodiscard]] int small_icon_size(UINT dpi = 0) noexcept;

enum class TrayEventKind : std::uint8_t {
    LeftClick,
    RightClick,
    DoubleClick,
    MiddleClick,
    KeyboardSelect,
    BalloonClicked,
    BalloonDismissed,
};

struct TrayEvent {
    TrayEventKind kind = TrayEventKind::LeftClick;
    POINT position{}; ///< screen pixels (icon anchor for keyboard events)
};

[[nodiscard]] std::string_view tray_event_name(TrayEventKind kind) noexcept;

/// @brief A notification-area (system tray) icon with tooltip, click events, a context menu built from the
/// shared MenuItem tree and balloon notifications. It owns a hidden top-level window, so it works without
/// any Boson Window and receives the TaskbarCreated broadcast: after Explorer restarts the icon is added
/// again automatically. Create, use and destroy it on a UI thread (one with a message loop).
class TrayIcon {
public:
    /// The callback message the hidden window receives from the shell.
    static constexpr UINT kWmTrayCallback = WM_APP + 2;

    using ShellNotifyFn = std::function<BOOL(DWORD message, NOTIFYICONDATAW* data)>;
    using EventCallback = std::function<void(const TrayEvent& event)>;
    using MenuListener = std::function<void(const menu::MenuItem& item, const menu::MenuClick& click)>;

    /// @param owner app window used as the default target of window roles (Show/Hide/Quit...); may be null.
    explicit TrayIcon(HWND owner = nullptr);
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;
    TrayIcon(TrayIcon&&) = delete;
    TrayIcon& operator=(TrayIcon&&) = delete;

    /// @brief Adds the icon to the notification area (no-op when already shown).
    bool show();
    /// @brief Compatibility with the former SystemTray: sets owner + tooltip, then show().
    bool show(HWND owner, std::string_view tooltip);
    void hide() noexcept;
    [[nodiscard]] bool is_visible() const noexcept { return m_visible; }

    /// @brief Uses a copy of icon (the caller keeps ownership of its handle).
    bool set_icon(HICON icon);
    /// @brief PNG / ICO bytes (e.g. an #embed array). Re-decoded at the right size after DPI changes.
    bool set_icon_from_bytes(std::span<const std::byte> bytes);
    [[nodiscard]] HICON icon() const noexcept;

    /// @brief At most 127 UTF-16 code units are shown; longer text is truncated.
    bool set_tooltip(std::string_view tooltip);
    bool update_tooltip(std::string_view tooltip) { return set_tooltip(tooltip); }
    [[nodiscard]] const std::string& tooltip() const noexcept { return m_tooltip; }

    /// @brief Context menu shown on right click (and on left click when set_menu_on_left_click(true)).
    void set_menu(std::vector<menu::MenuItem> items);
    [[nodiscard]] menu::Menu& menu() noexcept { return m_menu; }
    void set_menu_on_left_click(bool enabled) noexcept { m_menu_on_left_click = enabled; }
    void set_menu_theme(menu::MenuTheme theme) noexcept { m_theme = theme; }
    void set_role_context(menu::RoleContext context);
    /// @brief Runs after each tray menu click (after the item's own on_click / role).
    void set_menu_listener(MenuListener listener) { m_menu_listener = std::move(listener); }
    void set_event_callback(EventCallback callback) { m_on_event = std::move(callback); }
    /// @brief Shows the tray menu at screen_point (default: cursor). Returns the chosen item.
    std::optional<menu::MenuSelection> popup_menu(std::optional<POINT> screen_point = std::nullopt);

    /// @brief Balloon / toast from this icon (shows the icon first if needed).
    bool show_balloon(const NotificationOptions& options);

    [[nodiscard]] HWND message_window() const noexcept { return m_hwnd; }
    [[nodiscard]] HWND owner() const noexcept { return m_owner; }
    void set_owner(HWND owner) noexcept;
    /// @brief Screen rect of the icon (Shell_NotifyIconGetRect), when the shell knows it.
    [[nodiscard]] std::optional<RECT> icon_rect() const noexcept;
    /// @brief Successful NIM_ADD calls so far (1 + one per Explorer restart while visible).
    [[nodiscard]] std::size_t registration_count() const noexcept { return m_registrations; }

    /// @brief RegisterWindowMessage("TaskbarCreated").
    [[nodiscard]] static UINT taskbar_created_message() noexcept;

    /// @brief Test seam: replaces Shell_NotifyIconW. Pass {} to restore it.
    void set_shell_function(ShellNotifyFn fn) { m_shell = std::move(fn); }
    /// @brief Real tray menus need SetForegroundWindow (KB135788) or they will not close on outside clicks.
    /// Only tests turn this off.
    void set_foreground_on_popup(bool enabled) noexcept { m_foreground_on_popup = enabled; }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
    LRESULT handle_message(UINT msg, WPARAM w_param, LPARAM l_param);
    void handle_callback(WPARAM w_param, LPARAM l_param);
    void emit(TrayEventKind kind, POINT position);
    bool notify(DWORD message, NOTIFYICONDATAW& data) const;
    [[nodiscard]] NOTIFYICONDATAW base_data() const noexcept;
    bool add();
    bool modify(UINT flags);
    void reload_icon_for_dpi();
    [[nodiscard]] HICON current_icon() const noexcept;

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    UINT m_id = 1;
    bool m_visible = false;
    std::size_t m_registrations = 0;
    UniqueIcon m_icon;
    UniqueIcon m_default_icon;
    std::vector<std::byte> m_icon_bytes;
    std::string m_tooltip;
    menu::Menu m_menu;
    menu::MenuTheme m_theme = menu::MenuTheme::System;
    menu::RoleContext m_roles;
    bool m_menu_on_left_click = false;
    bool m_foreground_on_popup = true;
    EventCallback m_on_event;
    MenuListener m_menu_listener;
    ShellNotifyFn m_shell;
};

} // namespace boson::native
