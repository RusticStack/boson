#include <boson/core/window.hpp>

#include <boson/core/result.hpp>

#include <dwmapi.h>
#include <windowsx.h>

namespace boson::core {

namespace {

constexpr wchar_t kWindowClassName[] = L"BosonMainWindowClass";

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
#else
constexpr DWORD kDwmwaUseImmersiveDarkMode = DWMWA_USE_IMMERSIVE_DARK_MODE;
#endif

} // namespace

Window::Window(WindowConfig config)
    : m_config(std::move(config)) {}

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
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            hr_check(HRESULT_FROM_WIN32(err), "RegisterClassExW");
            return false;
        }
    }

    const DWORD style = m_config.borderless
        ? (WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)
        : WS_OVERLAPPEDWINDOW;

    m_hwnd = CreateWindowExW(
        0,
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
    if (m_config.borderless) {
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

RECT Window::client_rect() const noexcept {
    RECT rc{};
    if (m_hwnd != nullptr) {
        GetClientRect(m_hwnd, &rc);
    }
    return rc;
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
    case WM_NCCALCSIZE:
        if (m_config.borderless && w_param == TRUE) {
            return handle_nccalcsize(w_param, l_param);
        }
        break;

    case WM_NCHITTEST:
        if (m_config.borderless) {
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

void Window::apply_borderless_frame() const noexcept {
    if (m_hwnd == nullptr) {
        return;
    }
    const MARGINS margins{0, 0, 0, 1};
    BOSON_HR_CHECK(DwmExtendFrameIntoClientArea(m_hwnd, &margins));
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
    const LRESULT def_hit = DefWindowProcW(m_hwnd, WM_NCHITTEST, 0, l_param);
    if (def_hit != HTCLIENT) {
        return def_hit;
    }

    const POINT pt{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    RECT rc{};
    GetWindowRect(m_hwnd, &rc);

    const UINT dpi = GetDpiForWindow(m_hwnd);
    const int border = MulDiv(8, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    const int titlebar = MulDiv(m_config.titlebar_height, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);

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

    if (pt.y >= rc.top && pt.y < rc.top + titlebar) {
        return HTCAPTION;
    }

    return HTCLIENT;
}

} // namespace boson::core
