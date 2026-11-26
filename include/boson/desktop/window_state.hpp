#pragma once

#include <windows.h>

#include <boson/core/ui_timer.hpp>
#include <boson/desktop/screen.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::core {
class Window;
} // namespace boson::core

namespace boson::exec {
class TaskRunner;
} // namespace boson::exec

namespace boson::desktop {

/// @brief Persisted window geometry. x/y/width/height are the restored (non-maximized) outer window rect
/// in physical screen pixels at dpi.
struct WindowState {
    int version = 1;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
    bool fullscreen = false;
    std::string monitor;      ///< Display::id of the monitor the window was on
    std::uint32_t dpi = 96;   ///< DPI of that monitor when saved

    friend bool operator==(const WindowState&, const WindowState&) = default;
};

struct MonitorArea {
    std::string id;
    ScreenRect bounds;
    ScreenRect work_area;
    std::uint32_t dpi = 96;
};

/// @brief The current monitors as MonitorArea (from displays()).
[[nodiscard]] std::vector<MonitorArea> current_monitor_areas();

/// @brief Pure: makes state usable on monitors. Picks the saved monitor when it still exists and the rect
/// overlaps it, else the monitor with the largest overlap, else the saved/first monitor; rescales the
/// size when that monitor's DPI differs from the saved DPI; shrinks the window to the work area; and moves
/// it so its title bar is on-screen (at least min_visible pixels wide). A state that is already visible
/// is returned unchanged apart from monitor/dpi.
[[nodiscard]] WindowState fit_window_state(WindowState state, std::span<const MonitorArea> monitors, int min_visible = 48);

/// @brief %LOCALAPPDATA%\<app_name>\window-state-<window_id>.json (window_id sanitised to [A-Za-z0-9._-]).
[[nodiscard]] std::filesystem::path default_window_state_path(std::wstring_view app_name, std::wstring_view window_id = L"main");
[[nodiscard]] std::optional<WindowState> load_window_state(const std::filesystem::path& file);
/// @brief Writes atomically (temp file + MoveFileExW(REPLACE_EXISTING)); creates parent directories.
bool save_window_state(const std::filesystem::path& file, const WindowState& state);

struct WindowStateOptions {
    /// Folder name under %LOCALAPPDATA%. Required unless file is set.
    std::wstring app_name;
    std::wstring window_id = L"main";
    /// Explicit file; overrides app_name/window_id.
    std::filesystem::path file;
    /// Quiet period after the last move/resize before writing.
    std::chrono::milliseconds debounce{400};
    /// Debounced writes run on this runner when set (the final save on close is always synchronous).
    exec::TaskRunner* runner = nullptr;
    bool restore_fullscreen = true;
};

/// @brief Saves a Window's placement on move/resize (debounced) and on close, and restores it on start.
/// Create it right after Window::create(instance, SW_HIDE), call restore(show_cmd) to show the window at
/// its saved place. UI thread only; must not outlive the window's message processing.
class WindowStateKeeper {
public:
    WindowStateKeeper(core::Window& window, WindowStateOptions options);
    ~WindowStateKeeper();
    WindowStateKeeper(const WindowStateKeeper&) = delete;
    WindowStateKeeper& operator=(const WindowStateKeeper&) = delete;

    /// @brief Applies the saved state (fitted to the current monitors) with SetWindowPlacement and shows
    /// the window with show_cmd (SW_SHOWMAXIMIZED when it was maximized). Without a saved state it only
    /// shows the window. Returns true when a saved state was applied.
    bool restore(int show_cmd = SW_SHOWNORMAL);
    /// @brief Current placement; while fullscreen, the last windowed geometry with fullscreen = true.
    [[nodiscard]] std::optional<WindowState> capture();
    /// @brief Synchronous write of capture(). Skips the write when nothing changed since the last save.
    bool save_now();
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return m_file; }
    [[nodiscard]] const std::optional<WindowState>& last_saved() const noexcept { return m_last_saved; }

private:
    struct Writer;

    void schedule_save();
    void apply_placement(const WindowState& state, int show_cmd);

    core::Window& m_window;
    WindowStateOptions m_options;
    std::filesystem::path m_file;
    std::vector<std::uint64_t> m_listener_ids;
    core::UiTimer m_timer;
    std::optional<WindowState> m_last_saved;
    std::optional<WindowState> m_last_windowed;
    std::shared_ptr<Writer> m_writer;
    bool m_restoring = false;
    bool m_closed = false;
};

} // namespace boson::desktop
