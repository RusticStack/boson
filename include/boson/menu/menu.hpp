#pragma once

#include <windows.h>

#include <boson/menu/accelerator.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace boson::core {
class Window;
class WebView;
} // namespace boson::core

namespace boson::menu {

enum class MenuItemType : std::uint8_t {
    Normal,
    Separator,
    Checkbox,
    Radio,
    Submenu,
};

/// @brief Standard behaviour attached to an item. A role item with no on_click performs the role; an empty
/// label / accelerator is filled from the role's defaults (e.g. Copy -> "&Copy", "Ctrl+C").
enum class MenuRole : std::uint8_t {
    None,
    // Editing commands, sent to the focused element of RoleContext::webview (CDP editing commands).
    Undo,
    Redo,
    Cut,
    Copy,
    Paste,
    PasteAndMatchStyle,
    Delete,
    SelectAll,
    // WebView commands.
    Reload,
    ForceReload,
    ToggleDevTools,
    ResetZoom,
    ZoomIn,
    ZoomOut,
    // Window commands (RoleContext::window).
    ToggleFullscreen,
    Minimize,
    Maximize,
    Close,
    ShowWindow,
    HideWindow,
    Quit,
};

/// @brief Where a click came from.
enum class MenuSource : std::uint8_t {
    ApplicationMenu,
    ContextMenu,
    Tray,
    Accelerator,
};

struct MenuClick {
    std::string id;
    MenuSource source = MenuSource::ApplicationMenu;
    bool checked = false; ///< State after the click (checkbox toggled / radio selected).
};

using MenuClickCallback = std::function<void(const MenuClick& click)>;

/// @brief One node of a declarative menu tree.
struct MenuItem {
    std::string id;
    /// Win32 mnemonic syntax: "&File" underlines F; "&&" is a literal ampersand.
    std::string label;
    /// Accelerator::parse syntax ("Ctrl+Shift+P"). Shown right-aligned and, in a MenuBar, bound as a shortcut
    /// (editing roles are display-only: the WebView already handles Ctrl+C & co. natively).
    std::string accelerator;
    MenuItemType type = MenuItemType::Normal;
    MenuRole role = MenuRole::None;
    bool enabled = true;
    bool checked = false;
    bool visible = true;
    /// Radio items with the same non-empty group in the same submenu are exclusive; with an empty group the
    /// exclusive set is the run of adjacent radio items.
    std::string radio_group;
    std::vector<MenuItem> submenu;
    /// Runs on the UI thread. When empty, role (if any) is performed.
    MenuClickCallback on_click;

    [[nodiscard]] static MenuItem separator();
    [[nodiscard]] static MenuItem action(std::string id, std::string label, std::string accelerator = {}, MenuClickCallback on_click = {});
    [[nodiscard]] static MenuItem checkbox(std::string id, std::string label, bool checked, MenuClickCallback on_click = {});
    [[nodiscard]] static MenuItem radio(std::string id, std::string label, std::string group, bool checked, MenuClickCallback on_click = {});
    [[nodiscard]] static MenuItem sub(std::string label, std::vector<MenuItem> items, std::string id = {});
    [[nodiscard]] static MenuItem from_role(MenuRole role, std::string label = {});

    /// @brief Submenu when it has children, whatever type says.
    [[nodiscard]] MenuItemType effective_type() const noexcept;
    /// @brief label, or the role's default label.
    [[nodiscard]] std::string effective_label() const;
    /// @brief accelerator, or the role's default accelerator.
    [[nodiscard]] std::string effective_accelerator() const;
};

[[nodiscard]] std::string_view role_name(MenuRole role) noexcept;
/// @brief Case-insensitive; accepts "selectAll", "select_all", "selectall", "toggleDevTools", ...
[[nodiscard]] std::optional<MenuRole> parse_role(std::string_view name) noexcept;
[[nodiscard]] std::string_view role_default_label(MenuRole role) noexcept;
[[nodiscard]] std::string_view role_default_accelerator(MenuRole role) noexcept;
/// @brief Undo..SelectAll: performed inside the WebView, their accelerators are display-only.
[[nodiscard]] bool role_is_edit_command(MenuRole role) noexcept;
[[nodiscard]] std::string_view source_name(MenuSource source) noexcept;

/// @brief The uppercase mnemonic character of a label ("&File" -> 'F'), 0 when none.
[[nodiscard]] wchar_t mnemonic_of(std::wstring_view label) noexcept;

/// @brief Targets for role items.
struct RoleContext {
    core::Window* window = nullptr;
    core::WebView* webview = nullptr;
    /// Quit role; when empty Quit closes window (WM_CLOSE) or, without a window, posts WM_QUIT.
    std::function<void()> on_quit;
};

/// @brief Performs role on ctx (UI thread). false when the role needs a target ctx does not have.
bool perform_role(MenuRole role, const RoleContext& ctx);

struct MenuDeleter {
    void operator()(HMENU menu) const noexcept;
};
using UniqueMenu = std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter>;

enum class MenuStyle : std::uint8_t {
    Bar,   ///< CreateMenu: a window menu bar (top-level items are the bar entries).
    Popup, ///< CreatePopupMenu: context / tray menus.
};

/// @brief A MenuItem tree plus the HMENU built from it. Dynamic updates (set_enabled/set_checked/set_label)
/// patch the live HMENU in place; structural edits go through set_items() or rebuild(). UI thread only.
class Menu {
public:
    explicit Menu(std::vector<MenuItem> items = {}, UINT first_command_id = 1);
    ~Menu() = default;

    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;
    Menu(Menu&&) noexcept = default;
    Menu& operator=(Menu&&) noexcept = default;

    void set_items(std::vector<MenuItem> items);
    [[nodiscard]] const std::vector<MenuItem>& items() const noexcept { return m_items; }
    /// @brief Mutable access for structural edits; call rebuild() afterwards.
    [[nodiscard]] std::vector<MenuItem>& mutable_items() noexcept { return m_items; }
    /// @brief Depth-first search by id (nullptr when absent or id is empty).
    [[nodiscard]] MenuItem* find(std::string_view id) noexcept;
    [[nodiscard]] const MenuItem* find(std::string_view id) const noexcept;

    bool set_enabled(std::string_view id, bool enabled);
    /// @brief Checking a radio item unchecks the rest of its group.
    bool set_checked(std::string_view id, bool checked);
    bool set_label(std::string_view id, std::string label);
    bool set_accelerator(std::string_view id, std::string accelerator);
    /// @brief Rebuilds the HMENU (visibility changes need it).
    bool set_visible(std::string_view id, bool visible);

    /// @brief Destroys the HMENU; the next handle() call builds a fresh one with new command ids.
    void rebuild();
    /// @brief The HMENU for style, built on demand. Owned by this Menu.
    [[nodiscard]] HMENU handle(MenuStyle style = MenuStyle::Popup);
    [[nodiscard]] HMENU current_handle() const noexcept { return m_handle.get(); }
    [[nodiscard]] MenuStyle current_style() const noexcept { return m_style; }
    /// @brief Bumped on every rebuild (handles and command ids change).
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

    [[nodiscard]] bool owns_command(UINT command) const noexcept;
    [[nodiscard]] std::optional<UINT> command_for(std::string_view id) const noexcept;
    [[nodiscard]] const MenuItem* item_for_command(UINT command) const noexcept;
    /// @brief false when any ancestor submenu or the item itself is disabled/hidden.
    [[nodiscard]] bool is_command_enabled(UINT command) const noexcept;

    /// @brief Applies checkbox/radio state, then runs on_click (or the role) and the menu listener.
    /// false when command is unknown or disabled.
    bool dispatch(UINT command, MenuSource source, const RoleContext& roles = {});

    /// @brief Called after every item click handled by dispatch() (after the item's own callback).
    void set_listener(std::function<void(const MenuItem& item, const MenuClick& click)> listener) {
        m_listener = std::move(listener);
    }
    /// @brief Called after the HMENU changed (label/state patch or rebuild) so a menu bar can be redrawn.
    void set_changed_callback(std::function<void()> callback) { m_on_changed = std::move(callback); }

    /// @brief Visits every enabled+visible command item with a parseable effective accelerator.
    void for_each_accelerator(const std::function<void(const MenuItem& item, UINT command, const Accelerator& accel)>& fn);

private:
    struct Entry {
        MenuItem* item = nullptr;
        std::vector<MenuItem>* siblings = nullptr; ///< nullptr = top level (m_items)
        std::size_t index = 0;          ///< index of item in siblings
        HMENU parent = nullptr;         ///< HMENU containing this item
        UINT position = 0;              ///< position inside parent
        UINT command = 0;               ///< 0 for submenus and separators
        std::ptrdiff_t parent_entry = -1;
    };

    void ensure_built(MenuStyle style);
    void build_level(HMENU target, std::vector<MenuItem>& items, std::ptrdiff_t parent_entry);
    [[nodiscard]] Entry* entry_for(const MenuItem* item) noexcept;
    [[nodiscard]] const Entry* entry_for_command(UINT command) const noexcept;
    void apply_state(const Entry& entry) const;
    void apply_text(const Entry& entry) const;
    void uncheck_radio_siblings(std::vector<MenuItem>* siblings, std::size_t index);
    [[nodiscard]] std::vector<MenuItem>& siblings_of(const Entry& entry) noexcept;
    void notify_changed() const;

    std::vector<MenuItem> m_items;
    std::vector<Entry> m_entries;
    UniqueMenu m_handle;
    MenuStyle m_style = MenuStyle::Popup;
    UINT m_first_command = 1;
    UINT m_next_command = 1;
    std::uint64_t m_generation = 0;
    std::function<void(const MenuItem&, const MenuClick&)> m_listener;
    std::function<void()> m_on_changed;
};

/// @brief Menu theme for popup menus (process-wide uxtheme preferred app mode + per-window opt-in).
enum class MenuTheme : std::uint8_t {
    System, ///< follow the Windows app theme
    Light,
    Dark,
};

/// @brief Applies theme to popup menus owned by owner via the undocumented uxtheme SetPreferredAppMode /
/// AllowDarkModeForWindow / FlushMenuThemes (Windows 10 1809+). Returns false (and changes nothing) when the
/// running Windows build does not export them.
bool set_menu_theme(HWND owner, MenuTheme theme) noexcept;
[[nodiscard]] bool dark_menus_supported() noexcept;

struct PopupOptions {
    HWND owner = nullptr;
    /// Screen pixels; std::nullopt = current cursor position.
    std::optional<POINT> screen_point;
    /// Screen rect the menu must not cover (e.g. the button the menu drops from). Implies TPM_VERTICAL.
    std::optional<RECT> exclude_rect;
    /// Extra TPM_* alignment flags; 0 = left/top aligned (right aligned on RTL systems).
    UINT align_flags = 0;
    /// Applied with set_menu_theme(owner, ...) before showing. std::nullopt leaves the theme alone.
    std::optional<MenuTheme> theme;
    MenuSource source = MenuSource::ContextMenu;
    /// Run the chosen item's on_click / role. When false the caller dispatches.
    bool dispatch = true;
    RoleContext roles;
};

struct MenuSelection {
    UINT command = 0;
    std::string id;
    bool checked = false;
};

/// @brief Shows menu as a popup with TrackPopupMenuEx(TPM_RETURNCMD) and returns the chosen item.
/// The call runs the modal menu loop, which keeps pumping the thread's messages (the UI never freezes).
/// Returns std::nullopt when dismissed, or when another popup menu is already open on this thread.
std::optional<MenuSelection> popup_menu(Menu& menu, const PopupOptions& options);
/// @brief Same, for an existing popup HMENU that belongs to menu (e.g. a menu bar submenu).
std::optional<MenuSelection> popup_menu(Menu& menu, HMENU popup, const PopupOptions& options);

/// @brief true while popup_menu() runs on this thread.
[[nodiscard]] bool is_popup_menu_open() noexcept;

} // namespace boson::menu
