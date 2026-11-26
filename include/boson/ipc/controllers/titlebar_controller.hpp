#pragma once

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/export.hpp>
#include <boson/titlebar/titlebar.hpp>

#include <glaze/glaze.hpp>

#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace boson::ipc {

/// @brief Titlebar.set_overlay params. Colours are CSS strings ("#rrggbb[aa]", "rgb()/rgba()",
/// "transparent"); empty = theme default. height / button_width are DIPs.
struct TitlebarOverlayRequest {
    bool enabled = true;
    std::string color;
    std::string symbol_color;
    std::string hover_color;
    int height = 32;
    int button_width = 46;
    std::string theme = "system"; ///< "system" | "light" | "dark"
    std::optional<bool> rtl;
};

struct TitlebarRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

/// @brief Payload of the "titlebar:overlay" event and of Titlebar.get_overlay / set_overlay (DIPs = CSS px
/// at zoom 1). titlebar_area is the env(titlebar-area-*) equivalent; buttons is the occupied rect.
struct TitlebarOverlayEvent {
    bool visible = false;
    TitlebarRect titlebar_area;
    TitlebarRect buttons;
    unsigned dpi = USER_DEFAULT_SCREEN_DPI;
    bool rtl = false;
    bool maximized = false;
    bool active = true;
};

struct SystemColorsDto {
    std::string window;
    std::string window_text;
    std::string highlight;
    std::string highlight_text;
    std::string button_face;
    std::string button_text;
    std::string gray_text;
    std::string hotlight;
};

/// @brief Payload of the "theme:changed" event and of Titlebar.get_theme.
struct ThemeChangedEvent {
    bool dark_mode = false;
    bool system_dark_mode = false;
    bool high_contrast = false;
    std::string high_contrast_scheme;
    bool transparency_effects = true;
    bool accent_on_title_bars = false;
    std::string accent;
    std::vector<std::string> accent_light; ///< Light1..Light3
    std::vector<std::string> accent_dark;  ///< Dark1..Dark3
    SystemColorsDto colors;
};

/// @brief Titlebar.show_system_menu params: a point in CSS px of the calling WebView, or the cursor.
struct SystemMenuRequest {
    double x = 0.0;
    double y = 0.0;
    bool at_cursor = false;
};

[[nodiscard]] inline TitlebarOverlayEvent to_overlay_event(const titlebar::TitlebarOverlayInfo& info) {
    return TitlebarOverlayEvent{
        .visible = info.visible,
        .titlebar_area = TitlebarRect{info.x, info.y, info.width, info.height},
        .buttons = TitlebarRect{info.buttons_x, info.buttons_y, info.buttons_width, info.buttons_height},
        .dpi = info.dpi,
        .rtl = info.rtl,
        .maximized = info.maximized,
        .active = info.active,
    };
}

[[nodiscard]] inline ThemeChangedEvent to_theme_event(const titlebar::ThemeInfo& theme) {
    using titlebar::to_css_hex;
    ThemeChangedEvent e{
        .dark_mode = theme.dark_mode,
        .system_dark_mode = theme.system_dark_mode,
        .high_contrast = theme.high_contrast,
        .high_contrast_scheme = core::wide_to_utf8(theme.high_contrast_scheme),
        .transparency_effects = theme.transparency_effects,
        .accent_on_title_bars = theme.accent_on_title_bars,
        .accent = to_css_hex(theme.accent),
        .accent_light = {},
        .accent_dark = {},
        .colors = SystemColorsDto{
            .window = to_css_hex(theme.colors.window),
            .window_text = to_css_hex(theme.colors.window_text),
            .highlight = to_css_hex(theme.colors.highlight),
            .highlight_text = to_css_hex(theme.colors.highlight_text),
            .button_face = to_css_hex(theme.colors.button_face),
            .button_text = to_css_hex(theme.colors.button_text),
            .gray_text = to_css_hex(theme.colors.gray_text),
            .hotlight = to_css_hex(theme.colors.hotlight),
        },
    };
    for (const auto& c : theme.accent_light) e.accent_light.push_back(to_css_hex(c));
    for (const auto& c : theme.accent_dark) e.accent_dark.push_back(to_css_hex(c));
    return e;
}

[[nodiscard]] inline titlebar::ColorScheme parse_color_scheme(std::string_view name) {
    std::string lower;
    for (const char ch : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    if (lower.empty() || lower == "system" || lower == "auto") return titlebar::ColorScheme::System;
    if (lower == "light") return titlebar::ColorScheme::Light;
    if (lower == "dark") return titlebar::ColorScheme::Dark;
    throw std::invalid_argument("Unsupported titlebar theme: " + std::string(name));
}

/// @brief Validates and converts a request; throws std::invalid_argument (the bridge turns it into a JSON
/// error) for malformed colours.
[[nodiscard]] inline titlebar::TitlebarOverlayOptions to_overlay_options(const TitlebarOverlayRequest& req) {
    const auto color = [](const std::string& text, const char* field) -> std::optional<titlebar::Rgba> {
        if (text.empty()) return std::nullopt;
        auto parsed = titlebar::parse_css_color(text);
        if (!parsed.has_value()) throw std::invalid_argument(std::string("Invalid CSS colour for ") + field + ": " + text);
        return parsed;
    };
    if (req.height <= 0 || req.height > 400 || req.button_width <= 0 || req.button_width > 400) {
        throw std::invalid_argument("Titlebar overlay height / button_width must be in 1..400 DIPs");
    }
    return titlebar::TitlebarOverlayOptions{
        .color = color(req.color, "color"),
        .symbol_color = color(req.symbol_color, "symbol_color"),
        .hover_color = color(req.hover_color, "hover_color"),
        .height = req.height,
        .button_width = req.button_width,
        .scheme = parse_color_scheme(req.theme),
        .rtl = req.rtl,
    };
}

/// @brief JS: Titlebar.set_overlay / get_overlay / get_theme / show_system_menu. Methods run on the router's
/// pool thread and marshal window work to the UI thread.
class TitlebarController {
public:
    BOSON_CONTROLLER("Titlebar");

    TitlebarController(core::Window& window, core::WebView& webview, exec::UiMarshal& marshal, titlebar::Titlebar& titlebar)
        : m_window(window)
        , m_webview(webview)
        , m_marshal(marshal)
        , m_titlebar(titlebar) {}

    [[nodiscard]] TitlebarOverlayEvent set_overlay(TitlebarOverlayRequest req) const {
        std::optional<titlebar::TitlebarOverlayOptions> options;
        if (req.enabled) {
            options = to_overlay_options(req);
        }
        return m_marshal.invoke_sync([this, options]() {
            m_titlebar.set_overlay(options);
            return to_overlay_event(m_titlebar.overlay_info());
        });
    }

    [[nodiscard]] TitlebarOverlayEvent get_overlay() const {
        return m_marshal.invoke_sync([this]() { return to_overlay_event(m_titlebar.overlay_info()); });
    }

    [[nodiscard]] ThemeChangedEvent get_theme() const {
        return m_marshal.invoke_sync([this]() { return to_theme_event(m_titlebar.theme()); });
    }

    void show_system_menu(SystemMenuRequest req) const {
        // Posted: the menu runs a modal loop on the UI thread; the call returns immediately.
        m_marshal.post_task([this, req]() {
            POINT pt{};
            if (req.at_cursor) {
                GetCursorPos(&pt);
            } else {
                const double scale = static_cast<double>(m_window.dpi()) / USER_DEFAULT_SCREEN_DPI *
                                     m_webview.zoom_factor().value_or(1.0);
                const RECT origin = m_webview.bounds();
                pt = POINT{origin.left + static_cast<LONG>(req.x * scale + 0.5), origin.top + static_cast<LONG>(req.y * scale + 0.5)};
                ClientToScreen(m_window.hwnd(), &pt);
            }
            m_titlebar.show_system_menu(pt);
        });
    }

private:
    core::Window& m_window;
    core::WebView& m_webview;
    exec::UiMarshal& m_marshal;
    titlebar::Titlebar& m_titlebar;
};

/// @brief Pushes "titlebar:overlay" and "theme:changed" to every WebView attached to bridge. UI thread;
/// bridge must outlive titlebar's listeners (i.e. the Titlebar).
inline void connect_titlebar_events(titlebar::Titlebar& titlebar, Bridge& bridge) {
    titlebar.on_overlay_changed([&bridge](const titlebar::TitlebarOverlayInfo& info) {
        bridge.emit("titlebar:overlay", to_overlay_event(info));
    });
    titlebar.theme_watcher().on_changed([&bridge](const titlebar::ThemeInfo& theme) {
        bridge.emit("theme:changed", to_theme_event(theme));
    });
}

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::TitlebarController> {
    using T = boson::ipc::TitlebarController;
    static constexpr auto value = glz::object(
        "set_overlay", &T::set_overlay,
        "get_overlay", &T::get_overlay,
        "get_theme", &T::get_theme,
        "show_system_menu", &T::show_system_menu);
};
