#include <boson/desktop/shortcuts.hpp>

#include <boson/core/result.hpp>

#include "desktop_util.hpp"

#include <array>
#include <charconv>

namespace boson::desktop {

namespace {

struct NamedKey {
    std::string_view name; // lower-case lookup key
    UINT vk;
};

// Canonical names come from virtual_key_name(); this table maps every accepted spelling to a VK.
constexpr std::array<NamedKey, 66> kNamedKeys{{
    {"space", VK_SPACE},
    {"tab", VK_TAB},
    {"capslock", VK_CAPITAL},
    {"numlock", VK_NUMLOCK},
    {"scrolllock", VK_SCROLL},
    {"backspace", VK_BACK},
    {"delete", VK_DELETE},
    {"del", VK_DELETE},
    {"insert", VK_INSERT},
    {"ins", VK_INSERT},
    {"return", VK_RETURN},
    {"enter", VK_RETURN},
    {"up", VK_UP},
    {"down", VK_DOWN},
    {"left", VK_LEFT},
    {"right", VK_RIGHT},
    {"arrowup", VK_UP},
    {"arrowdown", VK_DOWN},
    {"arrowleft", VK_LEFT},
    {"arrowright", VK_RIGHT},
    {"home", VK_HOME},
    {"end", VK_END},
    {"pageup", VK_PRIOR},
    {"pagedown", VK_NEXT},
    {"escape", VK_ESCAPE},
    {"esc", VK_ESCAPE},
    {"volumeup", VK_VOLUME_UP},
    {"volumedown", VK_VOLUME_DOWN},
    {"volumemute", VK_VOLUME_MUTE},
    {"medianexttrack", VK_MEDIA_NEXT_TRACK},
    {"mediaprevioustrack", VK_MEDIA_PREV_TRACK},
    {"mediastop", VK_MEDIA_STOP},
    {"mediaplaypause", VK_MEDIA_PLAY_PAUSE},
    {"printscreen", VK_SNAPSHOT},
    {"pause", VK_PAUSE},
    {"num0", VK_NUMPAD0},
    {"num1", VK_NUMPAD1},
    {"num2", VK_NUMPAD2},
    {"num3", VK_NUMPAD3},
    {"num4", VK_NUMPAD4},
    {"num5", VK_NUMPAD5},
    {"num6", VK_NUMPAD6},
    {"num7", VK_NUMPAD7},
    {"num8", VK_NUMPAD8},
    {"num9", VK_NUMPAD9},
    {"numdec", VK_DECIMAL},
    {"numadd", VK_ADD},
    {"numsub", VK_SUBTRACT},
    {"nummult", VK_MULTIPLY},
    {"numdiv", VK_DIVIDE},
    {"browserback", VK_BROWSER_BACK},
    {"browserforward", VK_BROWSER_FORWARD},
    {"browserrefresh", VK_BROWSER_REFRESH},
    {"browserhome", VK_BROWSER_HOME},
    {"browsersearch", VK_BROWSER_SEARCH},
    {"launchmail", VK_LAUNCH_MAIL},
    {"apps", VK_APPS},
    {"contextmenu", VK_APPS},
    {"clear", VK_CLEAR},
    {"help", VK_HELP},
    {"sleep", VK_SLEEP},
    {"select", VK_SELECT},
    {"execute", VK_EXECUTE},
    {"zoom", VK_ZOOM},
    {"play", VK_PLAY},
    {"attn", VK_ATTN},
}};

struct CanonicalName {
    UINT vk;
    std::string_view name;
};

constexpr std::array<CanonicalName, 47> kCanonicalNames{{
    {VK_SPACE, "Space"}, {VK_TAB, "Tab"}, {VK_CAPITAL, "Capslock"}, {VK_NUMLOCK, "Numlock"},
    {VK_SCROLL, "Scrolllock"}, {VK_BACK, "Backspace"}, {VK_DELETE, "Delete"}, {VK_INSERT, "Insert"},
    {VK_RETURN, "Enter"}, {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"}, {VK_ESCAPE, "Escape"},
    {VK_VOLUME_UP, "VolumeUp"}, {VK_VOLUME_DOWN, "VolumeDown"}, {VK_VOLUME_MUTE, "VolumeMute"},
    {VK_MEDIA_NEXT_TRACK, "MediaNextTrack"}, {VK_MEDIA_PREV_TRACK, "MediaPreviousTrack"},
    {VK_MEDIA_STOP, "MediaStop"}, {VK_MEDIA_PLAY_PAUSE, "MediaPlayPause"}, {VK_SNAPSHOT, "PrintScreen"},
    {VK_PAUSE, "Pause"}, {VK_DECIMAL, "NumDec"}, {VK_ADD, "NumAdd"}, {VK_SUBTRACT, "NumSub"},
    {VK_MULTIPLY, "NumMult"}, {VK_DIVIDE, "NumDiv"}, {VK_BROWSER_BACK, "BrowserBack"},
    {VK_BROWSER_FORWARD, "BrowserForward"}, {VK_BROWSER_REFRESH, "BrowserRefresh"},
    {VK_BROWSER_HOME, "BrowserHome"}, {VK_BROWSER_SEARCH, "BrowserSearch"}, {VK_LAUNCH_MAIL, "LaunchMail"},
    {VK_APPS, "Apps"}, {VK_CLEAR, "Clear"}, {VK_HELP, "Help"}, {VK_SLEEP, "Sleep"}, {VK_SELECT, "Select"},
    {VK_EXECUTE, "Execute"}, {VK_ZOOM, "Zoom"}, {VK_PLAY, "Play"}, {VK_ATTN, "Attn"},
}};

std::optional<UINT> parse_modifier(std::string_view token) {
    const std::string t = detail::ascii_lower(token);
    if (t == "ctrl" || t == "control" || t == "command" || t == "cmd" || t == "commandorcontrol" || t == "cmdorctrl" ||
        t == "commandorctrl" || t == "cmdorcontrol") {
        return MOD_CONTROL;
    }
    if (t == "alt" || t == "option") return MOD_ALT;
    if (t == "altgr") return MOD_CONTROL | MOD_ALT;
    if (t == "shift") return MOD_SHIFT;
    if (t == "super" || t == "meta" || t == "win" || t == "windows") return MOD_WIN;
    return std::nullopt;
}

struct ParsedKey {
    UINT vk = 0;
    UINT extra_modifiers = 0;
};

std::optional<ParsedKey> character_key(wchar_t ch) {
    const SHORT scan = VkKeyScanW(ch);
    if (scan == -1) {
        return std::nullopt;
    }
    const UINT vk = static_cast<UINT>(LOBYTE(scan));
    const UINT state = static_cast<UINT>(HIBYTE(scan));
    UINT mods = 0;
    if ((state & 1) != 0) mods |= MOD_SHIFT;
    if ((state & 2) != 0) mods |= MOD_CONTROL;
    if ((state & 4) != 0) mods |= MOD_ALT;
    return ParsedKey{vk, mods};
}

std::optional<ParsedKey> parse_key(std::string_view token) {
    if (token.size() == 1) {
        const char ch = token.front();
        if (ch >= 'a' && ch <= 'z') return ParsedKey{static_cast<UINT>(ch - 'a' + 'A'), 0};
        if (ch >= 'A' && ch <= 'Z') return ParsedKey{static_cast<UINT>(ch), 0};
        if (ch >= '0' && ch <= '9') return ParsedKey{static_cast<UINT>(ch), 0};
        if (static_cast<unsigned char>(ch) >= 0x21 && static_cast<unsigned char>(ch) < 0x7F) {
            return character_key(static_cast<wchar_t>(ch));
        }
        return std::nullopt;
    }
    const std::string t = detail::ascii_lower(token);
    if (t == "plus") {
        return character_key(L'+');
    }
    if (t.size() >= 2 && t.size() <= 3 && t[0] == 'f') {
        unsigned n = 0;
        const auto [ptr, ec] = std::from_chars(t.data() + 1, t.data() + t.size(), n);
        if (ec == std::errc{} && ptr == t.data() + t.size() && n >= 1 && n <= 24) {
            return ParsedKey{VK_F1 + n - 1, 0};
        }
        return std::nullopt;
    }
    for (const auto& entry : kNamedKeys) {
        if (entry.name == t) {
            return ParsedKey{entry.vk, 0};
        }
    }
    // Non-ASCII single character (UTF-8), e.g. a layout-specific key.
    const std::wstring wide = core::utf8_to_wide(token);
    if (wide.size() == 1) {
        return character_key(wide.front());
    }
    return std::nullopt;
}

} // namespace

std::string virtual_key_name(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "F" + std::to_string(vk - VK_F1 + 1);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return "Num" + std::to_string(vk - VK_NUMPAD0);
    }
    for (const auto& entry : kCanonicalNames) {
        if (entry.vk == vk) {
            return std::string(entry.name);
        }
    }
    // Layout-dependent OEM keys: the unshifted character this layout produces.
    const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu;
    if (ch >= 0x21) {
        const wchar_t wide[2] = {static_cast<wchar_t>(ch), L'\0'};
        return core::wide_to_utf8(std::wstring_view{wide, 1});
    }
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "VK_%02X", vk);
    return buffer;
}

std::string format_accelerator(UINT modifiers, UINT key) {
    std::string out;
    if ((modifiers & MOD_CONTROL) != 0) out += "Ctrl+";
    if ((modifiers & MOD_ALT) != 0) out += "Alt+";
    if ((modifiers & MOD_SHIFT) != 0) out += "Shift+";
    if ((modifiers & MOD_WIN) != 0) out += "Super+";
    out += virtual_key_name(key);
    return out;
}

std::expected<Accelerator, std::string> parse_accelerator(std::string_view text) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    if (text.empty()) {
        return std::unexpected("accelerator is empty");
    }

    std::vector<std::string_view> tokens;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t plus = text.find('+', start);
        if (plus == start && plus + 1 == text.size()) {
            tokens.push_back(text.substr(start, 1)); // trailing "+" key, as in "Ctrl++"
            break;
        }
        if (plus == std::string_view::npos) {
            tokens.push_back(text.substr(start));
            break;
        }
        tokens.push_back(text.substr(start, plus - start));
        start = plus + 1;
    }

    UINT modifiers = 0;
    std::optional<ParsedKey> key;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        std::string_view token = tokens[i];
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
        if (token.empty()) {
            return std::unexpected("accelerator '" + std::string(text) + "' has an empty part");
        }
        if (const auto mod = parse_modifier(token)) {
            if (i + 1 == tokens.size()) {
                return std::unexpected("accelerator '" + std::string(text) + "' has no key, only modifiers");
            }
            modifiers |= *mod;
            continue;
        }
        if (key) {
            return std::unexpected("accelerator '" + std::string(text) + "' has more than one key");
        }
        if (i + 1 != tokens.size()) {
            return std::unexpected("'" + std::string(token) + "' is not a modifier (the key must come last)");
        }
        key = parse_key(token);
        if (!key) {
            return std::unexpected("unknown key '" + std::string(token) + "'");
        }
    }
    if (!key) {
        return std::unexpected("accelerator '" + std::string(text) + "' has no key");
    }
    Accelerator out;
    out.modifiers = modifiers | key->extra_modifiers;
    out.key = key->vk;
    out.canonical = format_accelerator(out.modifiers, out.key);
    return out;
}

// ---- GlobalShortcuts -------------------------------------------------------------------------------------

GlobalShortcuts::GlobalShortcuts() {
    m_window = std::make_unique<detail::HiddenWindow>(
        [this](UINT msg, WPARAM w_param, LPARAM) -> std::optional<LRESULT> {
            if (msg == WM_HOTKEY) {
                on_hotkey(static_cast<Id>(w_param));
                return 0;
            }
            return std::nullopt;
        },
        true);
}

GlobalShortcuts::~GlobalShortcuts() {
    remove_all();
}

HWND GlobalShortcuts::hwnd() const noexcept {
    return m_window ? m_window->hwnd() : nullptr;
}

std::expected<GlobalShortcuts::Id, ShortcutError> GlobalShortcuts::add(std::string_view accelerator, Callback callback) {
    auto parsed = parse_accelerator(accelerator);
    if (!parsed) {
        return std::unexpected(ShortcutError{ShortcutErrorKind::InvalidAccelerator, parsed.error()});
    }
    for (const auto& entry : m_entries) {
        if (entry.accelerator == *parsed) {
            return std::unexpected(ShortcutError{
                ShortcutErrorKind::AlreadyRegistered,
                "'" + parsed->canonical + "' is already registered by this application (id " + std::to_string(entry.id) + ")"});
        }
    }
    if (hwnd() == nullptr) {
        return std::unexpected(ShortcutError{ShortcutErrorKind::SystemError, "hotkey window could not be created"});
    }
    if (m_next_id > 0xBFFF) {
        return std::unexpected(ShortcutError{ShortcutErrorKind::SystemError, "hotkey id space exhausted"});
    }
    const Id id = m_next_id;
    if (!RegisterHotKey(hwnd(), static_cast<int>(id), parsed->modifiers | MOD_NOREPEAT, parsed->key)) {
        const DWORD error = GetLastError();
        if (error == ERROR_HOTKEY_ALREADY_REGISTERED) {
            return std::unexpected(ShortcutError{
                ShortcutErrorKind::Conflict,
                "'" + parsed->canonical + "' is already in use by another application or by Windows"});
        }
        return std::unexpected(ShortcutError{ShortcutErrorKind::SystemError, "RegisterHotKey: " + detail::win32_message(error)});
    }
    ++m_next_id;
    m_entries.push_back(Entry{.id = id, .accelerator = std::move(*parsed), .callback = std::move(callback)});
    return id;
}

bool GlobalShortcuts::remove(Id id) noexcept {
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->id == id) {
            UnregisterHotKey(hwnd(), static_cast<int>(id));
            m_entries.erase(it);
            return true;
        }
    }
    return false;
}

bool GlobalShortcuts::remove(std::string_view accelerator) noexcept {
    try {
        const auto id = find(accelerator);
        return id ? remove(*id) : false;
    } catch (...) {
        return false;
    }
}

void GlobalShortcuts::remove_all() noexcept {
    for (const auto& entry : m_entries) {
        UnregisterHotKey(hwnd(), static_cast<int>(entry.id));
    }
    m_entries.clear();
}

std::optional<GlobalShortcuts::Id> GlobalShortcuts::find(std::string_view accelerator) const {
    const auto parsed = parse_accelerator(accelerator);
    if (!parsed) {
        return std::nullopt;
    }
    for (const auto& entry : m_entries) {
        if (entry.accelerator == *parsed) {
            return entry.id;
        }
    }
    return std::nullopt;
}

bool GlobalShortcuts::is_registered(std::string_view accelerator) const {
    return find(accelerator).has_value();
}

std::vector<ShortcutInfo> GlobalShortcuts::list() const {
    std::vector<ShortcutInfo> out;
    out.reserve(m_entries.size());
    for (const auto& entry : m_entries) {
        out.push_back(ShortcutInfo{.id = entry.id, .accelerator = entry.accelerator.canonical});
    }
    return out;
}

void GlobalShortcuts::on_hotkey(Id id) {
    for (const auto& entry : m_entries) {
        if (entry.id == id) {
            // Copy first: the callback may remove this (or every) shortcut.
            const ShortcutEvent event{.id = id, .accelerator = entry.accelerator.canonical};
            const Callback callback = entry.callback;
            if (callback) {
                callback(event);
            }
            return;
        }
    }
}

} // namespace boson::desktop
