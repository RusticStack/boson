#include <boson/menu/menu.hpp>

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>

namespace boson::menu {

namespace {

struct RoleInfo {
    MenuRole role;
    std::string_view name;
    std::string_view label;
    std::string_view accelerator;
    std::string_view cdp_command; ///< Chromium editing command for edit roles
};

constexpr std::array kRoles{
    RoleInfo{MenuRole::Undo, "undo", "&Undo", "Ctrl+Z", "undo"},
    RoleInfo{MenuRole::Redo, "redo", "&Redo", "Ctrl+Y", "redo"},
    RoleInfo{MenuRole::Cut, "cut", "Cu&t", "Ctrl+X", "cut"},
    RoleInfo{MenuRole::Copy, "copy", "&Copy", "Ctrl+C", "copy"},
    RoleInfo{MenuRole::Paste, "paste", "&Paste", "Ctrl+V", "paste"},
    RoleInfo{MenuRole::PasteAndMatchStyle, "pasteAndMatchStyle", "Paste and &Match Style", "Ctrl+Shift+V", "pasteAndMatchStyle"},
    RoleInfo{MenuRole::Delete, "delete", "&Delete", "", "delete"},
    RoleInfo{MenuRole::SelectAll, "selectAll", "Select &All", "Ctrl+A", "selectAll"},
    RoleInfo{MenuRole::Reload, "reload", "&Reload", "Ctrl+R", ""},
    RoleInfo{MenuRole::ForceReload, "forceReload", "&Force Reload", "Ctrl+Shift+R", ""},
    RoleInfo{MenuRole::ToggleDevTools, "toggleDevTools", "Toggle &Developer Tools", "Ctrl+Shift+I", ""},
    RoleInfo{MenuRole::ResetZoom, "resetZoom", "&Actual Size", "Ctrl+0", ""},
    RoleInfo{MenuRole::ZoomIn, "zoomIn", "Zoom &In", "Ctrl+Plus", ""},
    RoleInfo{MenuRole::ZoomOut, "zoomOut", "Zoom &Out", "Ctrl+Minus", ""},
    RoleInfo{MenuRole::ToggleFullscreen, "toggleFullscreen", "Toggle &Full Screen", "F11", ""},
    RoleInfo{MenuRole::Minimize, "minimize", "Mi&nimize", "", ""},
    RoleInfo{MenuRole::Maximize, "maximize", "Ma&ximize", "", ""},
    RoleInfo{MenuRole::Close, "close", "&Close Window", "Ctrl+W", ""},
    RoleInfo{MenuRole::ShowWindow, "showWindow", "&Show", "", ""},
    RoleInfo{MenuRole::HideWindow, "hideWindow", "&Hide", "", ""},
    RoleInfo{MenuRole::Quit, "quit", "E&xit", "", ""},
};

[[nodiscard]] const RoleInfo* role_info(MenuRole role) noexcept {
    for (const RoleInfo& info : kRoles) {
        if (info.role == role) {
            return &info;
        }
    }
    return nullptr;
}

/// Lower-case with '_' and '-' removed, so "select_all", "selectAll" and "SELECT-ALL" compare equal.
[[nodiscard]] std::string fold(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        if (ch != '_' && ch != '-' && ch != ' ') {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return out;
}

[[nodiscard]] std::wstring item_text(const MenuItem& item) {
    std::wstring text = core::utf8_to_wide(item.effective_label());
    const std::string accel_text = item.effective_accelerator();
    if (!accel_text.empty()) {
        const auto parsed = Accelerator::parse(accel_text);
        text += L'\t';
        text += core::utf8_to_wide(parsed.has_value() ? parsed->to_string() : accel_text);
    }
    return text;
}

[[nodiscard]] UINT item_state(const MenuItem& item) noexcept {
    UINT state = item.enabled ? MFS_ENABLED : MFS_DISABLED;
    const MenuItemType type = item.effective_type();
    if ((type == MenuItemType::Checkbox || type == MenuItemType::Radio) && item.checked) {
        state |= MFS_CHECKED;
    }
    return state;
}

bool send_edit_command(core::WebView& webview, std::string_view command) {
    ICoreWebView2* core = webview.core_webview();
    if (core == nullptr) {
        return false;
    }
    const std::wstring down = L"{\"type\":\"rawKeyDown\",\"commands\":[\"" + core::utf8_to_wide(command) + L"\"]}";
    const auto ignore = []() {
        return Microsoft::WRL::Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [](HRESULT hr, LPCWSTR) -> HRESULT {
                core::hr_check(hr, "Input.dispatchKeyEvent (menu edit role)");
                return S_OK;
            });
    };
    const bool ok = BOSON_HR_CHECK(core->CallDevToolsProtocolMethod(L"Input.dispatchKeyEvent", down.c_str(), ignore().Get()));
    BOSON_HR_CHECK(core->CallDevToolsProtocolMethod(L"Input.dispatchKeyEvent", L"{\"type\":\"keyUp\"}", ignore().Get()));
    return ok;
}

bool adjust_zoom(core::WebView& webview, double delta) {
    const double current = webview.zoom_factor().value_or(1.0);
    return webview.set_zoom_factor(std::clamp(current + delta, 0.25, 5.0));
}

thread_local int t_popup_depth = 0;

} // namespace

// ---- MenuItem -------------------------------------------------------------------------------------------

MenuItem MenuItem::separator() {
    MenuItem item;
    item.type = MenuItemType::Separator;
    return item;
}

MenuItem MenuItem::action(std::string id, std::string label, std::string accelerator, MenuClickCallback on_click) {
    MenuItem item;
    item.id = std::move(id);
    item.label = std::move(label);
    item.accelerator = std::move(accelerator);
    item.on_click = std::move(on_click);
    return item;
}

MenuItem MenuItem::checkbox(std::string id, std::string label, bool checked, MenuClickCallback on_click) {
    MenuItem item = action(std::move(id), std::move(label), {}, std::move(on_click));
    item.type = MenuItemType::Checkbox;
    item.checked = checked;
    return item;
}

MenuItem MenuItem::radio(std::string id, std::string label, std::string group, bool checked, MenuClickCallback on_click) {
    MenuItem item = action(std::move(id), std::move(label), {}, std::move(on_click));
    item.type = MenuItemType::Radio;
    item.radio_group = std::move(group);
    item.checked = checked;
    return item;
}

MenuItem MenuItem::sub(std::string label, std::vector<MenuItem> items, std::string id) {
    MenuItem item;
    item.id = std::move(id);
    item.label = std::move(label);
    item.type = MenuItemType::Submenu;
    item.submenu = std::move(items);
    return item;
}

MenuItem MenuItem::from_role(MenuRole role, std::string label) {
    MenuItem item;
    item.id = std::string(role_name(role));
    item.role = role;
    item.label = std::move(label);
    return item;
}

MenuItemType MenuItem::effective_type() const noexcept {
    if (!submenu.empty()) {
        return MenuItemType::Submenu;
    }
    return type;
}

std::string MenuItem::effective_label() const {
    if (!label.empty() || role == MenuRole::None) {
        return label;
    }
    return std::string(role_default_label(role));
}

std::string MenuItem::effective_accelerator() const {
    if (!accelerator.empty() || role == MenuRole::None) {
        return accelerator;
    }
    return std::string(role_default_accelerator(role));
}

// ---- Roles ----------------------------------------------------------------------------------------------

std::string_view role_name(MenuRole role) noexcept {
    const RoleInfo* info = role_info(role);
    return info != nullptr ? info->name : std::string_view{};
}

std::optional<MenuRole> parse_role(std::string_view name) noexcept {
    const std::string wanted = fold(name);
    if (wanted.empty() || wanted == "none") {
        return MenuRole::None;
    }
    for (const RoleInfo& info : kRoles) {
        if (fold(info.name) == wanted) {
            return info.role;
        }
    }
    if (wanted == "exit") {
        return MenuRole::Quit;
    }
    if (wanted == "togglefullscreen" || wanted == "fullscreen") {
        return MenuRole::ToggleFullscreen;
    }
    if (wanted == "devtools") {
        return MenuRole::ToggleDevTools;
    }
    if (wanted == "show") {
        return MenuRole::ShowWindow;
    }
    if (wanted == "hide") {
        return MenuRole::HideWindow;
    }
    return std::nullopt;
}

std::string_view role_default_label(MenuRole role) noexcept {
    const RoleInfo* info = role_info(role);
    return info != nullptr ? info->label : std::string_view{};
}

std::string_view role_default_accelerator(MenuRole role) noexcept {
    const RoleInfo* info = role_info(role);
    return info != nullptr ? info->accelerator : std::string_view{};
}

bool role_is_edit_command(MenuRole role) noexcept {
    const RoleInfo* info = role_info(role);
    return info != nullptr && !info->cdp_command.empty();
}

std::string_view source_name(MenuSource source) noexcept {
    switch (source) {
    case MenuSource::ApplicationMenu:
        return "application";
    case MenuSource::ContextMenu:
        return "context";
    case MenuSource::Tray:
        return "tray";
    case MenuSource::Accelerator:
        return "accelerator";
    }
    return "application";
}

wchar_t mnemonic_of(std::wstring_view label) noexcept {
    for (std::size_t i = 0; i + 1 < label.size(); ++i) {
        if (label[i] != L'&') {
            continue;
        }
        if (label[i + 1] == L'&') {
            ++i;
            continue;
        }
        return static_cast<wchar_t>(std::towupper(static_cast<wint_t>(label[i + 1])));
    }
    return 0;
}

bool perform_role(MenuRole role, const RoleContext& ctx) {
    const RoleInfo* info = role_info(role);
    if (info == nullptr) {
        return false;
    }
    if (!info->cdp_command.empty()) {
        return ctx.webview != nullptr && send_edit_command(*ctx.webview, info->cdp_command);
    }

    switch (role) {
    case MenuRole::Reload:
        return ctx.webview != nullptr && ctx.webview->reload();
    case MenuRole::ForceReload: {
        if (ctx.webview == nullptr || ctx.webview->core_webview() == nullptr) {
            return false;
        }
        return BOSON_HR_CHECK(ctx.webview->core_webview()->CallDevToolsProtocolMethod(
            L"Page.reload",
            L"{\"ignoreCache\":true}",
            Microsoft::WRL::Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
                [](HRESULT hr, LPCWSTR) -> HRESULT {
                    core::hr_check(hr, "Page.reload (forceReload role)");
                    return S_OK;
                })
                .Get()));
    }
    case MenuRole::ToggleDevTools:
        return ctx.webview != nullptr && ctx.webview->open_devtools();
    case MenuRole::ResetZoom:
        return ctx.webview != nullptr && ctx.webview->set_zoom_factor(1.0);
    case MenuRole::ZoomIn:
        return ctx.webview != nullptr && adjust_zoom(*ctx.webview, 0.1);
    case MenuRole::ZoomOut:
        return ctx.webview != nullptr && adjust_zoom(*ctx.webview, -0.1);
    case MenuRole::ToggleFullscreen:
        if (ctx.window == nullptr) {
            return false;
        }
        ctx.window->set_fullscreen(!ctx.window->is_fullscreen());
        return true;
    case MenuRole::Minimize:
        if (ctx.window == nullptr) {
            return false;
        }
        ctx.window->minimize();
        return true;
    case MenuRole::Maximize:
        if (ctx.window == nullptr) {
            return false;
        }
        ctx.window->maximize_or_restore();
        return true;
    case MenuRole::Close:
        if (ctx.window == nullptr) {
            return false;
        }
        ctx.window->close();
        return true;
    case MenuRole::ShowWindow:
        if (ctx.window == nullptr) {
            return false;
        }
        ctx.window->focus();
        return true;
    case MenuRole::HideWindow:
        if (ctx.window == nullptr) {
            return false;
        }
        ctx.window->hide();
        return true;
    case MenuRole::Quit:
        if (ctx.on_quit) {
            ctx.on_quit();
        } else if (ctx.window != nullptr) {
            ctx.window->close();
        } else {
            PostQuitMessage(0);
        }
        return true;
    default:
        return false;
    }
}

void MenuDeleter::operator()(HMENU menu) const noexcept {
    if (menu != nullptr) {
        DestroyMenu(menu);
    }
}

// ---- Menu -----------------------------------------------------------------------------------------------

Menu::Menu(std::vector<MenuItem> items, UINT first_command_id)
    : m_items(std::move(items))
    , m_first_command(std::clamp<UINT>(first_command_id, 1, 0xFF00))
    , m_next_command(m_first_command) {}

void Menu::set_items(std::vector<MenuItem> items) {
    m_items = std::move(items);
    rebuild();
}

namespace {

MenuItem* find_in(std::vector<MenuItem>& items, std::string_view id) noexcept {
    for (MenuItem& item : items) {
        if (item.id == id) {
            return &item;
        }
        if (MenuItem* found = find_in(item.submenu, id); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

MenuItem* Menu::find(std::string_view id) noexcept {
    return id.empty() ? nullptr : find_in(m_items, id);
}

const MenuItem* Menu::find(std::string_view id) const noexcept {
    return id.empty() ? nullptr : find_in(const_cast<std::vector<MenuItem>&>(m_items), id);
}

std::vector<MenuItem>& Menu::siblings_of(const Entry& entry) noexcept {
    return entry.siblings != nullptr ? *entry.siblings : m_items;
}

Menu::Entry* Menu::entry_for(const MenuItem* item) noexcept {
    if (item == nullptr) {
        return nullptr;
    }
    const auto it = std::ranges::find(m_entries, item, &Entry::item);
    return it == m_entries.end() ? nullptr : &*it;
}

const Menu::Entry* Menu::entry_for_command(UINT command) const noexcept {
    if (command == 0) {
        return nullptr;
    }
    const auto it = std::ranges::find(m_entries, command, &Entry::command);
    return it == m_entries.end() ? nullptr : &*it;
}

void Menu::apply_state(const Entry& entry) const {
    if (entry.parent == nullptr || entry.item == nullptr) {
        return;
    }
    MENUITEMINFOW info{};
    info.cbSize = sizeof(MENUITEMINFOW);
    info.fMask = MIIM_STATE;
    info.fState = item_state(*entry.item);
    SetMenuItemInfoW(entry.parent, entry.position, TRUE, &info);
}

void Menu::apply_text(const Entry& entry) const {
    if (entry.parent == nullptr || entry.item == nullptr) {
        return;
    }
    std::wstring text = item_text(*entry.item);
    MENUITEMINFOW info{};
    info.cbSize = sizeof(MENUITEMINFOW);
    info.fMask = MIIM_STRING;
    info.dwTypeData = text.data();
    SetMenuItemInfoW(entry.parent, entry.position, TRUE, &info);
}

void Menu::notify_changed() const {
    if (m_on_changed) {
        const auto callback = m_on_changed;
        callback();
    }
}

bool Menu::set_enabled(std::string_view id, bool enabled) {
    MenuItem* item = find(id);
    if (item == nullptr) {
        return false;
    }
    item->enabled = enabled;
    if (const Entry* entry = entry_for(item); entry != nullptr) {
        apply_state(*entry);
    }
    notify_changed();
    return true;
}

void Menu::uncheck_radio_siblings(std::vector<MenuItem>* siblings_ptr, std::size_t index) {
    std::vector<MenuItem>& siblings = siblings_ptr != nullptr ? *siblings_ptr : m_items;
    const std::string group = siblings[index].radio_group;
    const auto uncheck = [this](MenuItem& other) {
        if (other.checked) {
            other.checked = false;
            if (const Entry* entry = entry_for(&other); entry != nullptr) {
                apply_state(*entry);
            }
        }
    };
    if (!group.empty()) {
        for (std::size_t i = 0; i < siblings.size(); ++i) {
            if (i != index && siblings[i].effective_type() == MenuItemType::Radio && siblings[i].radio_group == group) {
                uncheck(siblings[i]);
            }
        }
        return;
    }
    const auto in_run = [&siblings](std::size_t i) {
        return siblings[i].effective_type() == MenuItemType::Radio && siblings[i].radio_group.empty();
    };
    for (std::size_t i = index; i-- > 0 && in_run(i);) {
        uncheck(siblings[i]);
    }
    for (std::size_t i = index + 1; i < siblings.size() && in_run(i); ++i) {
        uncheck(siblings[i]);
    }
}

namespace {

/// Finds item's container and index by identity.
bool locate(std::vector<MenuItem>& items, const MenuItem* target, std::vector<MenuItem>*& container, std::size_t& index) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (&items[i] == target) {
            container = &items;
            index = i;
            return true;
        }
        if (locate(items[i].submenu, target, container, index)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool Menu::set_checked(std::string_view id, bool checked) {
    MenuItem* item = find(id);
    if (item == nullptr) {
        return false;
    }
    item->checked = checked;
    if (const Entry* entry = entry_for(item); entry != nullptr) {
        apply_state(*entry);
    }
    if (checked && item->effective_type() == MenuItemType::Radio) {
        std::vector<MenuItem>* container = nullptr;
        std::size_t index = 0;
        if (locate(m_items, item, container, index)) {
            uncheck_radio_siblings(container == &m_items ? nullptr : container, index);
        }
    }
    notify_changed();
    return true;
}

bool Menu::set_label(std::string_view id, std::string label) {
    MenuItem* item = find(id);
    if (item == nullptr) {
        return false;
    }
    item->label = std::move(label);
    if (const Entry* entry = entry_for(item); entry != nullptr) {
        apply_text(*entry);
    }
    notify_changed();
    return true;
}

bool Menu::set_accelerator(std::string_view id, std::string accelerator) {
    MenuItem* item = find(id);
    if (item == nullptr) {
        return false;
    }
    item->accelerator = std::move(accelerator);
    if (const Entry* entry = entry_for(item); entry != nullptr) {
        apply_text(*entry);
    }
    notify_changed();
    return true;
}

bool Menu::set_visible(std::string_view id, bool visible) {
    MenuItem* item = find(id);
    if (item == nullptr) {
        return false;
    }
    if (item->visible != visible) {
        item->visible = visible;
        rebuild();
    }
    return true;
}

void Menu::rebuild() {
    UniqueMenu old = std::move(m_handle);
    const bool was_built = old != nullptr;
    m_entries.clear();
    ++m_generation;
    if (was_built) {
        ensure_built(m_style); // build the replacement before the old handle goes away
    }
    notify_changed();
    old.reset();
}

HMENU Menu::handle(MenuStyle style) {
    if (m_handle != nullptr && m_style != style) {
        m_handle.reset();
        m_entries.clear();
        ++m_generation;
    }
    ensure_built(style);
    return m_handle.get();
}

void Menu::ensure_built(MenuStyle style) {
    if (m_handle != nullptr) {
        return;
    }
    m_style = style;
    m_next_command = m_first_command;
    m_entries.clear();
    HMENU root = style == MenuStyle::Bar ? CreateMenu() : CreatePopupMenu();
    if (root == nullptr) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreateMenu");
        return;
    }
    m_handle.reset(root);
    MENUINFO menu_info{};
    menu_info.cbSize = sizeof(MENUINFO);
    menu_info.fMask = MIM_STYLE;
    menu_info.dwStyle = MNS_CHECKORBMP;
    SetMenuInfo(root, &menu_info);
    build_level(root, m_items, -1);
}

void Menu::build_level(HMENU target, std::vector<MenuItem>& items, std::ptrdiff_t parent_entry) {
    UINT position = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        MenuItem& item = items[i];
        if (!item.visible) {
            continue;
        }
        const MenuItemType type = item.effective_type();
        MENUITEMINFOW info{};
        info.cbSize = sizeof(MENUITEMINFOW);
        std::wstring text;
        HMENU child = nullptr;
        UINT command = 0;
        if (type == MenuItemType::Separator) {
            info.fMask = MIIM_FTYPE;
            info.fType = MFT_SEPARATOR;
        } else {
            text = item_text(item);
            info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_STRING | MIIM_ID;
            info.fType = type == MenuItemType::Radio ? MFT_RADIOCHECK : MFT_STRING;
            info.fState = item_state(item);
            info.dwTypeData = text.data();
            if (type == MenuItemType::Submenu) {
                child = CreatePopupMenu();
                if (child == nullptr) {
                    core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreatePopupMenu");
                    continue;
                }
                info.fMask |= MIIM_SUBMENU;
                info.hSubMenu = child;
            } else {
                if (m_next_command >= 0xFFFF) {
                    core::hr_check(E_BOUNDS, "Menu: out of command ids");
                    return;
                }
                command = m_next_command++;
            }
            info.wID = command;
        }
        if (InsertMenuItemW(target, position, TRUE, &info) == FALSE) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "InsertMenuItemW");
            if (child != nullptr) {
                DestroyMenu(child);
            }
            continue;
        }
        m_entries.push_back(Entry{
            .item = &item,
            .siblings = parent_entry < 0 ? nullptr : &items,
            .index = i,
            .parent = target,
            .position = position,
            .command = command,
            .parent_entry = parent_entry,
        });
        const auto self_index = static_cast<std::ptrdiff_t>(m_entries.size() - 1);
        ++position;
        if (child != nullptr) {
            build_level(child, item.submenu, self_index);
        }
    }
}

bool Menu::owns_command(UINT command) const noexcept {
    return entry_for_command(command) != nullptr;
}

std::optional<UINT> Menu::command_for(std::string_view id) const noexcept {
    if (id.empty()) {
        return std::nullopt;
    }
    for (const Entry& entry : m_entries) {
        if (entry.command != 0 && entry.item != nullptr && entry.item->id == id) {
            return entry.command;
        }
    }
    return std::nullopt;
}

const MenuItem* Menu::item_for_command(UINT command) const noexcept {
    const Entry* entry = entry_for_command(command);
    return entry != nullptr ? entry->item : nullptr;
}

bool Menu::is_command_enabled(UINT command) const noexcept {
    const Entry* entry = entry_for_command(command);
    while (entry != nullptr) {
        if (entry->item == nullptr || !entry->item->enabled || !entry->item->visible) {
            return false;
        }
        entry = entry->parent_entry >= 0 ? &m_entries[static_cast<std::size_t>(entry->parent_entry)] : nullptr;
    }
    return true;
}

bool Menu::dispatch(UINT command, MenuSource source, const RoleContext& roles) {
    if (!is_command_enabled(command)) {
        return false;
    }
    const Entry entry = *entry_for_command(command);
    MenuItem& item = *entry.item;
    const MenuItemType type = item.effective_type();
    if (type == MenuItemType::Checkbox) {
        item.checked = !item.checked;
        apply_state(entry);
    } else if (type == MenuItemType::Radio) {
        if (!item.checked) {
            item.checked = true;
            apply_state(entry);
        }
        uncheck_radio_siblings(entry.siblings, entry.index);
    }
    if (type == MenuItemType::Checkbox || type == MenuItemType::Radio) {
        notify_changed();
    }

    // Callbacks may replace the whole tree, so work on copies from here on.
    MenuItem snapshot = item;
    const MenuClick click{.id = snapshot.id, .source = source, .checked = snapshot.checked};
    if (snapshot.on_click) {
        snapshot.on_click(click);
    } else if (snapshot.role != MenuRole::None) {
        perform_role(snapshot.role, roles);
    }
    if (m_listener) {
        const auto listener = m_listener;
        listener(snapshot, click);
    }
    return true;
}

void Menu::for_each_accelerator(const std::function<void(const MenuItem&, UINT, const Accelerator&)>& fn) {
    if (m_handle == nullptr) {
        ensure_built(m_style);
    }
    for (const Entry& entry : m_entries) {
        if (entry.command == 0 || !is_command_enabled(entry.command)) {
            continue;
        }
        const std::string text = entry.item->effective_accelerator();
        if (text.empty()) {
            continue;
        }
        if (const auto accel = Accelerator::parse(text); accel.has_value()) {
            fn(*entry.item, entry.command, *accel);
        }
    }
}

// ---- Theme ----------------------------------------------------------------------------------------------

namespace {

enum class PreferredAppMode : int {
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
};

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using AllowDarkModeForAppFn = BOOL(WINAPI*)(BOOL);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
using FlushMenuThemesFn = void(WINAPI*)();
using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

struct UxThemeApi {
    SetPreferredAppModeFn set_preferred_app_mode = nullptr; // 1903+
    AllowDarkModeForAppFn allow_dark_mode_for_app = nullptr; // 1809 (same ordinal 135)
    AllowDarkModeForWindowFn allow_dark_mode_for_window = nullptr;
    FlushMenuThemesFn flush_menu_themes = nullptr;
    bool loaded = false;
};

[[nodiscard]] DWORD windows_build() noexcept {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return 0;
    }
    const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (rtl_get_version == nullptr) {
        return 0;
    }
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(OSVERSIONINFOW);
    return rtl_get_version(&info) == 0 ? info.dwBuildNumber : 0;
}

[[nodiscard]] FARPROC ordinal(HMODULE module, WORD number) noexcept {
    return GetProcAddress(module, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(number)));
}

[[nodiscard]] const UxThemeApi& uxtheme() noexcept {
    static const UxThemeApi api = []() {
        UxThemeApi out;
        const DWORD build = windows_build();
        if (build < 17763) {
            return out;
        }
        const HMODULE module = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            return out;
        }
        if (build >= 18362) {
            out.set_preferred_app_mode = reinterpret_cast<SetPreferredAppModeFn>(reinterpret_cast<void*>(ordinal(module, 135)));
        } else {
            out.allow_dark_mode_for_app = reinterpret_cast<AllowDarkModeForAppFn>(reinterpret_cast<void*>(ordinal(module, 135)));
        }
        out.allow_dark_mode_for_window = reinterpret_cast<AllowDarkModeForWindowFn>(reinterpret_cast<void*>(ordinal(module, 133)));
        out.flush_menu_themes = reinterpret_cast<FlushMenuThemesFn>(reinterpret_cast<void*>(ordinal(module, 136)));
        out.loaded = (out.set_preferred_app_mode != nullptr || out.allow_dark_mode_for_app != nullptr) &&
                     out.allow_dark_mode_for_window != nullptr && out.flush_menu_themes != nullptr;
        return out;
    }();
    return api;
}

} // namespace

bool dark_menus_supported() noexcept {
    return uxtheme().loaded;
}

bool set_menu_theme(HWND owner, MenuTheme theme) noexcept {
    const UxThemeApi& api = uxtheme();
    if (!api.loaded) {
        return false;
    }
    if (api.set_preferred_app_mode != nullptr) {
        api.set_preferred_app_mode(theme == MenuTheme::Dark    ? PreferredAppMode::ForceDark
                                   : theme == MenuTheme::Light ? PreferredAppMode::ForceLight
                                                               : PreferredAppMode::AllowDark);
    } else {
        api.allow_dark_mode_for_app(theme != MenuTheme::Light ? TRUE : FALSE);
    }
    if (owner != nullptr) {
        api.allow_dark_mode_for_window(owner, theme != MenuTheme::Light ? TRUE : FALSE);
    }
    api.flush_menu_themes();
    return true;
}

// ---- Popups ---------------------------------------------------------------------------------------------

bool is_popup_menu_open() noexcept {
    return t_popup_depth > 0;
}

std::optional<MenuSelection> popup_menu(Menu& menu, const PopupOptions& options) {
    return popup_menu(menu, menu.handle(MenuStyle::Popup), options);
}

std::optional<MenuSelection> popup_menu(Menu& menu, HMENU popup, const PopupOptions& options) {
    if (popup == nullptr || options.owner == nullptr || IsWindow(options.owner) == FALSE || t_popup_depth > 0) {
        return std::nullopt;
    }
    if (options.theme.has_value()) {
        set_menu_theme(options.owner, *options.theme);
    }

    POINT pt{};
    if (options.screen_point.has_value()) {
        pt = *options.screen_point;
    } else {
        GetCursorPos(&pt);
    }

    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | options.align_flags;
    if (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0 && (options.align_flags & (TPM_CENTERALIGN | TPM_RIGHTALIGN)) == 0) {
        flags |= TPM_RIGHTALIGN;
    }
    TPMPARAMS params{};
    params.cbSize = sizeof(TPMPARAMS);
    if (options.exclude_rect.has_value()) {
        params.rcExclude = *options.exclude_rect;
        flags |= TPM_VERTICAL;
    }

    const std::uint64_t generation = menu.generation();
    ++t_popup_depth;
    const BOOL chosen = TrackPopupMenuEx(popup, flags, pt.x, pt.y, options.owner, options.exclude_rect.has_value() ? &params : nullptr);
    --t_popup_depth;
    if (chosen == 0 || menu.generation() != generation) {
        return std::nullopt; // dismissed, or the menu was rebuilt while open (command ids are stale)
    }

    const auto command = static_cast<UINT>(chosen);
    const MenuItem* item = menu.item_for_command(command);
    if (item == nullptr) {
        return std::nullopt;
    }
    MenuSelection selection{.command = command, .id = item->id, .checked = item->checked};
    if (options.dispatch) {
        if (!menu.dispatch(command, options.source, options.roles)) {
            return std::nullopt;
        }
        if (const MenuItem* after = menu.item_for_command(command); after != nullptr && menu.generation() == generation) {
            selection.checked = after->checked;
        }
    } else if (item->effective_type() == MenuItemType::Checkbox) {
        selection.checked = !item->checked;
    } else if (item->effective_type() == MenuItemType::Radio) {
        selection.checked = true;
    }
    return selection;
}

} // namespace boson::menu
