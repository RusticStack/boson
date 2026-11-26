#include <boson/core/window.hpp>

#include <boson/core/result.hpp>
#include <boson/titlebar/caption_accessibility.hpp>
#include <boson/titlebar/window_chrome.hpp>

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

namespace boson::core {

namespace {

constexpr wchar_t kWindowClassName[] = L"BosonMainWindowClass";
constexpr wchar_t kOverlayClassName[] = L"BosonCaptionOverlay";
constexpr wchar_t kCaptionOverlayProp[] = L"BosonCaptionOverlay";

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

bool register_class(HINSTANCE instance, const wchar_t* name, WNDPROC proc, HCURSOR cursor) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = cursor;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = name;
    if (RegisterClassExW(&wc) == 0) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            hr_check(HRESULT_FROM_WIN32(err), "RegisterClassExW");
            return false;
        }
    }
    return true;
}

} // namespace

Window::Window(WindowConfig config)
    : m_config(std::move(config)) {
    m_config.opacity = std::clamp(m_config.opacity, 0.1f, 1.0f);
    m_saved_placement.length = sizeof(WINDOWPLACEMENT);
}

Window::~Window() {
    destroy();
}

void Window::destroy() noexcept {
    if (m_hwnd != nullptr && IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
    }
    m_hwnd = nullptr;
}

bool Window::create(HINSTANCE instance, int show_cmd) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HINSTANCE h_inst = instance != nullptr ? instance : GetModuleHandleW(nullptr);
    if (!register_class(h_inst, kWindowClassName, &Window::wnd_proc, LoadCursorW(nullptr, IDC_ARROW))) {
        return false;
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
        m_config.owner,
        nullptr,
        h_inst,
        this);

    if (m_hwnd == nullptr) {
        hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW");
        return false;
    }

    // Sizes are DIPs: rescale for the monitor the window actually opened on.
    const UINT window_dpi = dpi();
    if (window_dpi != USER_DEFAULT_SCREEN_DPI) {
        SetWindowPos(
            m_hwnd,
            nullptr,
            0,
            0,
            dip_to_px(m_config.width),
            dip_to_px(m_config.height),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (m_config.center) {
        center();
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

void Window::show(int show_cmd) const noexcept {
    if (m_hwnd != nullptr) {
        ShowWindow(m_hwnd, show_cmd);
    }
}

void Window::hide() const noexcept {
    if (m_hwnd != nullptr) {
        ShowWindow(m_hwnd, SW_HIDE);
    }
}

void Window::focus() const noexcept {
    if (m_hwnd == nullptr) {
        return;
    }
    if (IsIconic(m_hwnd)) {
        ShowWindow(m_hwnd, SW_RESTORE);
    } else if (!IsWindowVisible(m_hwnd)) {
        ShowWindow(m_hwnd, SW_SHOW);
    }
    SetForegroundWindow(m_hwnd);
}

void Window::center() const noexcept {
    if (m_hwnd == nullptr) {
        return;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        return;
    }
    RECT rc{};
    GetWindowRect(m_hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const RECT& work = mi.rcWork;
    SetWindowPos(
        m_hwnd,
        nullptr,
        work.left + ((work.right - work.left) - width) / 2,
        work.top + ((work.bottom - work.top) - height) / 2,
        0,
        0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

RECT Window::bounds() const noexcept {
    RECT rc{};
    if (m_hwnd != nullptr) {
        GetWindowRect(m_hwnd, &rc);
    }
    return rc;
}

void Window::set_bounds(RECT screen_px) const noexcept {
    if (m_hwnd != nullptr) {
        SetWindowPos(
            m_hwnd,
            nullptr,
            screen_px.left,
            screen_px.top,
            screen_px.right - screen_px.left,
            screen_px.bottom - screen_px.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void Window::set_min_size(int min_width_dip, int min_height_dip) noexcept {
    m_config.min_width = std::max(0, min_width_dip);
    m_config.min_height = std::max(0, min_height_dip);
    if (m_hwnd == nullptr) {
        return;
    }
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    if (rc.right < dip_to_px(m_config.min_width) || rc.bottom < dip_to_px(m_config.min_height)) {
        // Re-run WM_GETMINMAXINFO by nudging the size.
        RECT wr = bounds();
        SetWindowPos(m_hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void Window::set_maximize_button_rect(std::optional<RECT> client_px) {
    m_max_button_rect = client_px;
    if (m_hwnd == nullptr) {
        return;
    }
    if (!client_px.has_value()) {
        if (m_caption_overlay != nullptr) {
            ShowWindow(m_caption_overlay, SW_HIDE);
        }
        set_max_button_hover(false);
        return;
    }

    if (m_caption_overlay == nullptr) {
        const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(m_hwnd, GWLP_HINSTANCE));
        if (!register_class(instance, kOverlayClassName, &Window::overlay_proc, LoadCursorW(nullptr, IDC_ARROW))) {
            return;
        }
        // A layered child (Windows 8+, needs the supportedOS manifest) at alpha 1 is invisible but still
        // takes hit testing above the WebView's child HWNDs.
        m_caption_overlay = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_NOACTIVATE,
            kOverlayClassName,
            L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 0, 0,
            m_hwnd,
            nullptr,
            instance,
            nullptr);
        if (m_caption_overlay != nullptr) {
            SetLayeredWindowAttributes(m_caption_overlay, 0, 1, LWA_ALPHA);
        } else {
            hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW(layered caption overlay); add a supportedOS manifest");
            m_caption_overlay = CreateWindowExW(
                WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                kOverlayClassName,
                L"",
                WS_CHILD | WS_CLIPSIBLINGS,
                0, 0, 0, 0,
                m_hwnd,
                nullptr,
                instance,
                nullptr);
        }
        if (m_caption_overlay == nullptr) {
            hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW(BosonCaptionOverlay)");
            return;
        }
        SetWindowLongPtrW(m_caption_overlay, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetPropW(m_hwnd, kCaptionOverlayProp, m_caption_overlay);
    }

    const RECT& rc = *client_px;
    SetWindowPos(
        m_caption_overlay,
        HWND_TOP,
        rc.left,
        rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK Window::overlay_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    // The overlay sits above the WebView's cross-process child HWNDs (HTTRANSPARENT would fall through to
    // them, not to the parent), so it answers HTMAXBUTTON itself and forwards the button logic to its Window.
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCHITTEST:
        return HTMAXBUTTON;
    case WM_NCMOUSEMOVE:
        if (self != nullptr) {
            self->set_max_button_hover(true);
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        break; // DefWindowProc still sees HTMAXBUTTON hover
    case WM_NCMOUSELEAVE:
        if (self != nullptr) {
            self->set_max_button_hover(false);
        }
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (self != nullptr) {
            self->m_max_pressed = true;
            WindowEvent event{.kind = WindowEventKind::MaximizeButtonPress, .dpi = self->dpi(), .flag = true};
            self->emit(event);
        }
        return 0; // no DefWindowProc button-tracking loop
    case WM_NCLBUTTONUP:
        if (self != nullptr && self->m_max_pressed) {
            self->m_max_pressed = false;
            WindowEvent event{.kind = WindowEventKind::MaximizeButtonPress, .dpi = self->dpi(), .flag = false};
            self->emit(event);
            self->maximize_or_restore();
        }
        return 0;
    case WM_GETOBJECT:
        if (self != nullptr && self->m_hwnd != nullptr) {
            // UI Automation: the overlay is a Button named "Maximize" / "Restore" with the Invoke pattern.
            if (const auto result = titlebar::html_caption_overlay_get_object(hwnd, self->m_hwnd, w_param, l_param)) {
                return *result;
            }
        }
        break;
    case WM_DESTROY:
        titlebar::html_caption_overlay_destroyed(hwnd);
        break;
    case WM_NCRBUTTONUP:
        if (self != nullptr && self->m_hwnd != nullptr) {
            // Right-click on a caption button shows the window menu, like a native caption.
            SendMessageW(self->m_hwnd, WM_NCRBUTTONUP, HTCAPTION, l_param);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
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

bool Window::is_minimized() const noexcept {
    return m_hwnd != nullptr && IsIconic(m_hwnd) != FALSE;
}

bool Window::is_visible() const noexcept {
    return m_hwnd != nullptr && IsWindowVisible(m_hwnd) != FALSE;
}

UINT Window::dpi() const noexcept {
    const UINT value = m_hwnd != nullptr ? GetDpiForWindow(m_hwnd) : GetDpiForSystem();
    return value == 0 ? USER_DEFAULT_SCREEN_DPI : value;
}

int Window::dip_to_px(int dip) const noexcept {
    return MulDiv(dip, static_cast<int>(dpi()), USER_DEFAULT_SCREEN_DPI);
}

int Window::px_to_dip(int px) const noexcept {
    return MulDiv(px, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi()));
}

// ---- Events ----------------------------------------------------------------------------------------------

Window::ListenerId Window::on(WindowEventKind kind, EventListener listener) {
    const ListenerId id = m_next_listener_id++;
    m_listeners.push_back(Listener{.id = id, .kind = kind, .fn = std::move(listener)});
    return id;
}

bool Window::off(ListenerId id) noexcept {
    for (auto& listener : m_listeners) {
        if (listener.id == id) {
            listener.fn = nullptr;
            m_needs_compaction = true;
            compact_listeners();
            return true;
        }
    }
    return false;
}

Window::ListenerId Window::add_message_hook(MessageHook hook) {
    const ListenerId id = m_next_listener_id++;
    m_hooks.push_back(Hook{.id = id, .fn = std::move(hook)});
    return id;
}

bool Window::remove_message_hook(ListenerId id) noexcept {
    for (auto& hook : m_hooks) {
        if (hook.id == id) {
            hook.fn = nullptr;
            m_needs_compaction = true;
            compact_listeners();
            return true;
        }
    }
    return false;
}

void Window::compact_listeners() noexcept {
    if (m_dispatch_depth != 0 || !m_needs_compaction) {
        return;
    }
    std::erase_if(m_listeners, [](const Listener& l) { return !l.fn; });
    std::erase_if(m_hooks, [](const Hook& h) { return !h.fn; });
    m_needs_compaction = false;
}

void Window::emit(WindowEvent& event) {
    ++m_dispatch_depth;
    // Index loop: listeners added during dispatch are appended and may run in this pass, never invalidated.
    for (std::size_t i = 0; i < m_listeners.size(); ++i) {
        if (m_listeners[i].kind == event.kind && m_listeners[i].fn) {
            EventListener fn = m_listeners[i].fn;
            fn(event);
        }
    }
    --m_dispatch_depth;
    compact_listeners();
}

void Window::emit(WindowEventKind kind) {
    WindowEvent event{.kind = kind, .dpi = dpi()};
    emit(event);
}

std::optional<LRESULT> Window::run_message_hooks(UINT msg, WPARAM w_param, LPARAM l_param) {
    if (m_hooks.empty()) {
        return std::nullopt;
    }
    std::optional<LRESULT> result;
    ++m_dispatch_depth;
    for (std::size_t i = 0; i < m_hooks.size() && !result.has_value(); ++i) {
        if (m_hooks[i].fn) {
            MessageHook fn = m_hooks[i].fn;
            result = fn(msg, w_param, l_param);
        }
    }
    --m_dispatch_depth;
    compact_listeners();
    return result;
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
        const LRESULT result = self->handle_message(msg, w_param, l_param);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->m_hwnd = nullptr;
            self->m_caption_overlay = nullptr;
        }
        return result;
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

LRESULT Window::handle_message(UINT msg, WPARAM w_param, LPARAM l_param) {
    if (msg != WM_NCHITTEST) {
        if (const auto hooked = run_message_hooks(msg, w_param, l_param); hooked.has_value()) {
            return *hooked;
        }
    }

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
        return handle_nchittest(l_param);

    case WM_NCMOUSEMOVE:
        if (w_param == HTMAXBUTTON && m_max_button_rect.has_value()) {
            set_max_button_hover(true);
            return 0;
        }
        set_max_button_hover(false);
        break;

    case WM_NCMOUSELEAVE:
        set_max_button_hover(false);
        break;

    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (w_param == HTMAXBUTTON && m_max_button_rect.has_value()) {
            m_max_pressed = true;
            WindowEvent event{.kind = WindowEventKind::MaximizeButtonPress, .dpi = dpi(), .flag = true};
            emit(event);
            return 0;
        }
        break;

    case WM_NCLBUTTONUP:
        if (w_param == HTMAXBUTTON && m_max_button_rect.has_value()) {
            if (m_max_pressed) {
                m_max_pressed = false;
                WindowEvent event{.kind = WindowEventKind::MaximizeButtonPress, .dpi = dpi(), .flag = false};
                emit(event);
                maximize_or_restore();
            }
            return 0;
        }
        break;

    case WM_GETMINMAXINFO:
        handle_min_max_info(reinterpret_cast<MINMAXINFO*>(l_param));
        return 0;

    case WM_SIZE: {
        const int width = static_cast<int>(LOWORD(l_param));
        const int height = static_cast<int>(HIWORD(l_param));
        const WPARAM previous = m_size_state;
        if (w_param == SIZE_MINIMIZED) {
            if (previous != SIZE_MINIMIZED) {
                m_size_state = SIZE_MINIMIZED;
                emit(WindowEventKind::Minimize);
            }
            return 0;
        }
        if (w_param == SIZE_MAXIMIZED || w_param == SIZE_RESTORED) {
            m_size_state = w_param;
            if (w_param == SIZE_MAXIMIZED && previous != SIZE_MAXIMIZED) {
                emit(WindowEventKind::Maximize);
            } else if (w_param == SIZE_RESTORED && previous != SIZE_RESTORED) {
                emit(WindowEventKind::Restore);
            }
        }
        if (m_on_resize) {
            m_on_resize(width, height);
        }
        WindowEvent event{.kind = WindowEventKind::Resize, .width = width, .height = height, .dpi = dpi()};
        emit(event);
        return 0;
    }

    case WM_MOVE: {
        WindowEvent event{
            .kind = WindowEventKind::Move,
            .x = static_cast<int>(static_cast<short>(LOWORD(l_param))),
            .y = static_cast<int>(static_cast<short>(HIWORD(l_param))),
            .dpi = dpi(),
        };
        emit(event);
        return 0;
    }

    case WM_ACTIVATE:
        emit(LOWORD(w_param) == WA_INACTIVE ? WindowEventKind::Blur : WindowEventKind::Focus);
        break;

    case WM_ENTERSIZEMOVE:
        emit(WindowEventKind::EnterSizeMove);
        break;

    case WM_EXITSIZEMOVE:
        emit(WindowEventKind::ExitSizeMove);
        break;

    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(l_param);
        if (suggested != nullptr) {
            SetWindowPos(
                m_hwnd,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        WindowEvent event{.kind = WindowEventKind::DpiChanged, .dpi = HIWORD(w_param)};
        if (suggested != nullptr) {
            event.x = suggested->left;
            event.y = suggested->top;
            event.width = suggested->right - suggested->left;
            event.height = suggested->bottom - suggested->top;
        }
        emit(event);
        return 0;
    }

    case kWmBosonInvoke:
        if (m_on_invoke) {
            m_on_invoke();
        }
        return 0;

    case WM_CLOSE: {
        WindowEvent event{.kind = WindowEventKind::CloseRequested, .dpi = dpi()};
        emit(event);
        if (!event.cancel) {
            DestroyWindow(m_hwnd);
        }
        return 0;
    }

    case WM_DESTROY:
        emit(WindowEventKind::Destroyed);
        if (m_on_close) {
            m_on_close();
        }
        RemovePropW(m_hwnd, kCaptionOverlayProp);
        if (m_config.quit_on_destroy) {
            PostQuitMessage(0);
        }
        return 0;

    default:
        break;
    }

    return DefWindowProcW(m_hwnd, msg, w_param, l_param);
}

void Window::handle_min_max_info(MINMAXINFO* info) const noexcept {
    if (info == nullptr || (m_config.min_width <= 0 && m_config.min_height <= 0)) {
        return;
    }
    const UINT window_dpi = dpi();
    RECT rc{0, 0, MulDiv(m_config.min_width, static_cast<int>(window_dpi), USER_DEFAULT_SCREEN_DPI),
            MulDiv(m_config.min_height, static_cast<int>(window_dpi), USER_DEFAULT_SCREEN_DPI)};
    if (is_headless() && uses_native_resize_borders()) {
        const SIZE frame = titlebar::resize_frame_size(window_dpi);
        rc.right += 2 * frame.cx;
        rc.bottom += frame.cy;
    } else if (!is_headless()) {
        AdjustWindowRectExForDpi(
            &rc,
            static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_STYLE)),
            FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE)),
            window_dpi);
    }
    if (m_config.min_width > 0) {
        info->ptMinTrackSize.x = rc.right - rc.left;
    }
    if (m_config.min_height > 0) {
        info->ptMinTrackSize.y = rc.bottom - rc.top;
    }
}

void Window::set_max_button_hover(bool hovered) {
    if (!hovered && m_max_pressed) {
        m_max_pressed = false;
        WindowEvent press{.kind = WindowEventKind::MaximizeButtonPress, .dpi = dpi(), .flag = false};
        emit(press);
    }
    if (m_max_hovered == hovered) {
        return;
    }
    m_max_hovered = hovered;
    if (hovered && m_hwnd != nullptr) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(TRACKMOUSEEVENT);
        tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
        tme.hwndTrack = m_hwnd;
        TrackMouseEvent(&tme);
    }
    WindowEvent event{.kind = WindowEventKind::MaximizeButtonHover, .dpi = dpi(), .flag = hovered};
    emit(event);
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

    // Maximized: strip the off-screen frame and clamp to the monitor work area (leaving a reveal strip on
    // auto-hide taskbar edges). Restored: keep side/bottom resize borders non-client when requested.
    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(l_param);
    titlebar::adjust_headless_client_rect(
        m_hwnd, params->rgrc[0], IsZoomed(m_hwnd) != FALSE, uses_native_resize_borders(), GetDpiForWindow(m_hwnd));
    return 0;
}

bool Window::uses_native_resize_borders() const noexcept {
    return m_config.native_resize_borders && m_hwnd != nullptr &&
           (GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) == 0;
}

LRESULT Window::resize_border_hit(POINT pt) const noexcept {
    if (IsZoomed(m_hwnd)) {
        return HTNOWHERE;
    }
    RECT rc{};
    GetWindowRect(m_hwnd, &rc);
    const int border = dip_to_px(8);

    const bool on_left = pt.x >= rc.left && pt.x < rc.left + border;
    const bool on_right = pt.x < rc.right && pt.x >= rc.right - border;
    const bool on_top = pt.y >= rc.top && pt.y < rc.top + border;
    const bool on_bottom = pt.y < rc.bottom && pt.y >= rc.bottom - border;

    if (on_top && on_left) return HTTOPLEFT;
    if (on_top && on_right) return HTTOPRIGHT;
    if (on_bottom && on_left) return HTBOTTOMLEFT;
    if (on_bottom && on_right) return HTBOTTOMRIGHT;
    if (on_left) return HTLEFT;
    if (on_right) return HTRIGHT;
    if (on_top) return HTTOP;
    if (on_bottom) return HTBOTTOM;
    return HTNOWHERE;
}

bool Window::point_in_max_button(POINT screen_pt) const noexcept {
    if (!m_max_button_rect.has_value()) {
        return false;
    }
    POINT client = screen_pt;
    ScreenToClient(m_hwnd, &client);
    return PtInRect(&*m_max_button_rect, client) != FALSE;
}

LRESULT Window::handle_nchittest(LPARAM l_param) {
    const POINT pt{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    const bool headless = is_headless() && !m_is_fullscreen;

    if (headless) {
        if (const LRESULT border = resize_border_hit(pt); border != HTNOWHERE) {
            return border;
        }
    } else {
        const LRESULT native = DefWindowProcW(m_hwnd, WM_NCHITTEST, 0, l_param);
        if (native != HTCLIENT) {
            return native;
        }
    }

    if (point_in_max_button(pt)) {
        return HTMAXBUTTON;
    }

    if (const auto hooked = run_message_hooks(WM_NCHITTEST, 0, l_param); hooked.has_value()) {
        return *hooked;
    }

    if (headless && m_config.titlebar_height > 0) {
        RECT rc{};
        GetWindowRect(m_hwnd, &rc);
        if (pt.y >= rc.top && pt.y < rc.top + dip_to_px(m_config.titlebar_height)) {
            return HTCAPTION;
        }
    }

    return HTCLIENT;
}

} // namespace boson::core
