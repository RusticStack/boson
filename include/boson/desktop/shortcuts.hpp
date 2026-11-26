#pragma once

#include <windows.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::desktop {

namespace detail {
class HiddenWindow;
} // namespace detail

/// @brief A parsed accelerator: RegisterHotKey modifiers (MOD_*) plus a virtual-key code.
struct Accelerator {
    UINT modifiers = 0; ///< MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN (never MOD_NOREPEAT)
    UINT key = 0;       ///< VK_* code
    /// Canonical spelling, e.g. "Ctrl+Shift+K": modifiers in the order Ctrl, Alt, Shift, Super.
    std::string canonical;

    friend bool operator==(const Accelerator& a, const Accelerator& b) noexcept {
        return a.modifiers == b.modifiers && a.key == b.key;
    }
};

/// @brief Parses an Electron-style accelerator ("CommandOrControl+Shift+K", "Alt+F4", "Super+Space",
/// "Ctrl+Plus", "MediaPlayPause", "num5"). Modifiers: Command/Cmd/Control/Ctrl/CommandOrControl/CmdOrCtrl
/// (all Ctrl on Windows), Alt/Option, AltGr (Ctrl+Alt), Shift, Super/Meta/Win. Case-insensitive. Single
/// punctuation keys are resolved with the active keyboard layout (VkKeyScanW), adding Shift when the
/// layout needs it.
[[nodiscard]] std::expected<Accelerator, std::string> parse_accelerator(std::string_view text);
/// @brief Name of a virtual key as used in canonical accelerators ("K", "F5", "Space", "Num5", ...).
[[nodiscard]] std::string virtual_key_name(UINT vk);
[[nodiscard]] std::string format_accelerator(UINT modifiers, UINT key);

enum class ShortcutErrorKind {
    InvalidAccelerator, ///< parse_accelerator failed
    AlreadyRegistered,  ///< this registry already holds the same combination
    Conflict,           ///< another application (or another registry) owns the hotkey
    SystemError,
};

struct ShortcutError {
    ShortcutErrorKind kind = ShortcutErrorKind::SystemError;
    std::string message;
};

struct ShortcutEvent {
    std::uint32_t id = 0;
    std::string accelerator; ///< canonical form
};

struct ShortcutInfo {
    std::uint32_t id = 0;
    std::string accelerator;
};

/// @brief RegisterHotKey-based global shortcut registry. Construct and use it on the UI thread: the hotkeys
/// belong to a private message-only window of that thread, and callbacks run there from the message loop.
/// Every shortcut is unregistered on destruction.
class GlobalShortcuts {
public:
    using Id = std::uint32_t;
    using Callback = std::function<void(const ShortcutEvent& event)>;

    GlobalShortcuts();
    ~GlobalShortcuts();
    GlobalShortcuts(const GlobalShortcuts&) = delete;
    GlobalShortcuts& operator=(const GlobalShortcuts&) = delete;

    /// @brief Registers accelerator with MOD_NOREPEAT (holding the keys fires once).
    [[nodiscard]] std::expected<Id, ShortcutError> add(std::string_view accelerator, Callback callback);
    bool remove(Id id) noexcept;
    /// @brief Unregisters by accelerator text (any spelling of the same combination).
    bool remove(std::string_view accelerator) noexcept;
    void remove_all() noexcept;

    [[nodiscard]] bool is_registered(std::string_view accelerator) const;
    [[nodiscard]] std::optional<Id> find(std::string_view accelerator) const;
    [[nodiscard]] std::vector<ShortcutInfo> list() const;
    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }
    /// @brief The window that receives WM_HOTKEY (exposed for tests and diagnostics).
    [[nodiscard]] HWND hwnd() const noexcept;

private:
    struct Entry {
        Id id = 0;
        Accelerator accelerator;
        Callback callback;
    };

    void on_hotkey(Id id);

    std::unique_ptr<detail::HiddenWindow> m_window;
    std::vector<Entry> m_entries;
    Id m_next_id = 1;
};

} // namespace boson::desktop
