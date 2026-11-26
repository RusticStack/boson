#include <boson/native/system_info.hpp>

#include <boson/core/result.hpp>
#include <boson/titlebar/theme.hpp>

#include <array>
#include <cstdlib>
#include <vector>

namespace boson::native {

namespace {

[[nodiscard]] std::string locale_string(LPCWSTR locale, LCTYPE type) {
    std::array<wchar_t, 128> buffer{};
    const int len = GetLocaleInfoEx(locale, type, buffer.data(), static_cast<int>(buffer.size()));
    if (len <= 1) {
        return {};
    }
    return core::wide_to_utf8(std::wstring_view{buffer.data(), static_cast<std::size_t>(len - 1)});
}

[[nodiscard]] std::uint32_t locale_number(LPCWSTR locale, LCTYPE type, std::uint32_t fallback) {
    DWORD value = 0;
    if (GetLocaleInfoEx(locale, type | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&value),
                        static_cast<int>(sizeof(value) / sizeof(wchar_t))) == 0) {
        return fallback;
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] bool spi_bool(UINT action, bool fallback) noexcept {
    BOOL value = fallback ? TRUE : FALSE;
    if (SystemParametersInfoW(action, 0, &value, 0) == FALSE) {
        return fallback;
    }
    return value != FALSE;
}

[[nodiscard]] double read_text_scale() noexcept {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS ||
        value < 100) {
        return 1.0;
    }
    return static_cast<double>(value) / 100.0;
}

} // namespace

LocaleInfo query_locale() {
    LocaleInfo info;
    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> name{};
    if (GetUserDefaultLocaleName(name.data(), static_cast<int>(name.size())) > 0) {
        info.user_locale = core::wide_to_utf8(name.data());
    }
    name.fill(L'\0');
    if (GetSystemDefaultLocaleName(name.data(), static_cast<int>(name.size())) > 0) {
        info.system_locale = core::wide_to_utf8(name.data());
    }

    ULONG count = 0;
    ULONG chars = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &chars) != FALSE && chars > 0) {
        std::vector<wchar_t> buffer(chars);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &chars) != FALSE) {
            const wchar_t* cursor = buffer.data();
            while (*cursor != L'\0') {
                const std::wstring_view lang{cursor};
                info.preferred_languages.push_back(core::wide_to_utf8(lang));
                cursor += lang.size() + 1;
            }
        }
    }
    if (!info.preferred_languages.empty()) {
        info.ui_language = info.preferred_languages.front();
    }

    std::array<wchar_t, 16> region{};
    const GEOID geo = GetUserGeoID(GEOCLASS_NATION);
    if (geo != GEOID_NOT_AVAILABLE && GetGeoInfoW(geo, GEO_ISO2, region.data(), static_cast<int>(region.size()), 0) > 0) {
        info.region = core::wide_to_utf8(region.data());
    }

    info.decimal_separator = locale_string(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL);
    info.list_separator = locale_string(LOCALE_NAME_USER_DEFAULT, LOCALE_SLIST);
    info.time_24h = locale_number(LOCALE_NAME_USER_DEFAULT, LOCALE_ITIME, 0) == 1;
    info.first_day_of_week = locale_number(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, 0);
    info.measurement_metric = locale_number(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE, 0) == 0 ? 1U : 0U;
    return info;
}

SystemPreferences query_system_preferences() {
    const titlebar::ThemeInfo theme = titlebar::query_theme();
    SystemPreferences prefs;
    prefs.dark_mode = theme.dark_mode;
    prefs.high_contrast = theme.high_contrast;
    prefs.high_contrast_scheme = core::wide_to_utf8(theme.high_contrast_scheme);
    prefs.transparency_effects = theme.transparency_effects;
    prefs.accent_color = titlebar::to_css_hex(theme.accent);
    prefs.reduced_motion = !spi_bool(SPI_GETCLIENTAREAANIMATION, true);
    prefs.keyboard_cues = spi_bool(SPI_GETKEYBOARDCUES, false);
    prefs.text_scale_factor = read_text_scale();
    const UINT blink = GetCaretBlinkTime();
    prefs.caret_blink_ms = blink == INFINITE ? 0U : static_cast<std::uint32_t>(blink);
    prefs.double_click_ms = static_cast<std::uint32_t>(GetDoubleClickTime());
    UINT lines = 3;
    if (SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0) != FALSE) {
        prefs.wheel_scroll_lines = static_cast<std::uint32_t>(lines);
    }
    prefs.swap_mouse_buttons = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    return prefs;
}

SystemPreferencesWatcher::SystemPreferencesWatcher(core::Window& window)
    : m_window(window), m_preferences(query_system_preferences()), m_locale(query_locale()) {
    m_hook = m_window.add_message_hook([this](UINT msg, WPARAM, LPARAM) -> std::optional<LRESULT> {
        switch (msg) {
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            refresh();
            break;
        default:
            break;
        }
        return std::nullopt;
    });
}

SystemPreferencesWatcher::~SystemPreferencesWatcher() {
    m_window.remove_message_hook(m_hook);
}

void SystemPreferencesWatcher::refresh() {
    SystemPreferences prefs = query_system_preferences();
    if (prefs != m_preferences) {
        m_preferences = std::move(prefs);
        if (m_on_preferences) {
            m_on_preferences(m_preferences);
        }
    }
    LocaleInfo locale = query_locale();
    const bool locale_changed = locale.user_locale != m_locale.user_locale ||
                                locale.preferred_languages != m_locale.preferred_languages ||
                                locale.region != m_locale.region || locale.decimal_separator != m_locale.decimal_separator ||
                                locale.time_24h != m_locale.time_24h ||
                                locale.first_day_of_week != m_locale.first_day_of_week;
    if (locale_changed) {
        m_locale = std::move(locale);
        if (m_on_locale) {
            m_on_locale(m_locale);
        }
    }
}

} // namespace boson::native
