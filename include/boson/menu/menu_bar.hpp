#pragma once

#include <windows.h>

#include <boson/menu/accelerator.hpp>
#include <boson/menu/menu.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::core {
class Window;
class WebView;
} // namespace boson::core

namespace boson::menu {

enum class MenuBarMode : std::uint8_t {
    /// Native when the window has a system title bar, Popups for headless_titlebar / borderless windows.
    Auto,
    /// SetMenu(): the classic Win32 menu bar under the caption.
    Native,
    /// No HMENU is attached. The HTML title bar renders its own buttons and calls open_submenu() with the
    /// button's rect; the submenus drop down from there as native popups (VS Code custom title bar style).
    Popups,
};

struct MenuBarOptions {
    MenuBarMode mode = MenuBarMode::Auto;
    /// std::nullopt: Dark when the window was created with dark_mode, Light otherwise.
    std::optional<MenuTheme> theme;
    /// Bind the items' accelerators in the MenuBar's AcceleratorTable.
    bool bind_accelerators = true;
    /// Native mode: owner-draw the menu bar strip in dark colours when the theme is dark (UAH messages).
    bool dark_native_bar = true;
    /// First WM_COMMAND id used by the bar's items.
    UINT first_command_id = 0x7000;
};

/// @brief Top-level entry of the bar, for HTML title bars.
struct MenuBarEntry {
    std::size_t index = 0;
    std::string id;
    std::string label;       ///< raw label with '&' mnemonics
    std::string text;        ///< label with mnemonics removed ("&&" -> "&")
    std::string mnemonic;    ///< uppercase mnemonic character (UTF-8) or empty
    bool enabled = true;
    bool has_submenu = false;
};

/// @brief Application menu of one Window built from a MenuItem tree. Click callbacks run on the UI thread;
/// keyboard accelerators are bound automatically (also while a WebView has focus, via attach_webview).
class MenuBar {
public:
    explicit MenuBar(core::Window& window, MenuBarOptions options = {});
    ~MenuBar();

    MenuBar(const MenuBar&) = delete;
    MenuBar& operator=(const MenuBar&) = delete;
    MenuBar(MenuBar&&) = delete;
    MenuBar& operator=(MenuBar&&) = delete;

    void set_items(std::vector<MenuItem> items);
    void clear();
    [[nodiscard]] Menu& menu() noexcept { return m_menu; }
    [[nodiscard]] const Menu& menu() const noexcept { return m_menu; }
    [[nodiscard]] bool empty() const noexcept { return m_menu.items().empty(); }

    bool set_enabled(std::string_view id, bool enabled) { return m_menu.set_enabled(id, enabled); }
    bool set_checked(std::string_view id, bool checked) { return m_menu.set_checked(id, checked); }
    bool set_label(std::string_view id, std::string label) { return m_menu.set_label(id, std::move(label)); }
    bool set_accelerator(std::string_view id, std::string accel) { return m_menu.set_accelerator(id, std::move(accel)); }
    bool set_visible(std::string_view id, bool visible) { return m_menu.set_visible(id, visible); }
    /// @brief Rebuilds the HMENU from the (possibly edited) item tree; re-binds accelerators.
    void rebuild();

    [[nodiscard]] MenuBarMode mode() const noexcept { return m_mode; }
    [[nodiscard]] bool is_native() const noexcept { return m_mode == MenuBarMode::Native; }
    [[nodiscard]] MenuTheme theme() const noexcept { return m_theme; }
    void set_theme(MenuTheme theme);

    /// @brief Runs after every click (application menu, popup submenu or accelerator), after the item's
    /// own on_click / role. Use it to forward clicks to the frontend ("menu:click").
    void set_click_listener(std::function<void(const MenuItem& item, const MenuClick& click)> listener) {
        m_listener = std::move(listener);
    }
    /// @brief Targets for role items (the window is filled in automatically).
    void set_role_context(RoleContext context);
    [[nodiscard]] const RoleContext& role_context() const noexcept { return m_roles; }

    /// @brief Accelerators typed inside webview reach the bar, and webview becomes the default role target.
    bool attach_webview(core::WebView& webview);
    void detach_webview(core::WebView& webview) noexcept;
    /// @brief The table holding the bar's accelerators; add your own shortcuts to it as well.
    [[nodiscard]] AcceleratorTable& accelerators() noexcept { return m_accelerators; }

    [[nodiscard]] std::vector<MenuBarEntry> entries() const;
    /// @brief Index of the visible top-level entry with id (or, when no id matches, with that plain text).
    [[nodiscard]] std::optional<std::size_t> index_of(std::string_view id_or_text) const;

    /// @brief Popups/headless mode: client-pixel rects of the HTML title bar buttons, one per entry().
    /// Enables hover-switching between menus and Left/Right arrow navigation while a submenu is open, and
    /// Alt+mnemonic / F10 opening a submenu while a WebView has focus.
    void set_anchor_rects(std::vector<RECT> client_px);
    [[nodiscard]] const std::vector<RECT>& anchor_rects() const noexcept { return m_anchors; }

    /// @brief Drops the submenu of top-level entry index below anchor_client_px (client pixels of the window)
    /// and runs it modally (the message loop keeps running). A top-level entry without children is
    /// dispatched directly. Returns the chosen item, std::nullopt when dismissed.
    std::optional<MenuSelection> open_submenu(std::size_t index, std::optional<RECT> anchor_client_px = std::nullopt);
    /// @brief Native mode: opens the bar's menu from the keyboard (as if Alt / F10 was pressed).
    bool activate_keyboard(wchar_t mnemonic = 0);
    /// @brief Index of the submenu currently dropped by open_submenu(), if any.
    [[nodiscard]] std::optional<std::size_t> open_index() const noexcept { return m_open_index; }

    [[nodiscard]] HMENU handle() noexcept { return m_menu.current_handle(); }

private:
    static LRESULT CALLBACK msg_filter_proc(int code, WPARAM w_param, LPARAM l_param);
    std::optional<LRESULT> on_message(UINT msg, WPARAM w_param, LPARAM l_param);
    bool on_unmatched_key(const Accelerator& chord, bool system_key);
    bool handle_menu_loop_message(const MSG& msg);
    void attach_native();
    void detach_native() noexcept;
    void sync_accelerators();
    void on_menu_changed();
    [[nodiscard]] std::optional<std::size_t> anchor_at(POINT screen_pt) const noexcept;
    [[nodiscard]] std::optional<std::size_t> step_index(std::size_t from, int delta) const;
    [[nodiscard]] std::optional<std::size_t> index_for_mnemonic(wchar_t mnemonic) const;
    bool paint_uah_bar(LPARAM l_param) const;
    bool paint_uah_item(LPARAM l_param) const;
    void paint_bar_bottom_line() const;
    [[nodiscard]] bool dark_bar() const noexcept;

    core::Window& m_window;
    MenuBarOptions m_options;
    MenuBarMode m_mode = MenuBarMode::Native;
    MenuTheme m_theme = MenuTheme::Light;
    Menu m_menu;
    AcceleratorTable m_accelerators;
    std::vector<AcceleratorTable::Id> m_bound_accelerators;
    RoleContext m_roles;
    std::function<void(const MenuItem&, const MenuClick&)> m_listener;
    std::vector<RECT> m_anchors;
    std::uint64_t m_hook_id = 0;
    bool m_attached = false;
    bool m_syncing = false;

    // Popup-mode menu loop state.
    std::optional<std::size_t> m_open_index;
    std::optional<std::size_t> m_pending_switch;
    int m_popup_depth = 0;
    bool m_hot_item_has_submenu = false;
    HHOOK m_msg_filter = nullptr;
};

} // namespace boson::menu
