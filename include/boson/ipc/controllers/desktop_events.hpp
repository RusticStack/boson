#pragma once

#include <boson/desktop/screen.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace boson::ipc {

/// "clipboard:changed": any process changed the clipboard. Format flags come from
/// IsClipboardFormatAvailable (the clipboard is not opened on the UI thread).
struct ClipboardChangedEvent {
    std::uint32_t sequence_number = 0;
    bool has_text = false;
    bool has_html = false;
    bool has_image = false;
    bool has_files = false;
};

/// "screen:changed": a monitor was added, removed or changed bounds / work area / scale / refresh rate.
struct ScreenChangedEvent {
    std::string reason; ///< "display-change" | "work-area" | "dpi" | "settings"
    std::vector<desktop::Display> displays;
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> changed;
};

/// "shortcut:triggered": a global shortcut registered through Shortcut.register was pressed.
struct ShortcutTriggeredEvent {
    std::uint32_t id = 0;
    std::string accelerator;
};

/// Event registry for TsGenerator::generate_with_events (combine with the app's own events).
struct DesktopEvents {
    ClipboardChangedEvent clipboard_changed;
    ScreenChangedEvent screen_changed;
    ShortcutTriggeredEvent shortcut_triggered;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::DesktopEvents> {
    using T = boson::ipc::DesktopEvents;
    static constexpr auto value = glz::object(
        "clipboard:changed", &T::clipboard_changed,
        "screen:changed", &T::screen_changed,
        "shortcut:triggered", &T::shortcut_triggered);
};
