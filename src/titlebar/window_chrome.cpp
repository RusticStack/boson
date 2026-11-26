#include <boson/titlebar/window_chrome.hpp>

#include <shellapi.h>

#include <algorithm>
#include <array>

namespace boson::titlebar {

namespace {

constexpr int kKeyEventKindSystemKeyDown = 2; // COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN

void set_item(HMENU menu, UINT id, bool enabled) noexcept {
    EnableMenuItem(menu, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : (MF_DISABLED | MF_GRAYED)));
}

} // namespace

void update_system_menu_state(HWND hwnd, HMENU menu) noexcept {
    if (hwnd == nullptr || menu == nullptr) {
        return;
    }
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const bool maximized = IsZoomed(hwnd) != FALSE;
    const bool minimized = IsIconic(hwnd) != FALSE;
    const bool sizable = (style & WS_THICKFRAME) != 0;
    const bool can_min = (style & WS_MINIMIZEBOX) != 0;
    const bool can_max = (style & WS_MAXIMIZEBOX) != 0;

    set_item(menu, SC_RESTORE, maximized || minimized);
    set_item(menu, SC_MOVE, !maximized && !minimized);
    set_item(menu, SC_SIZE, sizable && !maximized && !minimized);
    set_item(menu, SC_MINIMIZE, can_min && !minimized);
    set_item(menu, SC_MAXIMIZE, can_max && !maximized);
    set_item(menu, SC_CLOSE, true);
    SetMenuDefaultItem(menu, SC_CLOSE, FALSE);
}

UINT show_system_menu(HWND hwnd, POINT screen_pt, const MenuTracker& tracker) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return 0;
    }
    HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (menu == nullptr) {
        return 0;
    }
    update_system_menu_state(hwnd, menu);

    const bool rtl = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY;
    flags |= (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0 || rtl) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    if (rtl) {
        flags |= TPM_LAYOUTRTL;
    }

    UINT command = 0;
    if (tracker) {
        command = tracker(hwnd, menu, flags, screen_pt);
    } else {
        command = static_cast<UINT>(TrackPopupMenuEx(menu, flags, screen_pt.x, screen_pt.y, hwnd, nullptr));
    }
    if (command != 0) {
        PostMessageW(hwnd, WM_SYSCOMMAND, command, 0);
    }
    return command;
}

std::wstring strip_menu_text(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\t') {
            break;
        }
        if (ch == L'&') {
            if (i + 1 < text.size() && text[i + 1] == L'&') {
                out.push_back(L'&');
                ++i;
            }
            continue;
        }
        out.push_back(ch);
    }
    // Some locales append "(&X)" accelerators: drop a trailing "(X)" left behind by the '&' removal.
    if (out.size() >= 3 && out.back() == L')' && out[out.size() - 3] == L'(') {
        out.resize(out.size() - 3);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) {
        out.pop_back();
    }
    return out;
}

std::wstring localized_caption_name(HWND hwnd, CaptionButton button, bool maximized) {
    UINT command = SC_CLOSE;
    const wchar_t* fallback = L"Close";
    switch (button) {
    case CaptionButton::Minimize:
        command = SC_MINIMIZE;
        fallback = L"Minimize";
        break;
    case CaptionButton::Maximize:
        command = maximized ? SC_RESTORE : SC_MAXIMIZE;
        fallback = maximized ? L"Restore" : L"Maximize";
        break;
    case CaptionButton::Close:
        break;
    }
    if (hwnd != nullptr && IsWindow(hwnd)) {
        if (HMENU menu = GetSystemMenu(hwnd, FALSE); menu != nullptr) {
            std::array<wchar_t, 128> buffer{};
            const int len = GetMenuStringW(menu, command, buffer.data(), static_cast<int>(buffer.size()), MF_BYCOMMAND);
            if (len > 0) {
                std::wstring name = strip_menu_text(std::wstring_view(buffer.data(), static_cast<std::size_t>(len)));
                if (!name.empty()) {
                    return name;
                }
            }
        }
    }
    return fallback;
}

SystemKeyAction classify_system_key(int key_event_kind, UINT virtual_key, bool was_key_down) noexcept {
    if (key_event_kind != kKeyEventKindSystemKeyDown || was_key_down) {
        return SystemKeyAction::None;
    }
    if (virtual_key == VK_SPACE) {
        return SystemKeyAction::SystemMenu;
    }
    if (virtual_key == VK_F4) {
        return SystemKeyAction::Close;
    }
    return SystemKeyAction::None;
}

RECT compute_maximized_client_rect(
    RECT proposed, RECT work_area, RECT monitor, int frame_x, int frame_y, unsigned auto_hide_edges) noexcept {
    RECT rc{proposed.left + frame_x, proposed.top + frame_y, proposed.right - frame_x, proposed.bottom - frame_y};
    RECT clamped{};
    if (IntersectRect(&clamped, &rc, &work_area) != FALSE) {
        rc = clamped;
    }
    if ((auto_hide_edges & kAutoHideTop) != 0 && rc.top <= monitor.top) {
        rc.top = monitor.top + kAutoHideTaskbarReveal;
    }
    if ((auto_hide_edges & kAutoHideBottom) != 0 && rc.bottom >= monitor.bottom) {
        rc.bottom = monitor.bottom - kAutoHideTaskbarReveal;
    }
    if ((auto_hide_edges & kAutoHideLeft) != 0 && rc.left <= monitor.left) {
        rc.left = monitor.left + kAutoHideTaskbarReveal;
    }
    if ((auto_hide_edges & kAutoHideRight) != 0 && rc.right >= monitor.right) {
        rc.right = monitor.right - kAutoHideTaskbarReveal;
    }
    return rc;
}

RECT compute_restored_client_rect(RECT proposed, int frame_x, int frame_y, bool keep_side_frames) noexcept {
    if (!keep_side_frames) {
        return proposed;
    }
    RECT rc{proposed.left + frame_x, proposed.top, proposed.right - frame_x, proposed.bottom - frame_y};
    if (rc.right < rc.left) {
        rc.right = rc.left;
    }
    if (rc.bottom < rc.top) {
        rc.bottom = rc.top;
    }
    return rc;
}

unsigned query_auto_hide_edges(HMONITOR monitor) noexcept {
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &mi) == FALSE) {
        return kAutoHideNone;
    }
    unsigned edges = kAutoHideNone;
    constexpr std::array<std::pair<UINT, unsigned>, 4> kEdges{{
        {ABE_LEFT, kAutoHideLeft},
        {ABE_TOP, kAutoHideTop},
        {ABE_RIGHT, kAutoHideRight},
        {ABE_BOTTOM, kAutoHideBottom},
    }};
    for (const auto& [edge, bit] : kEdges) {
        APPBARDATA abd{};
        abd.cbSize = sizeof(APPBARDATA);
        abd.uEdge = edge;
        abd.rc = mi.rcMonitor;
        if (SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &abd) != 0) {
            edges |= bit;
        }
    }
    return edges;
}

SIZE resize_frame_size(UINT dpi) noexcept {
    const int padded = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    return SIZE{
        GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + padded,
        GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + padded,
    };
}

int top_resize_band(UINT dpi) noexcept {
    return MulDiv(8, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

void adjust_headless_client_rect(HWND hwnd, RECT& rect, bool maximized, bool keep_side_frames, UINT dpi) noexcept {
    const SIZE frame = resize_frame_size(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi);
    if (!maximized) {
        rect = compute_restored_client_rect(rect, frame.cx, frame.cy, keep_side_frames);
        return;
    }
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr && hwnd != nullptr) {
        monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &mi) == FALSE) {
        rect = RECT{rect.left + frame.cx, rect.top + frame.cy, rect.right - frame.cx, rect.bottom - frame.cy};
        return;
    }
    // Only a monitor whose work area equals its full rect can host an auto-hide taskbar; skip the shell
    // round trip otherwise.
    const unsigned edges = EqualRect(&mi.rcWork, &mi.rcMonitor) != FALSE ? query_auto_hide_edges(monitor) : kAutoHideNone;
    rect = compute_maximized_client_rect(rect, mi.rcWork, mi.rcMonitor, frame.cx, frame.cy, edges);
}

} // namespace boson::titlebar
