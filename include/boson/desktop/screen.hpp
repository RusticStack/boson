#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boson/core/ui_timer.hpp>

namespace boson::core {
class Window;
} // namespace boson::core

namespace boson::desktop {

namespace detail {
class HiddenWindow;
} // namespace detail

/// @brief Rectangle in physical screen pixels (virtual-screen coordinates).
struct ScreenRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    friend bool operator==(const ScreenRect&, const ScreenRect&) = default;
};

struct ScreenPoint {
    int x = 0;
    int y = 0;
};

struct Display {
    std::string id;            ///< GDI device name, stable while the monitor stays connected ("\\\\.\\DISPLAY1")
    std::string device_name;   ///< same as id (MONITORINFOEXW::szDevice)
    std::string label;         ///< monitor friendly name from DisplayConfig ("DELL U2720Q"), may be empty
    ScreenRect bounds;         ///< full monitor rect, physical pixels
    ScreenRect work_area;      ///< bounds minus taskbar / app bars
    std::uint32_t dpi = 96;    ///< GetDpiForMonitor(MDT_EFFECTIVE_DPI)
    double scale_factor = 1.0; ///< dpi / 96
    bool primary = false;
    double refresh_rate = 0.0; ///< Hz (dmDisplayFrequency), 0 when unknown
    int rotation = 0;          ///< degrees: 0, 90, 180, 270
    int bits_per_pixel = 0;

    friend bool operator==(const Display&, const Display&) = default;
};

[[nodiscard]] std::vector<Display> displays();
[[nodiscard]] std::optional<Display> primary_display();
[[nodiscard]] std::optional<Display> display_nearest_point(ScreenPoint point);
[[nodiscard]] std::optional<Display> display_nearest_rect(ScreenRect rect);
[[nodiscard]] std::optional<Display> display_for_window(HWND hwnd);
[[nodiscard]] std::optional<Display> display_from_monitor(HMONITOR monitor);
[[nodiscard]] ScreenPoint cursor_screen_point() noexcept;
/// @brief Bounding box of every monitor (SM_XVIRTUALSCREEN...).
[[nodiscard]] ScreenRect virtual_screen() noexcept;

struct DisplayDiff {
    std::vector<Display> added;
    std::vector<Display> removed;
    std::vector<Display> changed; ///< same id, different bounds / work area / scale / refresh / rotation
    [[nodiscard]] bool empty() const noexcept { return added.empty() && removed.empty() && changed.empty(); }
};

/// @brief Pure comparison of two display snapshots, matched by id.
[[nodiscard]] DisplayDiff diff_displays(const std::vector<Display>& before, const std::vector<Display>& after);

struct ScreenChange {
    std::string reason; ///< "display-change", "work-area", "dpi", "settings"
    std::vector<Display> displays;
    DisplayDiff diff;
};

/// @brief Watches for display changes on the UI thread: WM_DISPLAYCHANGE and WM_SETTINGCHANGE broadcasts
/// (through a hidden top-level window) and, when a window is given, its WM_DPICHANGED. Notifications are
/// coalesced for debounce; the callback only runs when the display snapshot actually changed.
class DisplayWatcher {
public:
    using Callback = std::function<void(const ScreenChange& change)>;

    explicit DisplayWatcher(Callback callback, core::Window* window = nullptr,
                            std::chrono::milliseconds debounce = std::chrono::milliseconds{150});
    ~DisplayWatcher();
    DisplayWatcher(const DisplayWatcher&) = delete;
    DisplayWatcher& operator=(const DisplayWatcher&) = delete;

    [[nodiscard]] const std::vector<Display>& current() const noexcept { return m_snapshot; }
    /// @brief Re-reads the displays now and fires the callback when they differ from the last snapshot.
    void refresh(const std::string& reason);
    [[nodiscard]] HWND hwnd() const noexcept;

private:
    void schedule(const char* reason);

    Callback m_callback;
    core::Window* m_window = nullptr;
    std::uint64_t m_hook_id = 0;
    std::chrono::milliseconds m_debounce;
    std::unique_ptr<detail::HiddenWindow> m_hidden;
    core::UiTimer m_timer;
    std::string m_pending_reason;
    std::vector<Display> m_snapshot;
};

} // namespace boson::desktop
