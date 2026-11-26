#pragma once

#include <windows.h>

#include <boson/core/window.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace boson::native {

/// @brief User / system locale and language preferences (BCP 47 tags).
struct LocaleInfo {
    std::string user_locale;                       ///< GetUserDefaultLocaleName, e.g. "pt-BR"
    std::string system_locale;                     ///< GetSystemDefaultLocaleName
    std::string ui_language;                       ///< first preferred UI language
    std::vector<std::string> preferred_languages;  ///< GetUserPreferredUILanguages (Electron getPreferredSystemLanguages)
    std::string region;                            ///< ISO 3166 two-letter code of the user's home region
    std::string decimal_separator;
    std::string list_separator;
    bool time_24h = false;
    std::uint32_t first_day_of_week = 0;           ///< 0 = Monday ... 6 = Sunday (LOCALE_IFIRSTDAYOFWEEK)
    std::uint32_t measurement_metric = 1;          ///< 1 = metric, 0 = US
};

[[nodiscard]] LocaleInfo query_locale();

/// @brief Accessibility / appearance settings a web UI should follow. Accent and high contrast come from
/// titlebar::query_theme (the same source the titlebar uses).
struct SystemPreferences {
    bool dark_mode = false;
    bool high_contrast = false;
    std::string high_contrast_scheme;
    bool reduced_motion = false;       ///< SPI_GETCLIENTAREAANIMATION == FALSE ("Animation effects" off)
    bool transparency_effects = true;
    std::string accent_color;          ///< "#rrggbb"
    double text_scale_factor = 1.0;    ///< Settings > Accessibility > Text size (1.0 .. 2.25)
    bool keyboard_cues = false;        ///< SPI_GETKEYBOARDCUES (always show access-key underlines)
    std::uint32_t caret_blink_ms = 530;///< GetCaretBlinkTime; 0 = no blinking
    std::uint32_t double_click_ms = 500;
    std::uint32_t wheel_scroll_lines = 3;
    bool swap_mouse_buttons = false;

    friend bool operator==(const SystemPreferences&, const SystemPreferences&) = default;
};

[[nodiscard]] SystemPreferences query_system_preferences();

/// @brief Raises a callback when SystemPreferences or LocaleInfo change. Event-driven: it hooks the
/// window's WM_SETTINGCHANGE / WM_THEMECHANGED / WM_SYSCOLORCHANGE / WM_DWMCOLORIZATIONCOLORCHANGED and
/// re-reads the (cheap, in-process) settings only then. UI thread; destroy it before the Window.
class SystemPreferencesWatcher {
public:
    using PreferencesFn = std::function<void(const SystemPreferences& preferences)>;
    using LocaleFn = std::function<void(const LocaleInfo& locale)>;

    explicit SystemPreferencesWatcher(core::Window& window);
    ~SystemPreferencesWatcher();

    SystemPreferencesWatcher(const SystemPreferencesWatcher&) = delete;
    SystemPreferencesWatcher& operator=(const SystemPreferencesWatcher&) = delete;
    SystemPreferencesWatcher(SystemPreferencesWatcher&&) = delete;
    SystemPreferencesWatcher& operator=(SystemPreferencesWatcher&&) = delete;

    void set_preferences_callback(PreferencesFn fn) { m_on_preferences = std::move(fn); }
    void set_locale_callback(LocaleFn fn) { m_on_locale = std::move(fn); }
    [[nodiscard]] const SystemPreferences& preferences() const noexcept { return m_preferences; }
    [[nodiscard]] const LocaleInfo& locale() const noexcept { return m_locale; }
    /// @brief Re-reads both snapshots and notifies for the ones that changed.
    void refresh();

private:
    core::Window& m_window;
    core::Window::ListenerId m_hook = 0;
    SystemPreferences m_preferences;
    LocaleInfo m_locale;
    PreferencesFn m_on_preferences;
    LocaleFn m_on_locale;
};

} // namespace boson::native
