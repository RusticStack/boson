#pragma once

#include <boson/core/window.hpp>
#include <boson/desktop/screen.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/controllers/desktop_events.hpp>
#include <boson/ipc/export.hpp>

#include <glaze/glaze.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace boson::ipc {

struct ScreenDisplaysResponse {
    std::vector<desktop::Display> displays;
};

/// @brief JS "Screen.*". Monitor queries are thread-safe Win32 calls, answered on the pool thread.
class ScreenController {
public:
    BOSON_CONTROLLER("Screen");

    explicit ScreenController(core::Window& window) noexcept
        : m_window(window) {}

    [[nodiscard]] ScreenDisplaysResponse get_all_displays() const {
        return ScreenDisplaysResponse{.displays = desktop::displays()};
    }
    [[nodiscard]] desktop::Display get_primary_display() const { return require(desktop::primary_display()); }
    [[nodiscard]] desktop::Display get_display_nearest_point(desktop::ScreenPoint point) const {
        return require(desktop::display_nearest_point(point));
    }
    [[nodiscard]] desktop::Display get_display_matching(desktop::ScreenRect rect) const {
        return require(desktop::display_nearest_rect(rect));
    }
    /// The display the calling app's main window is on.
    [[nodiscard]] desktop::Display get_window_display() const { return require(desktop::display_for_window(m_window.hwnd())); }
    [[nodiscard]] desktop::ScreenPoint get_cursor_screen_point() const { return desktop::cursor_screen_point(); }

private:
    static desktop::Display require(std::optional<desktop::Display> display) {
        if (!display) {
            throw std::runtime_error("no display found");
        }
        return std::move(*display);
    }

    core::Window& m_window;
};

/// @brief Emits "screen:changed" to every attached WebView while the returned watcher lives. UI thread.
[[nodiscard]] inline std::unique_ptr<desktop::DisplayWatcher> watch_screen(Bridge& bridge, core::Window* window = nullptr) {
    return std::make_unique<desktop::DisplayWatcher>(
        [&bridge](const desktop::ScreenChange& change) {
            ScreenChangedEvent event{.reason = change.reason, .displays = change.displays};
            for (const auto& d : change.diff.added) event.added.push_back(d.id);
            for (const auto& d : change.diff.removed) event.removed.push_back(d.id);
            for (const auto& d : change.diff.changed) event.changed.push_back(d.id);
            bridge.emit("screen:changed", event);
        },
        window);
}

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::ScreenController> {
    using T = boson::ipc::ScreenController;
    static constexpr auto value = glz::object(
        "get_all_displays", &T::get_all_displays,
        "get_primary_display", &T::get_primary_display,
        "get_display_nearest_point", &T::get_display_nearest_point,
        "get_display_matching", &T::get_display_matching,
        "get_window_display", &T::get_window_display,
        "get_cursor_screen_point", &T::get_cursor_screen_point);
};
