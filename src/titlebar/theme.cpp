#include <boson/titlebar/theme.hpp>

#include <dwmapi.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>

namespace boson::titlebar {

namespace {

#ifndef WM_DWMCOLORIZATIONCOLORCHANGED
constexpr UINT kWmDwmColorizationColorChanged = 0x0320;
#else
constexpr UINT kWmDwmColorizationColorChanged = WM_DWMCOLORIZATIONCOLORCHANGED;
#endif

constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kDwmKey[] = L"Software\\Microsoft\\Windows\\DWM";
constexpr wchar_t kAccentKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent";

std::optional<DWORD> read_dword(const wchar_t* key, const wchar_t* value) noexcept {
    DWORD data = 0;
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return data;
}

int hex_digit(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::optional<double> parse_number(std::string_view s) noexcept {
    s = trim(s);
    bool percent = false;
    if (!s.empty() && s.back() == '%') {
        percent = true;
        s.remove_suffix(1);
    }
    double value = 0.0;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), value);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return percent ? value / 100.0 * 255.0 : value;
}

std::uint8_t clamp_byte(double v) noexcept {
    return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
}

Rgba mix(Rgba a, Rgba b, double t) noexcept {
    const auto lerp = [t](std::uint8_t x, std::uint8_t y) {
        return clamp_byte(static_cast<double>(x) + (static_cast<double>(y) - static_cast<double>(x)) * t);
    };
    return Rgba{lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), 255};
}

} // namespace

std::optional<Rgba> parse_css_color(std::string_view text) noexcept {
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    if (iequals(text, "transparent")) {
        return Rgba{0, 0, 0, 0};
    }
    if (text.front() == '#') {
        const std::string_view hex = text.substr(1);
        for (const char ch : hex) {
            if (hex_digit(ch) < 0) {
                return std::nullopt;
            }
        }
        const auto byte_at = [hex](std::size_t i) {
            return static_cast<std::uint8_t>(hex_digit(hex[i]) * 16 + hex_digit(hex[i + 1]));
        };
        const auto nibble_at = [hex](std::size_t i) {
            return static_cast<std::uint8_t>(hex_digit(hex[i]) * 17);
        };
        switch (hex.size()) {
        case 3: return Rgba{nibble_at(0), nibble_at(1), nibble_at(2), 255};
        case 4: return Rgba{nibble_at(0), nibble_at(1), nibble_at(2), nibble_at(3)};
        case 6: return Rgba{byte_at(0), byte_at(2), byte_at(4), 255};
        case 8: return Rgba{byte_at(0), byte_at(2), byte_at(4), byte_at(6)};
        default: return std::nullopt;
        }
    }

    const auto open = text.find('(');
    if (open == std::string_view::npos || text.back() != ')') {
        return std::nullopt;
    }
    const std::string_view fn = trim(text.substr(0, open));
    if (!iequals(fn, "rgb") && !iequals(fn, "rgba")) {
        return std::nullopt;
    }
    std::string_view args = text.substr(open + 1, text.size() - open - 2);
    std::array<double, 4> values{0.0, 0.0, 0.0, 1.0};
    std::size_t count = 0;
    // Accepts both "r, g, b, a" and the CSS4 "r g b / a" forms.
    while (!args.empty() && count < 4) {
        args = trim(args);
        const auto sep = args.find_first_of(", /");
        const std::string_view part = sep == std::string_view::npos ? args : args.substr(0, sep);
        if (!part.empty()) {
            const auto value = parse_number(part);
            if (!value.has_value()) {
                return std::nullopt;
            }
            values[count++] = *value;
        }
        if (sep == std::string_view::npos) {
            break;
        }
        args.remove_prefix(sep + 1);
    }
    if (count < 3) {
        return std::nullopt;
    }
    double alpha = values[3];
    if (count == 4 && alpha > 1.0) {
        alpha /= 255.0; // percentages were scaled to 0..255 above
    }
    return Rgba{clamp_byte(values[0]), clamp_byte(values[1]), clamp_byte(values[2]), clamp_byte(alpha * 255.0)};
}

std::string to_css_hex(Rgba color) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "#";
    const auto put = [&out](std::uint8_t v) {
        out.push_back(kDigits[v >> 4]);
        out.push_back(kDigits[v & 0x0F]);
    };
    put(color.r);
    put(color.g);
    put(color.b);
    if (color.a != 255) {
        put(color.a);
    }
    return out;
}

ThemeInfo query_theme() {
    ThemeInfo info;
    info.dark_mode = read_dword(kPersonalizeKey, L"AppsUseLightTheme").value_or(1) == 0;
    info.system_dark_mode = read_dword(kPersonalizeKey, L"SystemUsesLightTheme").value_or(1) == 0;
    info.transparency_effects = read_dword(kPersonalizeKey, L"EnableTransparency").value_or(1) != 0;
    info.accent_on_title_bars = read_dword(kDwmKey, L"ColorPrevalence").value_or(0) != 0;

    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(HIGHCONTRASTW);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(HIGHCONTRASTW), &hc, 0) != FALSE) {
        info.high_contrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
        if (info.high_contrast && hc.lpszDefaultScheme != nullptr) {
            info.high_contrast_scheme = hc.lpszDefaultScheme;
        }
    }

    // Accent: DWM\AccentColor is 0xAABBGGRR; fall back to the DWM colorization colour (0xAARRGGBB).
    bool have_accent = false;
    if (const auto accent = read_dword(kDwmKey, L"AccentColor"); accent.has_value()) {
        info.accent = Rgba{
            static_cast<std::uint8_t>(*accent & 0xFF),
            static_cast<std::uint8_t>((*accent >> 8) & 0xFF),
            static_cast<std::uint8_t>((*accent >> 16) & 0xFF),
            255,
        };
        have_accent = true;
    }

    // Explorer\Accent\AccentPalette: 8 RGBA entries, Light3, Light2, Light1, Accent, Dark1, Dark2, Dark3, unused.
    std::array<std::uint8_t, 32> palette{};
    DWORD size = static_cast<DWORD>(palette.size());
    const bool have_palette =
        RegGetValueW(HKEY_CURRENT_USER, kAccentKey, L"AccentPalette", RRF_RT_REG_BINARY, nullptr, palette.data(), &size) ==
            ERROR_SUCCESS &&
        size >= 28;
    const auto entry = [&palette](std::size_t i) {
        return Rgba{palette[i * 4], palette[i * 4 + 1], palette[i * 4 + 2], 255};
    };
    if (!have_accent) {
        if (have_palette) {
            info.accent = entry(3);
        } else {
            DWORD colorization = 0;
            BOOL opaque = FALSE;
            if (SUCCEEDED(DwmGetColorizationColor(&colorization, &opaque))) {
                info.accent = Rgba{
                    static_cast<std::uint8_t>((colorization >> 16) & 0xFF),
                    static_cast<std::uint8_t>((colorization >> 8) & 0xFF),
                    static_cast<std::uint8_t>(colorization & 0xFF),
                    255,
                };
            }
        }
    }
    if (have_palette) {
        info.accent_light = {entry(2), entry(1), entry(0)};
        info.accent_dark = {entry(4), entry(5), entry(6)};
    } else {
        const Rgba white{255, 255, 255, 255};
        const Rgba black{0, 0, 0, 255};
        info.accent_light = {mix(info.accent, white, 0.25), mix(info.accent, white, 0.5), mix(info.accent, white, 0.75)};
        info.accent_dark = {mix(info.accent, black, 0.25), mix(info.accent, black, 0.5), mix(info.accent, black, 0.75)};
    }

    info.colors = SystemColors{
        .window = rgba_from_colorref(GetSysColor(COLOR_WINDOW)),
        .window_text = rgba_from_colorref(GetSysColor(COLOR_WINDOWTEXT)),
        .highlight = rgba_from_colorref(GetSysColor(COLOR_HIGHLIGHT)),
        .highlight_text = rgba_from_colorref(GetSysColor(COLOR_HIGHLIGHTTEXT)),
        .button_face = rgba_from_colorref(GetSysColor(COLOR_BTNFACE)),
        .button_text = rgba_from_colorref(GetSysColor(COLOR_BTNTEXT)),
        .gray_text = rgba_from_colorref(GetSysColor(COLOR_GRAYTEXT)),
        .hotlight = rgba_from_colorref(GetSysColor(COLOR_HOTLIGHT)),
    };
    return info;
}

// ---- ThemeWatcher ----------------------------------------------------------------------------------------

ThemeWatcher::ThemeWatcher(core::Window& window, QueryFn query)
    : m_window(window)
    , m_query(query ? std::move(query) : QueryFn{&query_theme}) {
    m_current = m_query();
    m_hook = m_window.add_message_hook([this](UINT msg, WPARAM w_param, LPARAM l_param) {
        return on_message(msg, w_param, l_param);
    });
}

ThemeWatcher::~ThemeWatcher() {
    m_debounce.stop();
    if (m_hook != 0) {
        m_window.remove_message_hook(m_hook);
    }
}

ThemeWatcher::ListenerId ThemeWatcher::on_changed(Listener listener) {
    const ListenerId id = m_next_id++;
    m_listeners.push_back(Entry{.id = id, .fn = std::move(listener)});
    return id;
}

bool ThemeWatcher::off(ListenerId id) noexcept {
    const auto it = std::find_if(m_listeners.begin(), m_listeners.end(), [id](const Entry& e) { return e.id == id; });
    if (it == m_listeners.end()) {
        return false;
    }
    m_listeners.erase(it);
    return true;
}

void ThemeWatcher::set_query(QueryFn query) {
    m_query = query ? std::move(query) : QueryFn{&query_theme};
}

void ThemeWatcher::refresh(bool force) {
    ThemeInfo next = m_query();
    if (!force && next == m_current) {
        return;
    }
    m_current = std::move(next);
    // Copy: a listener may add or remove listeners.
    const std::vector<Entry> listeners = m_listeners;
    for (const auto& entry : listeners) {
        if (entry.fn) {
            entry.fn(m_current);
        }
    }
}

std::optional<LRESULT> ThemeWatcher::on_message(UINT msg, WPARAM /*w_param*/, LPARAM /*l_param*/) {
    switch (msg) {
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
    case kWmDwmColorizationColorChanged:
        // Settings changes arrive in bursts (one WM_SETTINGCHANGE per changed value); coalesce them.
        m_debounce.start(std::chrono::milliseconds{40}, [this]() { refresh(); });
        break;
    default:
        break;
    }
    return std::nullopt;
}

} // namespace boson::titlebar
