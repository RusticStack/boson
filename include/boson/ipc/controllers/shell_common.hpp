#pragma once

#include <boson/ipc/bridge.hpp>
#include <boson/shell/icon.hpp>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace boson::ipc {

/// @brief Generic success/failure reply of the shell-integration controllers.
struct ShellResult {
    bool ok = false;
    std::string error;
};

/// @brief Icon description used by the Taskbar controller. The first non-empty source wins:
/// badge (text, e.g. "3"), stock (info, warning, error, shield, ...), glyph (Segoe Fluent Icons codepoint),
/// file ("C:\\path\\app.ico" or "app.exe,0").
struct IconSpec {
    std::string badge;
    std::string stock;
    std::uint32_t glyph = 0;
    std::string file;
    std::string background = "#C42B1C"; ///< badge disc
    std::string foreground = "#FFFFFF"; ///< badge text / glyph colour
    int size = 0;                       ///< px; 0 = SM_CXSMICON
};

namespace shell_detail {

[[nodiscard]] inline COLORREF parse_color(std::string_view text, COLORREF fallback) noexcept {
    if (text.starts_with('#')) {
        text.remove_prefix(1);
    }
    if (text.size() != 6) {
        return fallback;
    }
    unsigned value = 0;
    for (const char ch : text) {
        const int c = std::tolower(static_cast<unsigned char>(ch));
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a' + 10);
        } else {
            return fallback;
        }
        value = (value << 4) | digit;
    }
    return RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
}

/// @brief nullptr when the spec is empty; throws when a source is given but cannot be loaded.
[[nodiscard]] inline std::shared_ptr<const shell::Icon> make_icon(const IconSpec& spec) {
    shell::Icon icon;
    if (!spec.badge.empty()) {
        icon = shell::Icon::badge(core::utf8_to_wide(spec.badge), parse_color(spec.background, RGB(0xC4, 0x2B, 0x1C)),
                                  parse_color(spec.foreground, RGB(0xFF, 0xFF, 0xFF)), spec.size);
    } else if (!spec.stock.empty()) {
        icon = shell::Icon::stock(core::utf8_to_wide(spec.stock), spec.size <= 0 || spec.size <= shell::default_small_icon_size());
    } else if (spec.glyph != 0) {
        if (spec.glyph > 0xFFFF) {
            throw std::invalid_argument("glyph must be a BMP codepoint");
        }
        icon = shell::Icon::glyph(static_cast<wchar_t>(spec.glyph), parse_color(spec.foreground, RGB(0xFF, 0xFF, 0xFF)), spec.size);
    } else if (!spec.file.empty()) {
        icon = shell::Icon::from_file(core::utf8_to_wide(spec.file), spec.size);
    } else {
        return nullptr;
    }
    if (!icon.valid()) {
        throw std::runtime_error("Could not create the requested icon");
    }
    return std::make_shared<const shell::Icon>(std::move(icon));
}

/// @brief Bridge pointer that controllers emit through; set after the Bridge exists. Any thread.
class EventSink {
public:
    void set(Bridge* bridge) noexcept { m_bridge.store(bridge, std::memory_order_release); }

    template <typename T>
    void emit(std::string_view event, const T& payload) const {
        if (Bridge* bridge = m_bridge.load(std::memory_order_acquire)) {
            bridge->emit(event, payload);
        }
    }

private:
    std::atomic<Bridge*> m_bridge{nullptr};
};

} // namespace shell_detail

} // namespace boson::ipc
