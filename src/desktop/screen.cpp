#include <boson/desktop/screen.hpp>

#include <boson/core/result.hpp>
#include <boson/core/window.hpp>

#include "desktop_util.hpp"

#include <windows.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace boson::desktop {

namespace {

ScreenRect to_rect(const RECT& rc) noexcept {
    return ScreenRect{rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top};
}

/// GDI source device name -> monitor friendly name, from QueryDisplayConfig.
std::unordered_map<std::wstring, std::wstring> friendly_names() {
    std::unordered_map<std::wstring, std::wstring> out;
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 4 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
            return out;
        }
        paths.resize(path_count);
        modes.resize(mode_count);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    }
    if (status != ERROR_SUCCESS) {
        return out;
    }
    paths.resize(path_count);
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
            continue;
        }
        std::wstring name = target.monitorFriendlyDeviceName;
        if (name.empty() && path.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL) {
            name = L"Built-in display";
        }
        out.emplace(source.viewGdiDeviceName, std::move(name));
    }
    return out;
}

std::optional<Display> describe(HMONITOR monitor, const std::unordered_map<std::wstring, std::wstring>* names) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
        return std::nullopt;
    }
    Display display;
    display.device_name = core::wide_to_utf8(info.szDevice);
    display.id = display.device_name;
    display.bounds = to_rect(info.rcMonitor);
    display.work_area = to_rect(info.rcWork);
    display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    UINT dpi_x = USER_DEFAULT_SCREEN_DPI;
    UINT dpi_y = USER_DEFAULT_SCREEN_DPI;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x != 0) {
        display.dpi = dpi_x;
    }
    display.scale_factor = static_cast<double>(display.dpi) / USER_DEFAULT_SCREEN_DPI;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0)) {
        if (mode.dmDisplayFrequency > 1) { // 0 and 1 mean "hardware default"
            display.refresh_rate = static_cast<double>(mode.dmDisplayFrequency);
        }
        display.bits_per_pixel = static_cast<int>(mode.dmBitsPerPel);
        if ((mode.dmFields & DM_DISPLAYORIENTATION) != 0) {
            display.rotation = static_cast<int>(mode.dmDisplayOrientation) * 90;
        }
    }

    if (names != nullptr) {
        if (const auto it = names->find(info.szDevice); it != names->end()) {
            display.label = core::wide_to_utf8(it->second);
        }
    } else {
        const auto all = friendly_names();
        if (const auto it = all.find(info.szDevice); it != all.end()) {
            display.label = core::wide_to_utf8(it->second);
        }
    }
    return display;
}

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* monitors = reinterpret_cast<std::vector<HMONITOR>*>(data);
    monitors->push_back(monitor);
    return TRUE;
}

} // namespace

std::vector<Display> displays() {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &collect_monitor, reinterpret_cast<LPARAM>(&monitors));
    const auto names = friendly_names();
    std::vector<Display> out;
    out.reserve(monitors.size());
    for (const HMONITOR monitor : monitors) {
        if (auto display = describe(monitor, &names)) {
            out.push_back(std::move(*display));
        }
    }
    // Primary first, then left-to-right, top-to-bottom: a stable order for UIs and diffs.
    std::stable_sort(out.begin(), out.end(), [](const Display& a, const Display& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.bounds.x != b.bounds.x) return a.bounds.x < b.bounds.x;
        return a.bounds.y < b.bounds.y;
    });
    return out;
}

std::optional<Display> display_from_monitor(HMONITOR monitor) {
    return describe(monitor, nullptr);
}

std::optional<Display> primary_display() {
    return describe(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), nullptr);
}

std::optional<Display> display_nearest_point(ScreenPoint point) {
    return describe(MonitorFromPoint(POINT{point.x, point.y}, MONITOR_DEFAULTTONEAREST), nullptr);
}

std::optional<Display> display_nearest_rect(ScreenRect rect) {
    const RECT rc{rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
    return describe(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), nullptr);
}

std::optional<Display> display_for_window(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return std::nullopt;
    }
    return describe(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), nullptr);
}

ScreenPoint cursor_screen_point() noexcept {
    POINT pt{};
    if (!GetCursorPos(&pt)) {
        // Fails on a secure desktop / with no interactive session; report the primary origin.
        return ScreenPoint{};
    }
    return ScreenPoint{pt.x, pt.y};
}

ScreenRect virtual_screen() noexcept {
    return ScreenRect{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
}

DisplayDiff diff_displays(const std::vector<Display>& before, const std::vector<Display>& after) {
    DisplayDiff diff;
    for (const auto& now : after) {
        const auto it = std::find_if(before.begin(), before.end(), [&](const Display& d) { return d.id == now.id; });
        if (it == before.end()) {
            diff.added.push_back(now);
        } else if (!(*it == now)) {
            diff.changed.push_back(now);
        }
    }
    for (const auto& old : before) {
        const auto it = std::find_if(after.begin(), after.end(), [&](const Display& d) { return d.id == old.id; });
        if (it == after.end()) {
            diff.removed.push_back(old);
        }
    }
    return diff;
}

// ---- DisplayWatcher --------------------------------------------------------------------------------------

DisplayWatcher::DisplayWatcher(Callback callback, core::Window* window, std::chrono::milliseconds debounce)
    : m_callback(std::move(callback)), m_window(window), m_debounce(debounce), m_snapshot(displays()) {
    m_hidden = std::make_unique<detail::HiddenWindow>(
        [this](UINT msg, WPARAM w_param, LPARAM) -> std::optional<LRESULT> {
            switch (msg) {
            case WM_DISPLAYCHANGE:
                schedule("display-change");
                break;
            case WM_SETTINGCHANGE:
                schedule(w_param == SPI_SETWORKAREA ? "work-area" : "settings");
                break;
            case WM_DPICHANGED:
                schedule("dpi");
                break;
            default:
                break;
            }
            return std::nullopt;
        },
        false); // top-level (hidden) so it receives broadcasts
    if (m_window != nullptr) {
        m_hook_id = m_window->add_message_hook([this](UINT msg, WPARAM, LPARAM) -> std::optional<LRESULT> {
            if (msg == WM_DPICHANGED) {
                schedule("dpi");
            }
            return std::nullopt; // never consume: Window still handles the message
        });
    }
}

DisplayWatcher::~DisplayWatcher() {
    m_timer.stop();
    if (m_window != nullptr && m_hook_id != 0) {
        m_window->remove_message_hook(m_hook_id);
    }
}

HWND DisplayWatcher::hwnd() const noexcept {
    return m_hidden ? m_hidden->hwnd() : nullptr;
}

void DisplayWatcher::schedule(const char* reason) {
    // The most specific reason wins when several notifications coalesce.
    const std::string_view r{reason};
    if (m_pending_reason.empty() || m_pending_reason == "settings" || r == "display-change") {
        m_pending_reason = reason;
    }
    m_timer.start(m_debounce, [this]() {
        std::string pending = std::move(m_pending_reason);
        m_pending_reason.clear();
        refresh(pending.empty() ? std::string("settings") : pending);
    });
}

void DisplayWatcher::refresh(const std::string& reason) {
    std::vector<Display> now = displays();
    DisplayDiff diff = diff_displays(m_snapshot, now);
    if (diff.empty()) {
        return;
    }
    m_snapshot = now;
    if (m_callback) {
        m_callback(ScreenChange{.reason = reason, .displays = std::move(now), .diff = std::move(diff)});
    }
}

} // namespace boson::desktop
