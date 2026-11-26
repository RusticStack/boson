#pragma once

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/export.hpp>
#include <boson/ipc/system_controller.hpp>
#include <boson/menu/accelerator.hpp>
#include <boson/menu/menu.hpp>
#include <boson/menu/menu_bar.hpp>
#include <boson/native/notification.hpp>
#include <boson/native/tray.hpp>

#include <glaze/glaze.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace boson::ipc {

// ---- Wire types -----------------------------------------------------------------------------------------

/// @brief JSON form of menu::MenuItem. type: "normal" | "separator" | "checkbox" | "radio" | "submenu"
/// (default "normal", or "submenu" when submenu is non-empty). role: see menu::parse_role ("copy", "quit"...).
struct MenuItemTemplate {
    std::string id;
    std::string label;
    std::string accelerator;
    std::string type;
    std::string role;
    bool enabled = true;
    bool checked = false;
    bool visible = true;
    std::string group;
    std::vector<MenuItemTemplate> submenu;
};

struct MenuTemplateRequest {
    std::vector<MenuItemTemplate> items;
};

/// @brief CSS pixels relative to the calling WebView (getBoundingClientRect()).
struct CssRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct MenuBarEntryInfo {
    std::uint32_t index = 0;
    std::string id;
    std::string label;
    std::string text;
    std::string mnemonic;
    bool enabled = true;
    bool has_submenu = false;
};

struct MenuBarInfo {
    std::string mode; ///< "native" or "popups"
    std::vector<MenuBarEntryInfo> entries;
};

/// @brief Opens a top-level submenu of the application menu below rect (or the anchor registered with
/// set_application_menu_anchors). id wins over index when both are given.
struct PopupApplicationMenuRequest {
    std::string id;
    std::int32_t index = -1;
    std::optional<CssRect> rect;
};

struct MenuAnchorsRequest {
    std::vector<CssRect> rects;
};

/// @brief x/y in CSS pixels relative to the calling WebView; omit both to open at the mouse cursor.
struct ContextMenuRequest {
    std::vector<MenuItemTemplate> items;
    std::optional<double> x;
    std::optional<double> y;
};

struct MenuPopupResult {
    bool selected = false;
    std::string id;
    bool checked = false;
};

/// @brief Dynamic update of one item. target: "application" (default) or "tray".
struct MenuItemUpdate {
    std::string id;
    std::string target = "application";
    std::optional<bool> enabled;
    std::optional<bool> checked;
    std::optional<bool> visible;
    std::optional<std::string> label;
    std::optional<std::string> accelerator;
};

struct TrayShowRequest {
    std::string tooltip;
    /// PNG / ICO bytes as base64 (a "data:image/...;base64," prefix is accepted). Empty keeps the current icon.
    std::string icon_base64;
    std::vector<MenuItemTemplate> menu;
    bool menu_on_left_click = false;
};

struct TrayTooltipRequest {
    std::string tooltip;
};

struct TrayIconRequest {
    std::string icon_base64;
};

/// @brief Payload of the "menu:click" event. source: "application" | "context" | "tray" | "accelerator".
struct MenuClickEvent {
    std::string id;
    std::string source;
    bool checked = false;
};

/// @brief Payload of the "tray:click" event. button: "left" | "right" | "double" | "middle" | "keyboard" |
/// "balloon-click" | "balloon-dismissed". x/y are screen pixels.
struct TrayClickEvent {
    std::string button;
    std::int32_t x = 0;
    std::int32_t y = 0;
};

/// @brief Event registry for TsGenerator::generate_with_events (see glz::meta below).
struct MenuEvents {
    MenuClickEvent menu_click;
    TrayClickEvent tray_click;
};

// ---- Conversion helpers ---------------------------------------------------------------------------------

[[nodiscard]] inline menu::MenuItemType parse_menu_item_type(std::string_view name, bool has_children) {
    std::string lower;
    for (const char ch : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lower.empty()) return has_children ? menu::MenuItemType::Submenu : menu::MenuItemType::Normal;
    if (lower == "normal" || lower == "action" || lower == "item") return menu::MenuItemType::Normal;
    if (lower == "separator") return menu::MenuItemType::Separator;
    if (lower == "checkbox" || lower == "check") return menu::MenuItemType::Checkbox;
    if (lower == "radio") return menu::MenuItemType::Radio;
    if (lower == "submenu") return menu::MenuItemType::Submenu;
    throw std::invalid_argument("Unsupported menu item type: " + std::string(name));
}

/// @brief Converts a JSON template into a MenuItem tree. Throws std::invalid_argument on an unknown type,
/// role or accelerator, or on nesting deeper than 16 levels.
[[nodiscard]] inline std::vector<menu::MenuItem> to_menu_items(const std::vector<MenuItemTemplate>& templates, int depth = 0) {
    if (depth > 16) {
        throw std::invalid_argument("Menu template nested deeper than 16 levels");
    }
    std::vector<menu::MenuItem> out;
    out.reserve(templates.size());
    for (const MenuItemTemplate& t : templates) {
        menu::MenuItem item;
        item.id = t.id;
        item.label = t.label;
        item.accelerator = t.accelerator;
        item.type = parse_menu_item_type(t.type, !t.submenu.empty());
        const auto role = menu::parse_role(t.role);
        if (!role.has_value()) {
            throw std::invalid_argument("Unsupported menu role: " + t.role);
        }
        item.role = *role;
        if (item.id.empty() && item.role != menu::MenuRole::None) {
            item.id = std::string(menu::role_name(item.role));
        }
        if (!t.accelerator.empty() && !menu::Accelerator::parse(t.accelerator).has_value()) {
            throw std::invalid_argument("Invalid accelerator: " + t.accelerator);
        }
        item.enabled = t.enabled;
        item.checked = t.checked;
        item.visible = t.visible;
        item.radio_group = t.group;
        item.submenu = to_menu_items(t.submenu, depth + 1);
        if (item.type == menu::MenuItemType::Submenu && item.submenu.empty() && depth == 0) {
            item.type = menu::MenuItemType::Normal; // an empty top-level submenu behaves like a button
        }
        out.push_back(std::move(item));
    }
    return out;
}

/// @brief Standard base64 (RFC 4648, padding optional, whitespace ignored); accepts a data: URL.
/// Throws std::invalid_argument on invalid input.
[[nodiscard]] inline std::vector<std::byte> decode_base64(std::string_view text) {
    if (text.starts_with("data:")) {
        const std::size_t comma = text.find(',');
        if (comma == std::string_view::npos) {
            throw std::invalid_argument("Malformed data URL");
        }
        text.remove_prefix(comma + 1);
    }
    const auto value_of = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+' || ch == '-') return 62;
        if (ch == '/' || ch == '_') return 63;
        return -1;
    };
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    bool padding = false;
    for (const char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            continue;
        }
        if (ch == '=') {
            padding = true;
            continue;
        }
        const int v = value_of(ch);
        if (v < 0 || padding) {
            throw std::invalid_argument("Invalid base64 data");
        }
        buffer = (buffer << 6U) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> static_cast<std::uint32_t>(bits)) & 0xFFU));
        }
    }
    return out;
}

// ---- Backend (UI thread) --------------------------------------------------------------------------------

/// @brief UI-thread state behind MenuController: the window's MenuBar, the lazily created TrayIcon and the
/// "menu:click" / "tray:click" event forwarding. Construct and use it on the UI thread.
class MenuBackend {
public:
    MenuBackend(core::Window& window, core::WebView& webview, menu::MenuBarOptions options = {})
        : m_window(window)
        , m_webview(webview)
        , m_bar(window, options) {
        m_bar.set_role_context(menu::RoleContext{.window = &window, .webview = &webview, .on_quit = {}});
        m_bar.set_click_listener([this](const menu::MenuItem& item, const menu::MenuClick& click) { forward(item, click); });
        attach_webview();
    }

    MenuBackend(const MenuBackend&) = delete;
    MenuBackend& operator=(const MenuBackend&) = delete;
    MenuBackend(MenuBackend&&) = delete;
    MenuBackend& operator=(MenuBackend&&) = delete;
    ~MenuBackend() = default;

    /// @brief Events go to every WebView attached to bridge. Pass nullptr to stop emitting.
    void set_bridge(Bridge* bridge) noexcept { m_bridge = bridge; }
    /// @brief C++ observer of every menu click (application, context, tray, accelerator).
    void set_click_listener(std::function<void(const menu::MenuItem&, const menu::MenuClick&)> fn) { m_listener = std::move(fn); }
    void set_tray_listener(std::function<void(const native::TrayEvent&)> fn) { m_tray_listener = std::move(fn); }
    /// @brief Overrides the Quit role (default: close the window).
    void set_quit_handler(std::function<void()> fn) {
        m_on_quit = std::move(fn);
        menu::RoleContext roles = m_bar.role_context();
        roles.on_quit = m_on_quit;
        m_bar.set_role_context(std::move(roles));
        if (m_tray != nullptr) {
            m_tray->set_role_context(role_context());
        }
    }

    /// @brief Hooks AcceleratorKeyPressed once the WebView is ready (called again by every RPC).
    void attach_webview() {
        if (!m_webview_attached && m_webview.controller() != nullptr) {
            m_webview_attached = m_bar.attach_webview(m_webview);
        }
    }

    [[nodiscard]] menu::MenuBar& menu_bar() noexcept { return m_bar; }
    [[nodiscard]] core::Window& window() noexcept { return m_window; }
    [[nodiscard]] core::WebView& webview() noexcept { return m_webview; }
    [[nodiscard]] bool has_tray() const noexcept { return m_tray != nullptr; }

    [[nodiscard]] native::TrayIcon& tray() {
        if (m_tray == nullptr) {
            m_tray = std::make_unique<native::TrayIcon>(m_window.hwnd());
            m_tray->set_role_context(role_context());
            m_tray->set_menu_theme(m_bar.theme());
            m_tray->set_menu_listener([this](const menu::MenuItem& item, const menu::MenuClick& click) { forward(item, click); });
            m_tray->set_event_callback([this](const native::TrayEvent& event) {
                if (m_tray_listener) {
                    m_tray_listener(event);
                }
                if (m_bridge != nullptr) {
                    m_bridge->emit("tray:click", TrayClickEvent{
                        .button = std::string(native::tray_event_name(event.kind)),
                        .x = event.position.x,
                        .y = event.position.y,
                    });
                }
            });
        }
        return *m_tray;
    }

    /// @brief Maps a CSS-pixel rect of the WebView to window client pixels.
    [[nodiscard]] RECT css_to_client(const CssRect& rect) const {
        const double scale = static_cast<double>(m_window.dpi()) / USER_DEFAULT_SCREEN_DPI * m_webview.zoom_factor().value_or(1.0);
        const RECT origin = m_webview.bounds();
        const auto px = [scale](double css) { return static_cast<LONG>(css * scale + (css >= 0.0 ? 0.5 : -0.5)); };
        return RECT{
            origin.left + px(rect.x),
            origin.top + px(rect.y),
            origin.left + px(rect.x + rect.width),
            origin.top + px(rect.y + rect.height),
        };
    }

    /// @brief Shows a one-off context menu; clicks are forwarded like any other menu click.
    std::optional<menu::MenuSelection> popup_context_menu(std::vector<menu::MenuItem> items, std::optional<POINT> client_px) {
        if (menu::is_popup_menu_open()) {
            return std::nullopt;
        }
        menu::Menu context(std::move(items));
        context.set_listener([this](const menu::MenuItem& item, const menu::MenuClick& click) { forward(item, click); });
        menu::PopupOptions options;
        options.owner = m_window.hwnd();
        options.theme = m_bar.theme();
        options.source = menu::MenuSource::ContextMenu;
        options.roles = role_context();
        if (client_px.has_value()) {
            POINT screen = *client_px;
            ClientToScreen(m_window.hwnd(), &screen);
            options.screen_point = screen;
        }
        return menu::popup_menu(context, options);
    }

    [[nodiscard]] menu::RoleContext role_context() const {
        return menu::RoleContext{.window = &m_window, .webview = &m_webview, .on_quit = m_on_quit};
    }

private:
    void forward(const menu::MenuItem& item, const menu::MenuClick& click) {
        if (m_listener) {
            m_listener(item, click);
        }
        if (m_bridge != nullptr) {
            m_bridge->emit("menu:click", MenuClickEvent{
                .id = click.id,
                .source = std::string(menu::source_name(click.source)),
                .checked = click.checked,
            });
        }
    }

    core::Window& m_window;
    core::WebView& m_webview;
    menu::MenuBar m_bar;
    std::unique_ptr<native::TrayIcon> m_tray;
    Bridge* m_bridge = nullptr;
    bool m_webview_attached = false;
    std::function<void(const menu::MenuItem&, const menu::MenuClick&)> m_listener;
    std::function<void(const native::TrayEvent&)> m_tray_listener;
    std::function<void()> m_on_quit;
};

// ---- Controller (pool threads) --------------------------------------------------------------------------

/// @brief JS API "Menu.*". Every method marshals to the UI thread. Popup methods hold their pool thread
/// until the user picks or dismisses (the UI thread keeps pumping inside the native menu loop).
class MenuController {
public:
    BOSON_CONTROLLER("Menu");

    MenuController(MenuBackend& backend, exec::UiMarshal& marshal)
        : m_backend(backend)
        , m_marshal(marshal) {}

    [[nodiscard]] BoolResponse set_application_menu(MenuTemplateRequest req) const {
        auto items = to_menu_items(req.items);
        return m_marshal.invoke_sync([this, items = std::move(items)]() mutable {
            m_backend.attach_webview();
            m_backend.menu_bar().set_items(std::move(items));
            return BoolResponse{.value = true};
        });
    }

    void clear_application_menu() const {
        m_marshal.post_task([this]() { m_backend.menu_bar().clear(); });
    }

    [[nodiscard]] MenuBarInfo get_application_menu() const {
        return m_marshal.invoke_sync([this]() {
            MenuBarInfo info;
            info.mode = m_backend.menu_bar().is_native() ? "native" : "popups";
            for (const menu::MenuBarEntry& e : m_backend.menu_bar().entries()) {
                info.entries.push_back(MenuBarEntryInfo{
                    .index = static_cast<std::uint32_t>(e.index),
                    .id = e.id,
                    .label = e.label,
                    .text = e.text,
                    .mnemonic = e.mnemonic,
                    .enabled = e.enabled,
                    .has_submenu = e.has_submenu,
                });
            }
            return info;
        });
    }

    void set_application_menu_anchors(MenuAnchorsRequest req) const {
        m_marshal.post_task([this, rects = std::move(req.rects)]() {
            std::vector<RECT> client;
            client.reserve(rects.size());
            for (const CssRect& r : rects) {
                client.push_back(m_backend.css_to_client(r));
            }
            m_backend.menu_bar().set_anchor_rects(std::move(client));
        });
    }

    [[nodiscard]] MenuPopupResult popup_application_menu(PopupApplicationMenuRequest req) const {
        return m_marshal.invoke_sync([this, req = std::move(req)]() {
            menu::MenuBar& bar = m_backend.menu_bar();
            std::optional<std::size_t> index;
            if (!req.id.empty()) {
                index = bar.index_of(req.id);
            } else if (req.index >= 0) {
                index = static_cast<std::size_t>(req.index);
            }
            if (!index.has_value()) {
                throw std::invalid_argument("Unknown application menu entry");
            }
            std::optional<RECT> anchor;
            if (req.rect.has_value()) {
                anchor = m_backend.css_to_client(*req.rect);
            }
            return to_result(bar.open_submenu(*index, anchor));
        });
    }

    [[nodiscard]] MenuPopupResult popup_context_menu(ContextMenuRequest req) const {
        auto items = to_menu_items(req.items);
        return m_marshal.invoke_sync([this, items = std::move(items), x = req.x, y = req.y]() mutable {
            std::optional<POINT> client;
            if (x.has_value() && y.has_value()) {
                const RECT r = m_backend.css_to_client(CssRect{.x = *x, .y = *y, .width = 0.0, .height = 0.0});
                client = POINT{r.left, r.top};
            }
            return to_result(m_backend.popup_context_menu(std::move(items), client));
        });
    }

    [[nodiscard]] BoolResponse update_menu_item(MenuItemUpdate req) const {
        if (req.target != "application" && req.target != "tray") {
            throw std::invalid_argument("Unknown menu target: " + req.target);
        }
        if (req.accelerator.has_value() && !req.accelerator->empty() && !menu::Accelerator::parse(*req.accelerator).has_value()) {
            throw std::invalid_argument("Invalid accelerator: " + *req.accelerator);
        }
        return m_marshal.invoke_sync([this, req = std::move(req)]() {
            menu::Menu& target = req.target == "tray" ? m_backend.tray().menu() : m_backend.menu_bar().menu();
            if (target.find(req.id) == nullptr) {
                return BoolResponse{.value = false};
            }
            if (req.label.has_value()) target.set_label(req.id, *req.label);
            if (req.accelerator.has_value()) target.set_accelerator(req.id, *req.accelerator);
            if (req.enabled.has_value()) target.set_enabled(req.id, *req.enabled);
            if (req.checked.has_value()) target.set_checked(req.id, *req.checked);
            if (req.visible.has_value()) target.set_visible(req.id, *req.visible);
            return BoolResponse{.value = true};
        });
    }

    [[nodiscard]] BoolResponse tray_show(TrayShowRequest req) const {
        auto items = to_menu_items(req.menu);
        std::vector<std::byte> icon;
        if (!req.icon_base64.empty()) {
            icon = decode_base64(req.icon_base64);
        }
        return m_marshal.invoke_sync([this, items = std::move(items), icon = std::move(icon), req = std::move(req)]() mutable {
            native::TrayIcon& tray = m_backend.tray();
            if (!icon.empty() && !tray.set_icon_from_bytes(icon)) {
                throw std::invalid_argument("icon_base64 is not a decodable PNG/ICO image");
            }
            tray.set_tooltip(req.tooltip);
            tray.set_menu(std::move(items));
            tray.set_menu_on_left_click(req.menu_on_left_click);
            return BoolResponse{.value = tray.show()};
        });
    }

    void tray_hide() const {
        m_marshal.post_task([this]() {
            if (m_backend.has_tray()) {
                m_backend.tray().hide();
            }
        });
    }

    [[nodiscard]] BoolResponse tray_set_menu(MenuTemplateRequest req) const {
        auto items = to_menu_items(req.items);
        return m_marshal.invoke_sync([this, items = std::move(items)]() mutable {
            m_backend.tray().set_menu(std::move(items));
            return BoolResponse{.value = true};
        });
    }

    [[nodiscard]] BoolResponse tray_set_tooltip(TrayTooltipRequest req) const {
        return m_marshal.invoke_sync([this, tooltip = std::move(req.tooltip)]() {
            return BoolResponse{.value = m_backend.tray().set_tooltip(tooltip)};
        });
    }

    [[nodiscard]] BoolResponse tray_set_icon(TrayIconRequest req) const {
        const std::vector<std::byte> icon = decode_base64(req.icon_base64);
        return m_marshal.invoke_sync([this, &icon]() {
            return BoolResponse{.value = m_backend.tray().set_icon_from_bytes(icon)};
        });
    }

    [[nodiscard]] BoolResponse tray_show_balloon(NotifyRequest req) const {
        native::NotificationOptions options{
            .title = std::move(req.title),
            .body = std::move(req.body),
            .level = parse_notification_level(req.level),
            .silent = req.silent,
        };
        return m_marshal.invoke_sync([this, options = std::move(options)]() {
            return BoolResponse{.value = m_backend.tray().show_balloon(options)};
        });
    }

private:
    [[nodiscard]] static MenuPopupResult to_result(const std::optional<menu::MenuSelection>& selection) {
        if (!selection.has_value()) {
            return MenuPopupResult{};
        }
        return MenuPopupResult{.selected = true, .id = selection->id, .checked = selection->checked};
    }

    MenuBackend& m_backend;
    exec::UiMarshal& m_marshal;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::MenuEvents> {
    using T = boson::ipc::MenuEvents;
    static constexpr auto value = glz::object(
        "menu:click", &T::menu_click,
        "tray:click", &T::tray_click);
};

template <>
struct glz::meta<boson::ipc::MenuController> {
    using T = boson::ipc::MenuController;
    static constexpr auto value = glz::object(
        "set_application_menu", &T::set_application_menu,
        "clear_application_menu", &T::clear_application_menu,
        "get_application_menu", &T::get_application_menu,
        "set_application_menu_anchors", &T::set_application_menu_anchors,
        "popup_application_menu", &T::popup_application_menu,
        "popup_context_menu", &T::popup_context_menu,
        "update_menu_item", &T::update_menu_item,
        "tray_show", &T::tray_show,
        "tray_hide", &T::tray_hide,
        "tray_set_menu", &T::tray_set_menu,
        "tray_set_tooltip", &T::tray_set_tooltip,
        "tray_set_icon", &T::tray_set_icon,
        "tray_show_balloon", &T::tray_show_balloon);
};
