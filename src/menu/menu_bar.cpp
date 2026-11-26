#include <boson/menu/menu_bar.hpp>

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>

#include <algorithm>
#include <cwctype>

namespace boson::menu {

namespace {

// Undocumented messages and structs Windows uses to paint the non-client menu bar ("UAH" = user-mode
// app hooks). Layouts as used by Windows 10/11 and the open-source win32-darkmode project.
constexpr UINT kWmUahDrawMenu = 0x0091;
constexpr UINT kWmUahDrawMenuItem = 0x0092;

struct UahMenu {
    HMENU hmenu;
    HDC hdc;
    DWORD flags;
};

union UahMenuItemMetrics {
    struct {
        DWORD cx;
        DWORD cy;
    } bar[2];
    struct {
        DWORD cx;
        DWORD cy;
    } popup[4];
};

struct UahMenuPopupMetrics {
    DWORD cx[4];
    DWORD update_max_widths : 2;
};

struct UahMenuItem {
    int position;
    UahMenuItemMetrics metrics;
    UahMenuPopupMetrics popup_metrics;
};

struct UahDrawMenuItem {
    DRAWITEMSTRUCT dis;
    UahMenu um;
    UahMenuItem umi;
};

constexpr COLORREF kDarkBarBackground = RGB(32, 32, 32);
constexpr COLORREF kDarkBarHot = RGB(62, 62, 62);
constexpr COLORREF kDarkBarPushed = RGB(77, 77, 77);
constexpr COLORREF kDarkBarText = RGB(255, 255, 255);
constexpr COLORREF kDarkBarDisabledText = RGB(109, 109, 109);

[[nodiscard]] UINT open_message() noexcept {
    static const UINT message = RegisterWindowMessageW(L"Boson.MenuBar.OpenSubmenu");
    return message;
}

[[nodiscard]] bool system_uses_dark_apps() noexcept {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    return status == ERROR_SUCCESS && value == 0;
}

[[nodiscard]] std::string strip_mnemonics(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    for (std::size_t i = 0; i < label.size(); ++i) {
        if (label[i] == '&') {
            if (i + 1 < label.size() && label[i + 1] == '&') {
                out.push_back('&');
                ++i;
            }
            continue;
        }
        out.push_back(label[i]);
    }
    return out;
}

struct SolidBrush {
    HBRUSH brush;
    explicit SolidBrush(COLORREF color) noexcept : brush(CreateSolidBrush(color)) {}
    ~SolidBrush() {
        if (brush != nullptr) {
            DeleteObject(brush);
        }
    }
    SolidBrush(const SolidBrush&) = delete;
    SolidBrush& operator=(const SolidBrush&) = delete;
};

thread_local MenuBar* t_active_bar = nullptr;

} // namespace

MenuBar::MenuBar(core::Window& window, MenuBarOptions options)
    : m_window(window)
    , m_options(options)
    , m_menu({}, options.first_command_id) {
    m_mode = options.mode;
    if (m_mode == MenuBarMode::Auto) {
        m_mode = window.is_headless() ? MenuBarMode::Popups : MenuBarMode::Native;
    }
    m_theme = options.theme.value_or(window.config().dark_mode ? MenuTheme::Dark : MenuTheme::Light);
    m_roles.window = &window;

    m_menu.set_listener([this](const MenuItem& item, const MenuClick& click) {
        if (m_listener) {
            const auto listener = m_listener;
            listener(item, click);
        }
    });
    m_menu.set_changed_callback([this]() { on_menu_changed(); });

    m_accelerators.attach(window);
    m_accelerators.set_unmatched_webview_key_handler(
        [this](const Accelerator& chord, bool system_key) { return on_unmatched_key(chord, system_key); });
    m_hook_id = window.add_message_hook(
        [this](UINT msg, WPARAM w_param, LPARAM l_param) { return on_message(msg, w_param, l_param); });
    if (window.hwnd() != nullptr) {
        set_menu_theme(window.hwnd(), m_theme);
    }
}

MenuBar::~MenuBar() {
    if (t_active_bar == this) {
        t_active_bar = nullptr;
    }
    if (m_msg_filter != nullptr) {
        UnhookWindowsHookEx(m_msg_filter);
        m_msg_filter = nullptr;
    }
    detach_native();
    m_accelerators.detach_all_webviews();
    m_accelerators.detach_window();
    if (m_hook_id != 0) {
        m_window.remove_message_hook(m_hook_id);
    }
}

void MenuBar::set_items(std::vector<MenuItem> items) {
    m_menu.set_items(std::move(items));
    on_menu_changed(); // set_items only rebuilds an HMENU that already existed
}

void MenuBar::clear() {
    set_items({});
}

void MenuBar::rebuild() {
    m_menu.rebuild();
    on_menu_changed();
}

void MenuBar::set_theme(MenuTheme theme) {
    m_theme = theme;
    if (m_window.hwnd() != nullptr) {
        set_menu_theme(m_window.hwnd(), theme);
        if (m_attached) {
            DrawMenuBar(m_window.hwnd());
        }
    }
}

void MenuBar::set_role_context(RoleContext context) {
    m_roles = std::move(context);
    if (m_roles.window == nullptr) {
        m_roles.window = &m_window;
    }
}

bool MenuBar::attach_webview(core::WebView& webview) {
    if (m_roles.webview == nullptr) {
        m_roles.webview = &webview;
    }
    return m_accelerators.attach_webview(webview);
}

void MenuBar::detach_webview(core::WebView& webview) noexcept {
    if (m_roles.webview == &webview) {
        m_roles.webview = nullptr;
    }
    m_accelerators.detach_webview(webview);
}

void MenuBar::on_menu_changed() {
    if (m_menu.items().empty()) {
        detach_native();
    } else {
        const HMENU bar = m_menu.handle(MenuStyle::Bar);
        if (m_mode == MenuBarMode::Native && bar != nullptr) {
            attach_native();
        }
    }
    sync_accelerators();
}

void MenuBar::attach_native() {
    const HWND hwnd = m_window.hwnd();
    const HMENU bar = m_menu.current_handle();
    if (hwnd == nullptr || bar == nullptr) {
        return;
    }
    if (GetMenu(hwnd) != bar) {
        if (SetMenu(hwnd, bar) == FALSE) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "SetMenu");
            return;
        }
    }
    m_attached = true;
    DrawMenuBar(hwnd);
}

void MenuBar::detach_native() noexcept {
    if (!m_attached) {
        return;
    }
    const HWND hwnd = m_window.hwnd();
    if (hwnd != nullptr && IsWindow(hwnd) != FALSE) {
        SetMenu(hwnd, nullptr);
    }
    m_attached = false;
}

void MenuBar::sync_accelerators() {
    for (const AcceleratorTable::Id id : m_bound_accelerators) {
        m_accelerators.remove(id);
    }
    m_bound_accelerators.clear();
    if (!m_options.bind_accelerators || m_menu.items().empty()) {
        return;
    }
    const std::uint64_t generation = m_menu.generation();
    m_menu.for_each_accelerator([this, generation](const MenuItem& item, UINT command, const Accelerator& accel) {
        if (role_is_edit_command(item.role) || m_accelerators.contains(accel)) {
            return; // the WebView performs editing shortcuts natively; first binding wins
        }
        m_bound_accelerators.push_back(m_accelerators.add(accel, [this, command, generation]() {
            if (m_menu.generation() == generation) {
                m_menu.dispatch(command, MenuSource::Accelerator, m_roles);
            }
        }));
    });
}

std::vector<MenuBarEntry> MenuBar::entries() const {
    std::vector<MenuBarEntry> out;
    std::size_t index = 0;
    for (const MenuItem& item : m_menu.items()) {
        if (!item.visible) {
            continue;
        }
        const std::string label = item.effective_label();
        const wchar_t mnemonic = mnemonic_of(core::utf8_to_wide(label));
        out.push_back(MenuBarEntry{
            .index = index++,
            .id = item.id,
            .label = label,
            .text = strip_mnemonics(label),
            .mnemonic = mnemonic != 0 ? core::wide_to_utf8(std::wstring_view(&mnemonic, 1)) : std::string{},
            .enabled = item.enabled,
            .has_submenu = item.effective_type() == MenuItemType::Submenu,
        });
    }
    return out;
}

std::optional<std::size_t> MenuBar::index_of(std::string_view id_or_text) const {
    const auto list = entries();
    for (const MenuBarEntry& entry : list) {
        if (!entry.id.empty() && entry.id == id_or_text) {
            return entry.index;
        }
    }
    for (const MenuBarEntry& entry : list) {
        if (entry.text == id_or_text || entry.label == id_or_text) {
            return entry.index;
        }
    }
    return std::nullopt;
}

void MenuBar::set_anchor_rects(std::vector<RECT> client_px) {
    m_anchors = std::move(client_px);
}

std::optional<std::size_t> MenuBar::index_for_mnemonic(wchar_t mnemonic) const {
    const wchar_t wanted = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(mnemonic)));
    for (const MenuBarEntry& entry : entries()) {
        if (!entry.enabled || entry.mnemonic.empty()) {
            continue;
        }
        const std::wstring m = core::utf8_to_wide(entry.mnemonic);
        if (!m.empty() && m.front() == wanted) {
            return entry.index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> MenuBar::anchor_at(POINT screen_pt) const noexcept {
    const HWND hwnd = m_window.hwnd();
    if (hwnd == nullptr) {
        return std::nullopt;
    }
    POINT client = screen_pt;
    ScreenToClient(hwnd, &client);
    const HMENU bar = m_menu.current_handle();
    for (std::size_t i = 0; i < m_anchors.size(); ++i) {
        if (PtInRect(&m_anchors[i], client) != FALSE && bar != nullptr &&
            GetSubMenu(bar, static_cast<int>(i)) != nullptr) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> MenuBar::step_index(std::size_t from, int delta) const {
    const HMENU bar = m_menu.current_handle();
    if (bar == nullptr) {
        return std::nullopt;
    }
    const int count = GetMenuItemCount(bar);
    if (count <= 1) {
        return std::nullopt;
    }
    int index = static_cast<int>(from);
    for (int step = 0; step < count; ++step) {
        index = (index + delta + count) % count;
        if (index == static_cast<int>(from)) {
            return std::nullopt;
        }
        const auto i = static_cast<std::size_t>(index);
        const bool enabled = (GetMenuState(bar, static_cast<UINT>(index), MF_BYPOSITION) & (MF_DISABLED | MF_GRAYED)) == 0;
        if (enabled && GetSubMenu(bar, index) != nullptr && (m_anchors.empty() || i < m_anchors.size())) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<MenuSelection> MenuBar::open_submenu(std::size_t index, std::optional<RECT> anchor_client_px) {
    const HWND hwnd = m_window.hwnd();
    if (hwnd == nullptr || is_popup_menu_open() || m_menu.items().empty()) {
        return std::nullopt;
    }
    const HMENU bar = m_menu.handle(MenuStyle::Bar);
    if (bar == nullptr || index >= static_cast<std::size_t>(GetMenuItemCount(bar))) {
        return std::nullopt;
    }

    std::optional<std::size_t> current = index;
    std::optional<RECT> anchor = anchor_client_px;
    if (!anchor.has_value() && index < m_anchors.size()) {
        anchor = m_anchors[index];
    }
    std::optional<MenuSelection> result;
    while (current.has_value()) {
        const auto position = static_cast<int>(*current);
        const HMENU sub = GetSubMenu(bar, position);
        if (sub == nullptr) {
            // Top-level leaf: behaves like a button.
            const UINT command = GetMenuItemID(bar, position);
            const MenuItem* item = m_menu.item_for_command(command);
            if (item == nullptr || !m_menu.dispatch(command, MenuSource::ApplicationMenu, m_roles)) {
                return std::nullopt;
            }
            const MenuItem* after = m_menu.item_for_command(command);
            return MenuSelection{.command = command, .id = after != nullptr ? after->id : std::string{}, .checked = after != nullptr && after->checked};
        }

        PopupOptions options;
        options.owner = hwnd;
        options.theme = m_theme;
        options.source = MenuSource::ApplicationMenu;
        options.roles = m_roles;
        if (anchor.has_value()) {
            RECT screen = *anchor;
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&screen), 2);
            options.exclude_rect = screen;
            options.screen_point = POINT{screen.left, screen.bottom};
        }

        m_open_index = current;
        m_pending_switch.reset();
        m_popup_depth = 0;
        m_hot_item_has_submenu = false;
        MenuBar* const previous_active = t_active_bar;
        t_active_bar = this;
        if (m_msg_filter == nullptr) {
            m_msg_filter = SetWindowsHookExW(WH_MSGFILTER, &MenuBar::msg_filter_proc, nullptr, GetCurrentThreadId());
        }
        result = popup_menu(m_menu, sub, options);
        if (m_msg_filter != nullptr) {
            UnhookWindowsHookEx(m_msg_filter);
            m_msg_filter = nullptr;
        }
        t_active_bar = previous_active;
        m_open_index.reset();

        if (result.has_value() || !m_pending_switch.has_value()) {
            break;
        }
        current = m_pending_switch;
        m_pending_switch.reset();
        anchor = *current < m_anchors.size() ? std::optional<RECT>{m_anchors[*current]} : std::nullopt;
    }
    return result;
}

bool MenuBar::activate_keyboard(wchar_t mnemonic) {
    const HWND hwnd = m_window.hwnd();
    if (hwnd == nullptr || m_menu.items().empty()) {
        return false;
    }
    if (m_mode == MenuBarMode::Native) {
        return PostMessageW(hwnd, WM_SYSCOMMAND, SC_KEYMENU, static_cast<LPARAM>(mnemonic)) != FALSE;
    }
    std::optional<std::size_t> index;
    if (mnemonic != 0) {
        index = index_for_mnemonic(mnemonic);
    } else if (const HMENU bar = m_menu.handle(MenuStyle::Bar); bar != nullptr) {
        const int count = GetMenuItemCount(bar);
        for (int i = 0; i < count && !index.has_value(); ++i) {
            if (GetSubMenu(bar, i) != nullptr &&
                (GetMenuState(bar, static_cast<UINT>(i), MF_BYPOSITION) & (MF_DISABLED | MF_GRAYED)) == 0) {
                index = static_cast<std::size_t>(i);
            }
        }
    }
    if (!index.has_value()) {
        return false;
    }
    // Never run a modal menu loop from inside a COM event or hook: post it.
    return PostMessageW(hwnd, open_message(), static_cast<WPARAM>(*index), 0) != FALSE;
}

bool MenuBar::on_unmatched_key(const Accelerator& chord, bool /*system_key*/) {
    if (m_menu.items().empty()) {
        return false;
    }
    if (chord.modifiers == KeyModifiers::Alt &&
        ((chord.key >= 'A' && chord.key <= 'Z') || (chord.key >= '0' && chord.key <= '9'))) {
        const auto mnemonic = static_cast<wchar_t>(chord.key);
        if (index_for_mnemonic(mnemonic).has_value()) {
            return activate_keyboard(mnemonic);
        }
        return false;
    }
    if (chord.modifiers == KeyModifiers::None && chord.key == VK_F10) {
        return activate_keyboard(0);
    }
    return false;
}

LRESULT CALLBACK MenuBar::msg_filter_proc(int code, WPARAM w_param, LPARAM l_param) {
    if (code == MSGF_MENU && t_active_bar != nullptr && l_param != 0) {
        if (t_active_bar->handle_menu_loop_message(*reinterpret_cast<const MSG*>(l_param))) {
            return TRUE;
        }
    }
    return CallNextHookEx(nullptr, code, w_param, l_param);
}

bool MenuBar::handle_menu_loop_message(const MSG& msg) {
    if (!m_open_index.has_value() || m_pending_switch.has_value()) {
        return false;
    }
    if (msg.message == WM_MOUSEMOVE) {
        const auto hovered = anchor_at(msg.pt);
        if (hovered.has_value() && hovered != m_open_index) {
            m_pending_switch = hovered;
            EndMenu();
            return true;
        }
        return false;
    }
    if (msg.message == WM_KEYDOWN && m_popup_depth <= 1) {
        const bool rtl = GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0;
        int delta = 0;
        if (msg.wParam == VK_LEFT) {
            delta = rtl ? 1 : -1;
        } else if (msg.wParam == VK_RIGHT && !m_hot_item_has_submenu) {
            delta = rtl ? -1 : 1;
        }
        if (delta != 0) {
            if (const auto next = step_index(*m_open_index, delta); next.has_value()) {
                m_pending_switch = next;
                EndMenu();
                return true;
            }
        }
    }
    return false;
}

std::optional<LRESULT> MenuBar::on_message(UINT msg, WPARAM w_param, LPARAM l_param) {
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(w_param) == 0 && l_param == 0 && m_mode == MenuBarMode::Native) {
            const UINT command = LOWORD(w_param);
            if (m_menu.owns_command(command)) {
                m_menu.dispatch(command, MenuSource::ApplicationMenu, m_roles);
                return 0;
            }
        }
        break;
    case WM_INITMENUPOPUP:
        if (m_open_index.has_value()) {
            ++m_popup_depth;
        }
        break;
    case WM_UNINITMENUPOPUP:
        if (m_open_index.has_value() && m_popup_depth > 0) {
            --m_popup_depth;
        }
        break;
    case WM_MENUSELECT:
        if (m_open_index.has_value()) {
            const UINT flags = HIWORD(w_param);
            m_hot_item_has_submenu = flags != 0xFFFF && (flags & MF_POPUP) != 0;
        }
        break;
    case kWmUahDrawMenu:
        if (dark_bar() && paint_uah_bar(l_param)) {
            return 0;
        }
        break;
    case kWmUahDrawMenuItem:
        if (dark_bar() && paint_uah_item(l_param)) {
            return 0;
        }
        break;
    case WM_NCPAINT:
    case WM_NCACTIVATE:
        if (dark_bar() && !m_window.is_headless()) {
            const LRESULT result = DefWindowProcW(m_window.hwnd(), msg, w_param, l_param);
            paint_bar_bottom_line();
            return result;
        }
        break;
    case WM_SETTINGCHANGE:
        if (m_attached && !m_options.theme.has_value() && l_param != 0 &&
            std::wstring_view(reinterpret_cast<const wchar_t*>(l_param)) == L"ImmersiveColorSet") {
            DrawMenuBar(m_window.hwnd());
        }
        break;
    default:
        if (msg == open_message()) {
            open_submenu(static_cast<std::size_t>(w_param));
            return 0;
        }
        break;
    }
    return std::nullopt;
}

bool MenuBar::dark_bar() const noexcept {
    if (!m_attached || !m_options.dark_native_bar) {
        return false;
    }
    if (m_theme == MenuTheme::System) {
        return system_uses_dark_apps();
    }
    return m_theme == MenuTheme::Dark;
}

bool MenuBar::paint_uah_bar(LPARAM l_param) const {
    const auto* um = reinterpret_cast<const UahMenu*>(l_param);
    const HWND hwnd = m_window.hwnd();
    if (um == nullptr || um->hdc == nullptr || hwnd == nullptr) {
        return false;
    }
    MENUBARINFO info{};
    info.cbSize = sizeof(MENUBARINFO);
    if (GetMenuBarInfo(hwnd, OBJID_MENU, 0, &info) == FALSE) {
        return false;
    }
    RECT window_rect{};
    GetWindowRect(hwnd, &window_rect);
    RECT bar = info.rcBar;
    OffsetRect(&bar, -window_rect.left, -window_rect.top);
    const SolidBrush brush(kDarkBarBackground);
    FillRect(um->hdc, &bar, brush.brush);
    return true;
}

bool MenuBar::paint_uah_item(LPARAM l_param) const {
    auto* item = reinterpret_cast<UahDrawMenuItem*>(l_param);
    if (item == nullptr || item->um.hdc == nullptr) {
        return false;
    }
    wchar_t text[256]{};
    MENUITEMINFOW info{};
    info.cbSize = sizeof(MENUITEMINFOW);
    info.fMask = MIIM_STRING;
    info.dwTypeData = text;
    info.cch = static_cast<UINT>(std::size(text) - 1);
    if (GetMenuItemInfoW(item->um.hmenu, static_cast<UINT>(item->umi.position), TRUE, &info) == FALSE) {
        return false;
    }
    const UINT state = item->dis.itemState;
    COLORREF background = kDarkBarBackground;
    if ((state & (ODS_SELECTED | ODS_HOTLIGHT)) != 0) {
        background = (state & ODS_SELECTED) != 0 ? kDarkBarPushed : kDarkBarHot;
    }
    const bool disabled = (state & (ODS_GRAYED | ODS_DISABLED | ODS_INACTIVE)) != 0;
    const SolidBrush brush(background);
    FillRect(item->um.hdc, &item->dis.rcItem, brush.brush);
    SetBkMode(item->um.hdc, TRANSPARENT);
    SetTextColor(item->um.hdc, disabled ? kDarkBarDisabledText : kDarkBarText);
    UINT format = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
    if ((state & ODS_NOACCEL) != 0) {
        format |= DT_HIDEPREFIX;
    }
    DrawTextW(item->um.hdc, text, -1, &item->dis.rcItem, format);
    return true;
}

void MenuBar::paint_bar_bottom_line() const {
    const HWND hwnd = m_window.hwnd();
    if (hwnd == nullptr) {
        return;
    }
    MENUBARINFO info{};
    info.cbSize = sizeof(MENUBARINFO);
    if (GetMenuBarInfo(hwnd, OBJID_MENU, 0, &info) == FALSE) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd, &client);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
    RECT window_rect{};
    GetWindowRect(hwnd, &window_rect);
    OffsetRect(&client, -window_rect.left, -window_rect.top);
    RECT line = client;
    line.bottom = line.top;
    line.top -= 1;
    const HDC dc = GetWindowDC(hwnd);
    if (dc == nullptr) {
        return;
    }
    const SolidBrush brush(kDarkBarBackground);
    FillRect(dc, &line, brush.brush);
    ReleaseDC(hwnd, dc);
}

} // namespace boson::menu
