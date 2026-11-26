#pragma once

#include <windows.h>

#include <boson/core/ui_timer.hpp>
#include <boson/core/window.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::titlebar {

/// @brief Straight (non-premultiplied) 8-bit RGBA colour.
struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    friend bool operator==(const Rgba&, const Rgba&) = default;
};

[[nodiscard]] constexpr Rgba rgba_from_colorref(COLORREF color, std::uint8_t alpha = 255) noexcept {
    return Rgba{GetRValue(color), GetGValue(color), GetBValue(color), alpha};
}

/// @brief Parses "#rgb", "#rgba", "#rrggbb", "#rrggbbaa", "rgb(r,g,b)", "rgba(r,g,b,a)" (a in 0..1) and
/// "transparent". Returns std::nullopt for anything else.
[[nodiscard]] std::optional<Rgba> parse_css_color(std::string_view text) noexcept;
/// @brief "#rrggbb", or "#rrggbbaa" when alpha < 255.
[[nodiscard]] std::string to_css_hex(Rgba color);

/// @brief GetSysColor values that matter for high-contrast-aware titlebars.
struct SystemColors {
    Rgba window;
    Rgba window_text;
    Rgba highlight;
    Rgba highlight_text;
    Rgba button_face;
    Rgba button_text;
    Rgba gray_text;
    Rgba hotlight;

    friend bool operator==(const SystemColors&, const SystemColors&) = default;
};

/// @brief Snapshot of the OS appearance settings a custom titlebar needs to follow.
struct ThemeInfo {
    bool dark_mode = false;            ///< Apps theme (Personalize\AppsUseLightTheme == 0).
    bool system_dark_mode = false;     ///< Taskbar / Start theme (SystemUsesLightTheme == 0).
    bool high_contrast = false;        ///< SPI_GETHIGHCONTRAST / HCF_HIGHCONTRASTON.
    std::wstring high_contrast_scheme; ///< Name of the active contrast theme, when reported.
    bool transparency_effects = true;  ///< Personalize\EnableTransparency.
    bool accent_on_title_bars = false; ///< DWM\ColorPrevalence ("Show accent colour on title bars").
    Rgba accent{0, 120, 212, 255};
    std::array<Rgba, 3> accent_light{}; ///< SystemAccentColorLight1..3
    std::array<Rgba, 3> accent_dark{};  ///< SystemAccentColorDark1..3
    SystemColors colors;

    friend bool operator==(const ThemeInfo&, const ThemeInfo&) = default;
};

/// @brief Reads the current settings (HKCU registry values, SystemParametersInfo, GetSysColor). Cheap: no
/// cross-process calls, safe on the UI thread.
[[nodiscard]] ThemeInfo query_theme();

/// @brief Watches a Window for WM_SETTINGCHANGE / WM_THEMECHANGED / WM_SYSCOLORCHANGE /
/// WM_DWMCOLORIZATIONCOLORCHANGED, re-reads the theme (coalesced through a short UI timer) and notifies
/// listeners when something changed. UI thread only; must be destroyed before its Window.
class ThemeWatcher {
public:
    using QueryFn = std::function<ThemeInfo()>;
    using Listener = std::function<void(const ThemeInfo& theme)>;
    using ListenerId = std::uint64_t;

    explicit ThemeWatcher(core::Window& window, QueryFn query = {});
    ~ThemeWatcher();

    ThemeWatcher(const ThemeWatcher&) = delete;
    ThemeWatcher& operator=(const ThemeWatcher&) = delete;
    ThemeWatcher(ThemeWatcher&&) = delete;
    ThemeWatcher& operator=(ThemeWatcher&&) = delete;

    [[nodiscard]] const ThemeInfo& current() const noexcept { return m_current; }
    ListenerId on_changed(Listener listener);
    bool off(ListenerId id) noexcept;

    /// @brief Re-reads the theme now; notifies when it differs from current() (or always when force).
    void refresh(bool force = false);
    /// @brief Replaces the query function (tests inject fake settings through this).
    void set_query(QueryFn query);

private:
    std::optional<LRESULT> on_message(UINT msg, WPARAM w_param, LPARAM l_param);

    core::Window& m_window;
    QueryFn m_query;
    ThemeInfo m_current;
    core::UiTimer m_debounce;
    core::Window::ListenerId m_hook = 0;
    struct Entry {
        ListenerId id = 0;
        Listener fn;
    };
    std::vector<Entry> m_listeners;
    ListenerId m_next_id = 1;
};

} // namespace boson::titlebar
