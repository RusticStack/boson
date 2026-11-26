#include <boson/menu/accelerator.hpp>

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>

namespace boson::menu {

namespace {

struct KeyAlias {
    std::string_view name; ///< First alias of a key is its canonical display name.
    UINT vk;
};

constexpr std::array kKeyAliases{
    KeyAlias{"Space", VK_SPACE},
    KeyAlias{"Enter", VK_RETURN},
    KeyAlias{"Return", VK_RETURN},
    KeyAlias{"Tab", VK_TAB},
    KeyAlias{"Esc", VK_ESCAPE},
    KeyAlias{"Escape", VK_ESCAPE},
    KeyAlias{"Backspace", VK_BACK},
    KeyAlias{"Back", VK_BACK},
    KeyAlias{"Delete", VK_DELETE},
    KeyAlias{"Del", VK_DELETE},
    KeyAlias{"Insert", VK_INSERT},
    KeyAlias{"Ins", VK_INSERT},
    KeyAlias{"Home", VK_HOME},
    KeyAlias{"End", VK_END},
    KeyAlias{"PageUp", VK_PRIOR},
    KeyAlias{"PgUp", VK_PRIOR},
    KeyAlias{"PageDown", VK_NEXT},
    KeyAlias{"PgDn", VK_NEXT},
    KeyAlias{"Up", VK_UP},
    KeyAlias{"ArrowUp", VK_UP},
    KeyAlias{"Down", VK_DOWN},
    KeyAlias{"ArrowDown", VK_DOWN},
    KeyAlias{"Left", VK_LEFT},
    KeyAlias{"ArrowLeft", VK_LEFT},
    KeyAlias{"Right", VK_RIGHT},
    KeyAlias{"ArrowRight", VK_RIGHT},
    KeyAlias{"Plus", VK_OEM_PLUS},
    KeyAlias{"+", VK_OEM_PLUS},
    KeyAlias{"=", VK_OEM_PLUS},
    KeyAlias{"Equal", VK_OEM_PLUS},
    KeyAlias{"Minus", VK_OEM_MINUS},
    KeyAlias{"-", VK_OEM_MINUS},
    KeyAlias{",", VK_OEM_COMMA},
    KeyAlias{"Comma", VK_OEM_COMMA},
    KeyAlias{".", VK_OEM_PERIOD},
    KeyAlias{"Period", VK_OEM_PERIOD},
    KeyAlias{"/", VK_OEM_2},
    KeyAlias{"Slash", VK_OEM_2},
    KeyAlias{";", VK_OEM_1},
    KeyAlias{"Semicolon", VK_OEM_1},
    KeyAlias{"'", VK_OEM_7},
    KeyAlias{"Quote", VK_OEM_7},
    KeyAlias{"[", VK_OEM_4},
    KeyAlias{"BracketLeft", VK_OEM_4},
    KeyAlias{"]", VK_OEM_6},
    KeyAlias{"BracketRight", VK_OEM_6},
    KeyAlias{"\\", VK_OEM_5},
    KeyAlias{"Backslash", VK_OEM_5},
    KeyAlias{"`", VK_OEM_3},
    KeyAlias{"Backquote", VK_OEM_3},
    KeyAlias{"Num0", VK_NUMPAD0},
    KeyAlias{"Num1", VK_NUMPAD1},
    KeyAlias{"Num2", VK_NUMPAD2},
    KeyAlias{"Num3", VK_NUMPAD3},
    KeyAlias{"Num4", VK_NUMPAD4},
    KeyAlias{"Num5", VK_NUMPAD5},
    KeyAlias{"Num6", VK_NUMPAD6},
    KeyAlias{"Num7", VK_NUMPAD7},
    KeyAlias{"Num8", VK_NUMPAD8},
    KeyAlias{"Num9", VK_NUMPAD9},
    KeyAlias{"NumAdd", VK_ADD},
    KeyAlias{"NumSub", VK_SUBTRACT},
    KeyAlias{"NumMult", VK_MULTIPLY},
    KeyAlias{"NumDiv", VK_DIVIDE},
    KeyAlias{"NumDec", VK_DECIMAL},
    KeyAlias{"PrintScreen", VK_SNAPSHOT},
    KeyAlias{"PrtSc", VK_SNAPSHOT},
    KeyAlias{"Pause", VK_PAUSE},
    KeyAlias{"Apps", VK_APPS},
    KeyAlias{"ContextMenu", VK_APPS},
    KeyAlias{"VolumeUp", VK_VOLUME_UP},
    KeyAlias{"VolumeDown", VK_VOLUME_DOWN},
    KeyAlias{"VolumeMute", VK_VOLUME_MUTE},
    KeyAlias{"MediaPlayPause", VK_MEDIA_PLAY_PAUSE},
    KeyAlias{"MediaStop", VK_MEDIA_STOP},
    KeyAlias{"MediaNextTrack", VK_MEDIA_NEXT_TRACK},
    KeyAlias{"MediaPreviousTrack", VK_MEDIA_PREV_TRACK},
};

constexpr std::array<std::string_view, 24> kFunctionKeyNames{
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
};

constexpr std::string_view kLetterAndDigitNames = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
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

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] std::optional<KeyModifiers> parse_modifier(std::string_view token) noexcept {
    if (iequals(token, "ctrl") || iequals(token, "control") || iequals(token, "cmdorctrl") ||
        iequals(token, "commandorcontrol") || iequals(token, "cmd") || iequals(token, "command")) {
        return KeyModifiers::Ctrl;
    }
    if (iequals(token, "alt") || iequals(token, "option") || iequals(token, "altgr")) {
        return KeyModifiers::Alt;
    }
    if (iequals(token, "shift")) {
        return KeyModifiers::Shift;
    }
    if (iequals(token, "win") || iequals(token, "windows") || iequals(token, "super") || iequals(token, "meta")) {
        return KeyModifiers::Win;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<UINT> parse_key(std::string_view token) noexcept {
    if (token.size() == 1) {
        const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(token.front())));
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            return static_cast<UINT>(ch);
        }
    }
    for (std::size_t i = 0; i < kFunctionKeyNames.size(); ++i) {
        if (iequals(token, kFunctionKeyNames[i])) {
            return static_cast<UINT>(VK_F1 + i);
        }
    }
    for (const KeyAlias& alias : kKeyAliases) {
        if (iequals(token, alias.name)) {
            return alias.vk;
        }
    }
    return std::nullopt;
}

[[nodiscard]] UINT invoke_message() noexcept {
    static const UINT message = RegisterWindowMessageW(L"Boson.AcceleratorTable.Invoke");
    return message;
}

[[nodiscard]] bool in_menu_mode() noexcept {
    GUITHREADINFO info{};
    info.cbSize = sizeof(GUITHREADINFO);
    return GetGUIThreadInfo(GetCurrentThreadId(), &info) != FALSE && (info.flags & GUI_INMENUMODE) != 0;
}

std::atomic<std::uint64_t> g_next_table_serial{1};

struct ThreadHookState {
    HHOOK hook = nullptr;
    std::vector<AcceleratorTable*> tables;
};

thread_local ThreadHookState t_hook_state;

} // namespace

std::string_view key_name(UINT vk) noexcept {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        const std::size_t index = vk >= 'A' ? vk - 'A' : 26 + (vk - '0');
        return kLetterAndDigitNames.substr(index, 1);
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return kFunctionKeyNames[vk - VK_F1];
    }
    for (const KeyAlias& alias : kKeyAliases) {
        if (alias.vk == vk) {
            return alias.name;
        }
    }
    return {};
}

std::optional<Accelerator> Accelerator::parse(std::string_view text) {
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }

    std::vector<std::string_view> tokens;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t plus = text.find('+', pos);
        if (plus == pos) {
            // A '+' where a token should start is the Plus key itself ("Ctrl++", "+").
            tokens.push_back(text.substr(pos, 1));
            pos = plus + 1;
            if (pos < text.size() && text[pos] == '+') {
                ++pos;
            }
            continue;
        }
        const std::size_t end = plus == std::string_view::npos ? text.size() : plus;
        tokens.push_back(trim(text.substr(pos, end - pos)));
        pos = plus == std::string_view::npos ? text.size() : plus + 1;
    }
    if (tokens.empty()) {
        return std::nullopt;
    }

    Accelerator accel;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        const auto mod = parse_modifier(tokens[i]);
        if (!mod.has_value()) {
            return std::nullopt;
        }
        accel.modifiers = accel.modifiers | *mod;
    }
    const auto key = parse_key(tokens.back());
    if (!key.has_value()) {
        return std::nullopt;
    }
    accel.key = *key;
    return accel;
}

std::string Accelerator::to_string() const {
    std::string out;
    if (has_modifier(modifiers, KeyModifiers::Ctrl)) {
        out += "Ctrl+";
    }
    if (has_modifier(modifiers, KeyModifiers::Alt)) {
        out += "Alt+";
    }
    if (has_modifier(modifiers, KeyModifiers::Shift)) {
        out += "Shift+";
    }
    if (has_modifier(modifiers, KeyModifiers::Win)) {
        out += "Win+";
    }
    out += key_name(key);
    return out;
}

KeyModifiers current_modifiers() noexcept {
    const auto down = [](int vk) { return (GetKeyState(vk) & 0x8000) != 0; };
    KeyModifiers mods = KeyModifiers::None;
    if (down(VK_CONTROL)) {
        mods = mods | KeyModifiers::Ctrl;
    }
    if (down(VK_MENU)) {
        mods = mods | KeyModifiers::Alt;
    }
    if (down(VK_SHIFT)) {
        mods = mods | KeyModifiers::Shift;
    }
    if (down(VK_LWIN) || down(VK_RWIN)) {
        mods = mods | KeyModifiers::Win;
    }
    return mods;
}

bool is_modifier_key(UINT vk) noexcept {
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

AcceleratorTable::AcceleratorTable(core::Window& window) {
    attach(window);
}

AcceleratorTable::~AcceleratorTable() {
    detach_all_webviews();
    detach_window();
}

AcceleratorTable::Id AcceleratorTable::add(Accelerator accelerator, Callback callback) {
    const Id id = m_next_id++;
    m_entries.push_back(Entry{.id = id, .accelerator = accelerator, .callback = std::move(callback)});
    return id;
}

std::optional<AcceleratorTable::Id> AcceleratorTable::add(std::string_view accelerator, Callback callback) {
    const auto parsed = Accelerator::parse(accelerator);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return add(*parsed, std::move(callback));
}

bool AcceleratorTable::remove(Id id) noexcept {
    const auto it = std::ranges::find(m_entries, id, &Entry::id);
    if (it == m_entries.end()) {
        return false;
    }
    m_entries.erase(it);
    return true;
}

void AcceleratorTable::clear() noexcept {
    m_entries.clear();
}

bool AcceleratorTable::contains(const Accelerator& accelerator) const noexcept {
    return std::ranges::any_of(m_entries, [&](const Entry& e) { return e.accelerator == accelerator; });
}

bool AcceleratorTable::attach(core::Window& window) {
    detach_window();
    if (window.hwnd() == nullptr) {
        return false;
    }
    m_window = &window;
    m_hwnd = window.hwnd();
    const std::uint64_t serial = g_next_table_serial.fetch_add(1, std::memory_order_relaxed);
    m_window_hook = window.add_message_hook([this, serial](UINT msg, WPARAM w_param, LPARAM l_param) -> std::optional<LRESULT> {
        if (msg == invoke_message() && static_cast<std::uint64_t>(l_param) == serial) {
            run_entry(static_cast<Id>(w_param));
            return 0;
        }
        return std::nullopt;
    });
    m_serial = serial;
    register_thread_hook();
    return true;
}

void AcceleratorTable::detach_window() noexcept {
    unregister_thread_hook();
    if (m_window != nullptr && m_window_hook != 0) {
        m_window->remove_message_hook(m_window_hook);
    }
    m_window = nullptr;
    m_hwnd = nullptr;
    m_window_hook = 0;
    m_serial = 0;
}

void AcceleratorTable::register_thread_hook() {
    if (m_thread_hooked) {
        return;
    }
    ThreadHookState& state = t_hook_state;
    if (state.hook == nullptr) {
        state.hook = SetWindowsHookExW(WH_GETMESSAGE, &AcceleratorTable::get_message_hook, nullptr, GetCurrentThreadId());
        if (state.hook == nullptr) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "SetWindowsHookExW(WH_GETMESSAGE)");
            return;
        }
    }
    state.tables.push_back(this);
    m_thread_hooked = true;
}

void AcceleratorTable::unregister_thread_hook() noexcept {
    if (!m_thread_hooked) {
        return;
    }
    ThreadHookState& state = t_hook_state;
    std::erase(state.tables, this);
    if (state.tables.empty() && state.hook != nullptr) {
        UnhookWindowsHookEx(state.hook);
        state.hook = nullptr;
    }
    m_thread_hooked = false;
}

LRESULT CALLBACK AcceleratorTable::get_message_hook(int code, WPARAM w_param, LPARAM l_param) {
    if (code == HC_ACTION && w_param == PM_REMOVE && l_param != 0) {
        auto* msg = reinterpret_cast<MSG*>(l_param);
        if ((msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) && !in_menu_mode()) {
            const std::vector<AcceleratorTable*> tables = t_hook_state.tables;
            for (AcceleratorTable* table : tables) {
                if (std::ranges::find(t_hook_state.tables, table) == t_hook_state.tables.end()) {
                    continue; // removed by an earlier callback
                }
                const HWND root = table->m_hwnd;
                if (root == nullptr || (msg->hwnd != root && IsChild(root, msg->hwnd) == FALSE)) {
                    continue;
                }
                if (table->translate(*msg)) {
                    msg->message = WM_NULL;
                    break;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, w_param, l_param);
}

bool AcceleratorTable::translate(const MSG& msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) {
        return false;
    }
    const auto vk = static_cast<UINT>(msg.wParam);
    if (is_modifier_key(vk)) {
        return false;
    }
    const bool repeat = (static_cast<std::uint64_t>(msg.lParam) & (1ULL << 30U)) != 0;
    return dispatch(Accelerator{.modifiers = current_modifiers(), .key = vk}, repeat);
}

bool AcceleratorTable::dispatch(const Accelerator& chord, bool is_repeat) {
    if (!m_enabled) {
        return false;
    }
    const auto it = std::ranges::find(m_entries, chord, &Entry::accelerator);
    if (it == m_entries.end()) {
        return false;
    }
    if (is_repeat && !m_allow_repeat) {
        return true; // swallow auto-repeat of a bound chord without firing again
    }
    if (m_hwnd != nullptr && IsWindow(m_hwnd) != FALSE) {
        PostMessageW(m_hwnd, invoke_message(), static_cast<WPARAM>(it->id), static_cast<LPARAM>(m_serial));
    } else {
        run_entry(it->id);
    }
    return true;
}

void AcceleratorTable::run_entry(Id id) {
    const auto it = std::ranges::find(m_entries, id, &Entry::id);
    if (it == m_entries.end() || !it->callback) {
        return;
    }
    const Callback callback = it->callback; // the callback may edit the table
    callback();
}

bool AcceleratorTable::handle_webview_key(COREWEBVIEW2_KEY_EVENT_KIND kind, UINT virtual_key, bool is_repeat) {
    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
        return false;
    }
    if (is_modifier_key(virtual_key)) {
        return false;
    }
    const Accelerator chord{.modifiers = current_modifiers(), .key = virtual_key};
    if (dispatch(chord, is_repeat)) {
        return true;
    }
    if (m_enabled && !is_repeat && m_unmatched) {
        return m_unmatched(chord, kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN);
    }
    return false;
}

bool AcceleratorTable::attach_webview(core::WebView& webview) {
    return attach_webview(webview.controller());
}

bool AcceleratorTable::attach_webview(ICoreWebView2Controller* controller) {
    if (controller == nullptr) {
        return false;
    }
    if (std::ranges::any_of(m_webviews, [controller](const WebViewHook& h) { return h.controller.Get() == controller; })) {
        return true;
    }
    WebViewHook hook;
    hook.controller = controller;
    const HRESULT hr = controller->add_AcceleratorKeyPressed(
        Microsoft::WRL::Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [this](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                if (args == nullptr) {
                    return S_OK;
                }
                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                UINT vk = 0;
                COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
                if (!BOSON_HR_CHECK(args->get_KeyEventKind(&kind)) || !BOSON_HR_CHECK(args->get_VirtualKey(&vk))) {
                    return S_OK;
                }
                const bool repeat = SUCCEEDED(args->get_PhysicalKeyStatus(&status)) && status.WasKeyDown != FALSE;
                if (handle_webview_key(kind, vk, repeat)) {
                    BOSON_HR_CHECK(args->put_Handled(TRUE));
                }
                return S_OK;
            })
            .Get(),
        &hook.token);
    if (!BOSON_HR_CHECK(hr)) {
        return false;
    }
    m_webviews.push_back(std::move(hook));
    return true;
}

void AcceleratorTable::detach_webview(core::WebView& webview) noexcept {
    detach_webview(webview.controller());
}

void AcceleratorTable::detach_webview(ICoreWebView2Controller* controller) noexcept {
    const auto it = std::ranges::find_if(m_webviews, [controller](const WebViewHook& h) { return h.controller.Get() == controller; });
    if (it == m_webviews.end()) {
        return;
    }
    it->controller->remove_AcceleratorKeyPressed(it->token);
    m_webviews.erase(it);
}

void AcceleratorTable::detach_all_webviews() noexcept {
    for (WebViewHook& hook : m_webviews) {
        hook.controller->remove_AcceleratorKeyPressed(hook.token);
    }
    m_webviews.clear();
}

} // namespace boson::menu
