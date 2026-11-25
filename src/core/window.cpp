#include <boson/core/window.hpp>

#include <boson/core/result.hpp>

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

namespace boson::core {

namespace {

constexpr wchar_t kWindowClassName[] = L"BosonMainWindowClass";

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
#else
constexpr DWORD kDwmwaUseImmersiveDarkMode = DWMWA_USE_IMMERSIVE_DARK_MODE;
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kDwmwaWindowCornerPreference = 33;
#else
constexpr DWORD kDwmwaWindowCornerPreference = DWMWA_WINDOW_CORNER_PREFERENCE;
#endif

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
constexpr DWORD kDwmwaSystemBackdropType = 38;
#else
constexpr DWORD kDwmwaSystemBackdropType = DWMWA_SYSTEMBACKDROP_TYPE;
#endif

enum DwmSystemBackdrop : DWORD {
    kDwmsbtAuto = 0,
    kDwmsbtNone = 1,
    kDwmsbtMainWindow = 2,      // Mica
    kDwmsbtTransientWindow = 3, // Acrylic
    kDwmsbtTabbedWindow = 4,    // Mica Alt
};

void set_dwm_blur_behind(HWND hwnd, bool enable) noexcept {
    DWM_BLURBEHIND bb{};
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = enable ? TRUE : FALSE;

    HRGN empty_rgn = nullptr;
    if (enable) {
        empty_rgn = CreateRectRgn(0, 0, -1, -1);
        if (empty_rgn != nullptr) {
            bb.dwFlags |= DWM_BB_BLURREGION;
            bb.hRgnBlur = empty_rgn;
        }
    }

    DwmEnableBlurBehindWindow(hwnd, &bb);
    if (empty_rgn != nullptr) {
        DeleteObject(empty_rgn);
    }
}

} // namespace

Window::Window(WindowConfig config)
    : m_config(std::move(config)) {
    m_config.opacity = std::clamp(m_config.opacity, 0.1f, 1.0f);
    m_saved_placement.length = sizeof(WINDOWPLACEMENT);
}

Window::~Window() {
    if (m_hwnd != nullptr && IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool Window::create(HINSTANCE instance, int show_cmd) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HINSTANCE h_inst = instance != nullptr ? instance : GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::wnd_proc;
    wc.hInstance = h_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            hr_check(HRESULT_FROM_WIN32(err), "RegisterClassExW");
            return false;
        }
    }

    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD ex_style = m_config.always_on_top ? WS_EX_TOPMOST : 0;

    m_hwnd = CreateWindowExW(
        ex_style,
        kWindowClassName,
        m_config.title.c_str(),
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        m_config.width,
        m_config.height,
        nullptr,
        nullptr,
        h_inst,
        this);

    if (m_hwnd == nullptr) {
        hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW");
        return false;
    }

    apply_dark_mode();
    apply_corner_preference();
    apply_backdrop();

    if (is_headless()) {
        apply_borderless_frame();
    }

    ShowWindow(m_hwnd, show_cmd);
    UpdateWindow(m_hwnd);
    return true;
}

int Window::run_message_loop() {
    MSG msg{};
    while (true) {
        const BOOL res = GetMessageW(&msg, nullptr, 0, 0);
        if (res <= 0) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void Window::post_invoke() const noexcept {
    if (m_hwnd != nullptr) {
        PostMessageW(m_hwnd, kWmBosonInvoke, 0, 0);
    }
}

void Window::close() const noexcept {
    if (m_hwnd != nullptr) {
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }
}

void Window::minimize() const noexcept {
    if (m_hwnd != nullptr) {
        ShowWindow(m_hwnd, SW_MINIMIZE);
    }
}

void Window::maximize_or_restore() const noexcept {
    if (m_hwnd != nullptr) {
        ShowWindow(m_hwnd, IsZoomed(m_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    }
}

void Window::start_drag() const noexcept {
    if (m_hwnd != nullptr) {
        ReleaseCapture();
        SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
}

bool Window::set_backdrop(BackdropType backdrop) noexcept {
    m_config.backdrop = backdrop;
    if (backdrop != BackdropType::Translucent) {
        m_config.opacity = 1.0f;
    }
    if (m_hwnd == nullptr) {
        return true;
    }
    apply_backdrop();
    return true;
}

bool Window::set_opacity(float opacity) noexcept {
    m_config.opacity = std::clamp(opacity, 0.1f, 1.0f);
    if (m_hwnd == nullptr) {
        return true;
    }

    const LONG_PTR ex_style = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (m_config.opacity < 0.999f || m_config.backdrop == BackdropType::Translucent) {
        if ((ex_style & WS_EX_LAYERED) == 0) {
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
        }
        const BYTE alpha = static_cast<BYTE>(m_config.opacity * 255.0f);
        return SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA) != FALSE;
    }

    if ((ex_style & WS_EX_LAYERED) != 0) {
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex_style & ~WS_EX_LAYERED);
    }
    return true;
}

bool Window::set_dark_mode(bool enabled) noexcept {
    m_config.dark_mode = enabled;
    if (m_hwnd == nullptr) {
        return true;
    }
    apply_dark_mode();
    apply_backdrop();
    return true;
}

bool Window::set_corner_preference(CornerPreference pref) noexcept {
    m_config.corner_preference = pref;
    if (m_hwnd == nullptr) {
        return true;
    }
    apply_corner_preference();
    return true;
}

void Window::set_always_on_top(bool enabled) noexcept {
    m_config.always_on_top = enabled;
    if (m_hwnd != nullptr) {
        SetWindowPos(
            m_hwnd,
            enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void Window::set_fullscreen(bool enabled) noexcept {
    if (m_hwnd == nullptr || m_is_fullscreen == enabled) {
        return;
    }

    if (enabled) {
        m_saved_style = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_STYLE));
        m_saved_placement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(m_hwnd, &m_saved_placement);

        MONITORINFO mi{};
        mi.cbSize = sizeof(MONITORINFO);
        if (GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongPtrW(m_hwnd, GWL_STYLE, static_cast<LONG_PTR>(m_saved_style & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(
                m_hwnd,
                HWND_TOP,
                mi.rcMonitor.left,
                mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            m_is_fullscreen = true;
        }
    } else {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, static_cast<LONG_PTR>(m_saved_style));
        SetWindowPlacement(m_hwnd, &m_saved_placement);
        SetWindowPos(
            m_hwnd,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        m_is_fullscreen = false;
    }
}

void Window::set_title(std::wstring_view title) noexcept {
    m_config.title = std::wstring(title);
    if (m_hwnd != nullptr) {
        SetWindowTextW(m_hwnd, m_config.title.c_str());
    }
}

RECT Window::client_rect() const noexcept {
    RECT rc{};
    if (m_hwnd != nullptr) {
        GetClientRect(m_hwnd, &rc);
    }
    return rc;
}

bool Window::is_maximized() const noexcept {
    return m_hwnd != nullptr && IsZoomed(m_hwnd) != FALSE;
}

bool Window::is_transparent_backdrop() const noexcept {
    return m_config.backdrop == BackdropType::Mica ||
           m_config.backdrop == BackdropType::MicaAlt ||
           m_config.backdrop == BackdropType::Acrylic ||
           m_config.backdrop == BackdropType::Transparent;
}

void Window::set_resize_callback(std::function<void(int width, int height)> cb) {
    m_on_resize = std::move(cb);
}

void Window::set_invoke_callback(std::function<void()> cb) {
    m_on_invoke = std::move(cb);
}

void Window::set_close_callback(std::function<void()> cb) {
    m_on_close = std::move(cb);
}

LRESULT CALLBACK Window::wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    Window* self = nullptr;

    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        self = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->m_hwnd = hwnd;
        }
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->handle_message(msg, w_param, l_param);
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

LRESULT Window::handle_message(UINT msg, WPARAM w_param, LPARAM l_param) {
    switch (msg) {
    case WM_ERASEBKGND: {
        if (is_transparent_backdrop()) {
            return 1;
        }
        HDC hdc = reinterpret_cast<HDC>(w_param);
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(m_config.dark_mode ? RGB(32, 32, 32) : RGB(243, 243, 243));
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        return 1;
    }

    case WM_NCACTIVATE:
        if (is_headless() && !m_is_fullscreen) {
            // Returning TRUE prevents DWM/DefWindowProc from painting native caption chrome on focus change.
            return TRUE;
        }
        break;

    case WM_NCCALCSIZE:
        if (is_headless() && !m_is_fullscreen && w_param == TRUE) {
            return handle_nccalcsize(w_param, l_param);
        }
        break;

    case WM_NCHITTEST:
        if (is_headless() && !m_is_fullscreen) {
            return handle_nchittest(l_param);
        }
        break;

    case WM_SIZE:
        if (m_on_resize && w_param != SIZE_MINIMIZED) {
            const int width = static_cast<int>(LOWORD(l_param));
            const int height = static_cast<int>(HIWORD(l_param));
            m_on_resize(width, height);
        }
        return 0;

    case kWmBosonInvoke:
        if (m_on_invoke) {
            m_on_invoke();
        }
        return 0;

    case WM_DESTROY:
        if (m_on_close) {
            m_on_close();
        }
        m_hwnd = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(m_hwnd, msg, w_param, l_param);
}

void Window::apply_dark_mode() const noexcept {
    if (m_hwnd == nullptr) {
        return;
    }
    const BOOL dark = m_config.dark_mode ? TRUE : FALSE;
    BOSON_HR_CHECK(DwmSetWindowAttribute(m_hwnd, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark)));
}

void Window::apply_corner_preference() const noexcept {
    if (m_hwnd == nullptr) {
        return;
    }
    DWORD pref = 0;
    switch (m_config.corner_preference) {
    case CornerPreference::Default:
        pref = 0;
        break;
    case CornerPreference::Square:
        pref = 1;
        break;
    case CornerPreference::Round:
        pref = 2;
        break;
    case CornerPreference::RoundSmall:
        pref = 3;
        break;
    }
    DwmSetWindowAttribute(m_hwnd, kDwmwaWindowCornerPreference, &pref, sizeof(pref));
}

void Window::apply_backdrop() noexcept {
    if (m_hwnd == nullptr) {
        return;
    }

    const LONG_PTR ex_style = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (m_config.backdrop == BackdropType::Translucent) {
        set_dwm_blur_behind(m_hwnd, false);
        if ((ex_style & WS_EX_LAYERED) == 0) {
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
        }
        const float effective = m_config.opacity >= 0.999f ? 0.88f : m_config.opacity;
        const BYTE alpha = static_cast<BYTE>(std::clamp(effective, 0.1f, 1.0f) * 255.0f);
        SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);
    } else {
        // DWM system backdrops and blur-behind must not be combined with WS_EX_LAYERED.
        if ((ex_style & WS_EX_LAYERED) != 0) {
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex_style & ~WS_EX_LAYERED);
        }
        set_dwm_blur_behind(m_hwnd, m_config.backdrop == BackdropType::Transparent);
    }

    const MARGINS margins = is_transparent_backdrop()
        ? MARGINS{-1, -1, -1, -1}
        : (is_headless() ? MARGINS{0, 0, 0, 1} : MARGINS{0, 0, 0, 0});
    BOSON_HR_CHECK(DwmExtendFrameIntoClientArea(m_hwnd, &margins));

    DWORD dwm_backdrop = kDwmsbtNone;
    switch (m_config.backdrop) {
    case BackdropType::None:
    case BackdropType::Transparent:
    case BackdropType::Translucent:
        dwm_backdrop = kDwmsbtNone;
        break;
    case BackdropType::Mica:
        dwm_backdrop = kDwmsbtMainWindow;
        break;
    case BackdropType::Acrylic:
        dwm_backdrop = kDwmsbtTransientWindow;
        break;
    case BackdropType::MicaAlt:
        dwm_backdrop = kDwmsbtTabbedWindow;
        break;
    }

    DwmSetWindowAttribute(m_hwnd, kDwmwaSystemBackdropType, &dwm_backdrop, sizeof(dwm_backdrop));
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

void Window::apply_borderless_frame() const noexcept {
    if (m_hwnd == nullptr) {
        return;
    }
    SetWindowPos(
        m_hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT Window::handle_nccalcsize(WPARAM w_param, LPARAM l_param) const noexcept {
    if (w_param == FALSE) {
        return 0;
    }

    if (IsZoomed(m_hwnd)) {
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(l_param);
        const UINT dpi = GetDpiForWindow(m_hwnd);
        const int padded = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int frame_x = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + padded;
        const int frame_y = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + padded;

        params->rgrc[0].left += frame_x;
        params->rgrc[0].top += frame_y;
        params->rgrc[0].right -= frame_x;
        params->rgrc[0].bottom -= frame_y;
    }

    return 0;
}

LRESULT Window::handle_nchittest(LPARAM l_param) const noexcept {
    const POINT pt{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    RECT rc{};
    GetWindowRect(m_hwnd, &rc);

    const UINT dpi = GetDpiForWindow(m_hwnd);
    const int border = MulDiv(8, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);

    const bool on_left = pt.x >= rc.left && pt.x < rc.left + border;
    const bool on_right = pt.x < rc.right && pt.x >= rc.right - border;
    const bool on_top = pt.y >= rc.top && pt.y < rc.top + border;
    const bool on_bottom = pt.y < rc.bottom && pt.y >= rc.bottom - border;

    if (!IsZoomed(m_hwnd)) {
        if (on_top && on_left) return HTTOPLEFT;
        if (on_top && on_right) return HTTOPRIGHT;
        if (on_bottom && on_left) return HTBOTTOMLEFT;
        if (on_bottom && on_right) return HTBOTTOMRIGHT;
        if (on_left) return HTLEFT;
        if (on_right) return HTRIGHT;
        if (on_top) return HTTOP;
        if (on_bottom) return HTBOTTOM;
    }

    if (m_config.titlebar_height > 0) {
        const int titlebar = MulDiv(m_config.titlebar_height, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        if (pt.y >= rc.top && pt.y < rc.top + titlebar) {
            return HTCAPTION;
        }
    }

    return HTCLIENT;
}

} // namespace boson::core
