#include <boson/desktop/window_state.hpp>

#include <boson/core/result.hpp>
#include <boson/core/window.hpp>
#include <boson/desktop/shell.hpp>
#include <boson/exec/task_runner.hpp>

#include "desktop_util.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <fstream>
#include <mutex>

namespace boson::desktop {

namespace {

long long overlap_area(const ScreenRect& a, const ScreenRect& b) noexcept {
    const long long left = std::max(a.x, b.x);
    const long long top = std::max(a.y, b.y);
    const long long right = std::min(static_cast<long long>(a.x) + a.width, static_cast<long long>(b.x) + b.width);
    const long long bottom = std::min(static_cast<long long>(a.y) + a.height, static_cast<long long>(b.y) + b.height);
    return (right > left && bottom > top) ? (right - left) * (bottom - top) : 0;
}

std::string serialise(const WindowState& state) {
    std::string json;
    if (glz::write<glz::opts{.prettify = true}>(state, json)) {
        return {};
    }
    return json;
}

/// Offset between workspace coordinates (WINDOWPLACEMENT) and screen coordinates for a monitor.
POINT workspace_offset(HWND hwnd, HMONITOR monitor) noexcept {
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return POINT{0, 0};
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return POINT{0, 0};
    }
    return POINT{info.rcWork.left - info.rcMonitor.left, info.rcWork.top - info.rcMonitor.top};
}

std::uint32_t monitor_dpi(HMONITOR monitor) noexcept {
    UINT dpi_x = USER_DEFAULT_SCREEN_DPI;
    UINT dpi_y = USER_DEFAULT_SCREEN_DPI;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x != 0) {
        return dpi_x;
    }
    return USER_DEFAULT_SCREEN_DPI;
}

std::string monitor_id(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return {};
    }
    return core::wide_to_utf8(info.szDevice);
}

std::wstring sanitise_id(std::wstring_view id) {
    std::wstring out;
    for (const wchar_t ch : id) {
        const bool ok = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') ||
                        ch == L'.' || ch == L'_' || ch == L'-';
        out.push_back(ok ? ch : L'_');
    }
    return out.empty() ? std::wstring(L"main") : out;
}

} // namespace

std::vector<MonitorArea> current_monitor_areas() {
    std::vector<MonitorArea> out;
    for (const auto& display : displays()) {
        out.push_back(MonitorArea{.id = display.id, .bounds = display.bounds, .work_area = display.work_area, .dpi = display.dpi});
    }
    return out;
}

WindowState fit_window_state(WindowState state, std::span<const MonitorArea> monitors, int min_visible) {
    if (monitors.empty()) {
        return state;
    }
    ScreenRect rect{state.x, state.y, std::max(1, state.width), std::max(1, state.height)};

    const MonitorArea* saved = nullptr;
    for (const auto& monitor : monitors) {
        if (!state.monitor.empty() && monitor.id == state.monitor) {
            saved = &monitor;
        }
    }
    const MonitorArea* best = nullptr;
    long long best_area = 0;
    for (const auto& monitor : monitors) {
        const long long area = overlap_area(rect, monitor.work_area);
        if (area > best_area) {
            best_area = area;
            best = &monitor;
        }
    }
    const MonitorArea* target = nullptr;
    if (saved != nullptr && overlap_area(rect, saved->work_area) > 0) {
        target = saved;
    } else if (best != nullptr) {
        target = best;
    } else {
        target = saved != nullptr ? saved : &monitors.front();
    }
    const ScreenRect& wa = target->work_area;

    // A saved state from another DPI: keep the physical size proportional to the scale change.
    if (state.dpi != 0 && target->dpi != 0 && state.dpi != target->dpi) {
        const double ratio = static_cast<double>(target->dpi) / static_cast<double>(state.dpi);
        rect.width = static_cast<int>(rect.width * ratio + 0.5);
        rect.height = static_cast<int>(rect.height * ratio + 0.5);
        if (best_area == 0) {
            // Off-screen rect being moved anyway: keep its offset inside the work area proportional too.
            rect.x = wa.x + static_cast<int>((rect.x - wa.x) * ratio);
            rect.y = wa.y + static_cast<int>((rect.y - wa.y) * ratio);
        }
    }

    rect.width = std::clamp(rect.width, 1, std::max(1, wa.width));
    rect.height = std::clamp(rect.height, 1, std::max(1, wa.height));

    const int right_limit = wa.x + wa.width - rect.width;
    const int bottom_limit = wa.y + wa.height - rect.height;
    if (best_area == 0) {
        // Not on any monitor: place it entirely inside the chosen work area.
        rect.x = std::clamp(rect.x, wa.x, std::max(wa.x, right_limit));
        rect.y = std::clamp(rect.y, wa.y, std::max(wa.y, bottom_limit));
    }
    // The title bar (top edge) must be inside the work area vertically.
    if (rect.y < wa.y || rect.y > wa.y + wa.height - std::min(min_visible, rect.height)) {
        rect.y = std::clamp(rect.y, wa.y, std::max(wa.y, bottom_limit));
    }
    // And at least min_visible pixels of it horizontally.
    const int visible = std::min(rect.x + rect.width, wa.x + wa.width) - std::max(rect.x, wa.x);
    if (visible < std::min(min_visible, rect.width)) {
        rect.x = std::clamp(rect.x, wa.x, std::max(wa.x, right_limit));
    }

    state.x = rect.x;
    state.y = rect.y;
    state.width = rect.width;
    state.height = rect.height;
    state.monitor = target->id;
    state.dpi = target->dpi;
    return state;
}

std::filesystem::path default_window_state_path(std::wstring_view app_name, std::wstring_view window_id) {
    auto base = known_folder(KnownFolder::LocalAppData);
    std::filesystem::path root = base ? *base : std::filesystem::temp_directory_path();
    root /= std::wstring(app_name.empty() ? std::wstring_view{L"Boson"} : app_name);
    return root / (L"window-state-" + sanitise_id(window_id) + L".json");
}

std::optional<WindowState> load_window_state(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    WindowState state;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(state, json)) {
        return std::nullopt;
    }
    if (state.width <= 0 || state.height <= 0 || state.width > 100000 || state.height > 100000) {
        return std::nullopt;
    }
    return state;
}

bool save_window_state(const std::filesystem::path& file, const WindowState& state) {
    const std::string json = serialise(state);
    if (json.empty()) {
        return false;
    }
    std::error_code ec;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    std::filesystem::path temp = file;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out) {
            return false;
        }
    }
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "save_window_state: MoveFileExW");
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

// ---- WindowStateKeeper -----------------------------------------------------------------------------------

/// Serialises writes from the UI thread and the TaskRunner; a stale generation never overwrites a newer one.
struct WindowStateKeeper::Writer {
    std::mutex mutex;
    std::filesystem::path file;
    std::uint64_t next_generation = 1;
    std::uint64_t written_generation = 0;

    bool write(std::uint64_t generation, const WindowState& state) {
        std::scoped_lock lock(mutex);
        if (generation <= written_generation) {
            return true;
        }
        written_generation = generation;
        return save_window_state(file, state);
    }
};

WindowStateKeeper::WindowStateKeeper(core::Window& window, WindowStateOptions options)
    : m_window(window), m_options(std::move(options)), m_writer(std::make_shared<Writer>()) {
    m_file = m_options.file.empty() ? default_window_state_path(m_options.app_name, m_options.window_id) : m_options.file;
    m_writer->file = m_file;

    using core::WindowEventKind;
    for (const auto kind : {WindowEventKind::Move, WindowEventKind::Resize, WindowEventKind::Maximize,
                            WindowEventKind::Restore, WindowEventKind::ExitSizeMove, WindowEventKind::DpiChanged}) {
        m_listener_ids.push_back(m_window.on(kind, [this](core::WindowEvent&) { schedule_save(); }));
    }
    m_listener_ids.push_back(m_window.on(WindowEventKind::CloseRequested, [this](core::WindowEvent&) {
        m_timer.stop();
        save_now();
    }));
    m_listener_ids.push_back(m_window.on(WindowEventKind::Destroyed, [this](core::WindowEvent&) {
        m_timer.stop();
        save_now();
        m_closed = true;
    }));
}

WindowStateKeeper::~WindowStateKeeper() {
    m_timer.stop();
    for (const auto id : m_listener_ids) {
        m_window.off(id);
    }
}

std::optional<WindowState> WindowStateKeeper::capture() {
    const HWND hwnd = m_window.hwnd();
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return std::nullopt;
    }
    if (m_window.is_fullscreen()) {
        if (!m_last_windowed) {
            return std::nullopt;
        }
        WindowState state = *m_last_windowed;
        state.fullscreen = true;
        return state;
    }
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd, &placement)) {
        return std::nullopt;
    }
    const RECT& normal = placement.rcNormalPosition;
    const HMONITOR monitor = MonitorFromRect(&normal, MONITOR_DEFAULTTONEAREST);
    const POINT offset = workspace_offset(hwnd, monitor);

    WindowState state;
    state.x = normal.left + offset.x;
    state.y = normal.top + offset.y;
    state.width = normal.right - normal.left;
    state.height = normal.bottom - normal.top;
    state.maximized = IsZoomed(hwnd) != FALSE ||
                      (IsIconic(hwnd) != FALSE && (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);
    state.fullscreen = false;
    state.monitor = monitor_id(state.maximized ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : monitor);
    state.dpi = monitor_dpi(monitor);
    if (state.width <= 0 || state.height <= 0) {
        return std::nullopt;
    }
    m_last_windowed = state;
    return state;
}

bool WindowStateKeeper::save_now() {
    if (m_closed) {
        return false;
    }
    const auto state = capture();
    if (!state) {
        return false;
    }
    if (m_last_saved && *m_last_saved == *state) {
        return true;
    }
    std::uint64_t generation = 0;
    {
        std::scoped_lock lock(m_writer->mutex);
        generation = m_writer->next_generation++;
    }
    const bool ok = m_writer->write(generation, *state);
    if (ok) {
        m_last_saved = state;
    }
    return ok;
}

void WindowStateKeeper::schedule_save() {
    if (m_restoring || m_closed || m_window.is_minimized()) {
        return;
    }
    m_timer.start(m_options.debounce, [this]() {
        const auto state = capture();
        if (!state || (m_last_saved && *m_last_saved == *state)) {
            return;
        }
        std::uint64_t generation = 0;
        {
            std::scoped_lock lock(m_writer->mutex);
            generation = m_writer->next_generation++;
        }
        m_last_saved = state;
        if (m_options.runner != nullptr) {
            m_options.runner->spawn([writer = m_writer, generation, snapshot = *state]() { writer->write(generation, snapshot); });
        } else {
            m_writer->write(generation, *state);
        }
    });
}

void WindowStateKeeper::apply_placement(const WindowState& state, int show_cmd) {
    const HWND hwnd = m_window.hwnd();
    const RECT screen{state.x, state.y, state.x + state.width, state.y + state.height};
    const HMONITOR monitor = MonitorFromRect(&screen, MONITOR_DEFAULTTONEAREST);
    const POINT offset = workspace_offset(hwnd, monitor);

    if (show_cmd == SW_SHOWDEFAULT) {
        // SetWindowPlacement does not consult STARTUPINFO the way ShowWindow does.
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        GetStartupInfoW(&startup);
        show_cmd = (startup.dwFlags & STARTF_USESHOWWINDOW) != 0 ? startup.wShowWindow : SW_SHOWNORMAL;
    }
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    placement.flags = state.maximized ? WPF_RESTORETOMAXIMIZED : 0;
    placement.showCmd = static_cast<UINT>(state.maximized && show_cmd != SW_HIDE ? SW_SHOWMAXIMIZED : show_cmd);
    placement.ptMinPosition = POINT{-1, -1};
    placement.ptMaxPosition = POINT{-1, -1};
    placement.rcNormalPosition = RECT{screen.left - offset.x, screen.top - offset.y, screen.right - offset.x, screen.bottom - offset.y};
    SetWindowPlacement(hwnd, &placement);

    // Crossing into a monitor with another DPI makes WM_DPICHANGED resize the window to the suggested
    // rect. The window is now on the right monitor, so a second pass sets the exact size.
    WINDOWPLACEMENT applied{};
    applied.length = sizeof(applied);
    if (GetWindowPlacement(hwnd, &applied) && !EqualRect(&applied.rcNormalPosition, &placement.rcNormalPosition)) {
        SetWindowPlacement(hwnd, &placement);
    }
}

bool WindowStateKeeper::restore(int show_cmd) {
    const auto saved = load_window_state(m_file);
    if (!saved) {
        m_window.show(show_cmd);
        return false;
    }
    const auto monitors = current_monitor_areas();
    const WindowState fitted = fit_window_state(*saved, monitors);
    m_restoring = true;
    apply_placement(fitted, show_cmd);
    if (fitted.fullscreen && m_options.restore_fullscreen) {
        [[maybe_unused]] const auto windowed = capture(); // remembers the windowed geometry first
        m_window.set_fullscreen(true);
    }
    m_restoring = false;
    m_last_saved = saved;
    return true;
}

} // namespace boson::desktop
