#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::core {
class Window;
class WebView;
} // namespace boson::core

namespace boson::menu {

/// @brief Modifier bit set of an accelerator. Combine with operator|.
enum class KeyModifiers : std::uint8_t {
    None = 0,
    Ctrl = 1U << 0U,
    Alt = 1U << 1U,
    Shift = 1U << 2U,
    Win = 1U << 3U,
};

[[nodiscard]] constexpr KeyModifiers operator|(KeyModifiers a, KeyModifiers b) noexcept {
    return static_cast<KeyModifiers>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr KeyModifiers operator&(KeyModifiers a, KeyModifiers b) noexcept {
    return static_cast<KeyModifiers>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr bool has_modifier(KeyModifiers set, KeyModifiers flag) noexcept {
    return (set & flag) != KeyModifiers::None;
}

/// @brief One key chord: modifiers + a Win32 virtual-key code.
struct Accelerator {
    KeyModifiers modifiers = KeyModifiers::None;
    UINT key = 0;

    /// @brief Parses "Ctrl+Shift+P", "CmdOrCtrl+S", "Alt+F4", "Win+Shift+S", "F11", "Ctrl+Plus", "Ctrl+=" ...
    /// Case-insensitive; Cmd/Command/CmdOrCtrl/CommandOrControl map to Ctrl and Super/Meta to Win so
    /// cross-platform templates keep working. Returns std::nullopt for an unknown key or a modifier-only chord.
    [[nodiscard]] static std::optional<Accelerator> parse(std::string_view text);

    /// @brief Canonical display text, e.g. "Ctrl+Alt+Shift+Win+K" (modifier order is fixed).
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] constexpr bool valid() const noexcept { return key != 0; }

    friend constexpr bool operator==(const Accelerator&, const Accelerator&) noexcept = default;
};

/// @brief Display name of a virtual key ("A", "F5", "PageUp", "Plus", ...); empty when it has no name.
[[nodiscard]] std::string_view key_name(UINT vk) noexcept;

/// @brief Modifiers currently held according to this thread's keyboard state (GetKeyState).
[[nodiscard]] KeyModifiers current_modifiers() noexcept;

/// @brief true for VK_SHIFT/VK_CONTROL/VK_MENU/VK_LWIN/... (keys that cannot be an accelerator's main key).
[[nodiscard]] bool is_modifier_key(UINT vk) noexcept;

/// @brief Keyboard shortcut table for one Window and any number of its WebViews.
///
/// Keys reach a Boson app on two paths, and the table covers both with the same callbacks:
///  * keys posted to the window's own thread (focus on the window, not inside a WebView): picked up by a
///    thread WH_GETMESSAGE hook before TranslateMessage/DispatchMessage, so it works with
///    Window::run_message_loop and with any custom PeekMessage/GetMessage loop;
///  * keys typed while a WebView has focus (WebView2 swallows those): ICoreWebView2Controller
///    AcceleratorKeyPressed, marked Handled when a chord matches.
/// Callbacks never run inside the hook or the COM event: they are deferred with a PostMessage to the
/// attached window, so they may open dialogs or menus. All members must be called on the UI thread.
class AcceleratorTable {
public:
    using Id = std::uint64_t;
    using Callback = std::function<void()>;
    /// Called for a WebView key chord that matched nothing. Return true to mark it handled.
    using UnmatchedKeyHandler = std::function<bool(const Accelerator& chord, bool system_key)>;

    AcceleratorTable() = default;
    explicit AcceleratorTable(core::Window& window);
    ~AcceleratorTable();

    AcceleratorTable(const AcceleratorTable&) = delete;
    AcceleratorTable& operator=(const AcceleratorTable&) = delete;
    AcceleratorTable(AcceleratorTable&&) = delete;
    AcceleratorTable& operator=(AcceleratorTable&&) = delete;

    Id add(Accelerator accelerator, Callback callback);
    /// @brief Parses text with Accelerator::parse; std::nullopt when it does not parse.
    std::optional<Id> add(std::string_view accelerator, Callback callback);
    bool remove(Id id) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }
    [[nodiscard]] bool contains(const Accelerator& accelerator) const noexcept;

    /// @brief Ignore keyboard auto-repeat (default) or fire once per repeat.
    void set_allow_repeat(bool allow) noexcept { m_allow_repeat = allow; }
    void set_enabled(bool enabled) noexcept { m_enabled = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
    void set_unmatched_webview_key_handler(UnmatchedKeyHandler handler) { m_unmatched = std::move(handler); }

    /// @brief Starts handling keys posted to window (and its same-thread child windows).
    bool attach(core::Window& window);
    void detach_window() noexcept;
    [[nodiscard]] HWND attached_hwnd() const noexcept { return m_hwnd; }

    /// @brief Registers AcceleratorKeyPressed on webview's controller (must be ready).
    bool attach_webview(core::WebView& webview);
    bool attach_webview(ICoreWebView2Controller* controller);
    void detach_webview(core::WebView& webview) noexcept;
    void detach_webview(ICoreWebView2Controller* controller) noexcept;
    void detach_all_webviews() noexcept;
    [[nodiscard]] std::size_t webview_count() const noexcept { return m_webviews.size(); }

    /// @brief Looks up chord and schedules its callback. Returns true when it matched an entry.
    bool dispatch(const Accelerator& chord, bool is_repeat = false);
    /// @brief For custom message loops / tests: true when msg is a WM_KEYDOWN / WM_SYSKEYDOWN that matched.
    bool translate(const MSG& msg);
    /// @brief The AcceleratorKeyPressed decision, exposed for tests. true = mark the event handled.
    bool handle_webview_key(COREWEBVIEW2_KEY_EVENT_KIND kind, UINT virtual_key, bool is_repeat);

private:
    struct Entry {
        Id id = 0;
        Accelerator accelerator;
        Callback callback;
    };
    struct WebViewHook {
        Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
        EventRegistrationToken token{};
    };

    static LRESULT CALLBACK get_message_hook(int code, WPARAM w_param, LPARAM l_param);
    void register_thread_hook();
    void unregister_thread_hook() noexcept;
    void run_entry(Id id);

    std::vector<Entry> m_entries;
    std::vector<WebViewHook> m_webviews;
    core::Window* m_window = nullptr;
    HWND m_hwnd = nullptr;
    std::uint64_t m_window_hook = 0;
    std::uint64_t m_serial = 0;
    Id m_next_id = 1;
    bool m_allow_repeat = false;
    bool m_enabled = true;
    bool m_thread_hooked = false;
    UnmatchedKeyHandler m_unmatched;
};

} // namespace boson::menu
