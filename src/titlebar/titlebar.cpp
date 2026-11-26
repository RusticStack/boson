#include <boson/titlebar/titlebar.hpp>

#include <boson/core/result.hpp>

#include <windowsx.h>

#include <algorithm>

namespace boson::titlebar {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kGripClassName[] = L"BosonTopResizeGrip";
constexpr wchar_t kKeepOnTopProp[] = L"BosonKeepOnTop";

double to_dip(LONG px, UINT dpi) noexcept {
    return static_cast<double>(px) * USER_DEFAULT_SCREEN_DPI / static_cast<double>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi);
}

} // namespace

Titlebar::Titlebar(core::Window& window, TitlebarOptions options)
    : m_window(window)
    , m_options(options)
    , m_theme(window)
    , m_buttons(std::make_unique<CaptionButtons>(window, m_theme))
    , m_dpi(window.dpi()) {
    m_system_key_msg = RegisterWindowMessageW(L"Boson.Titlebar.SystemKey");
    m_hook = m_window.add_message_hook([this](UINT msg, WPARAM w_param, LPARAM l_param) {
        return on_message(msg, w_param, l_param);
    });
    using core::WindowEventKind;
    const auto layout_changed = [this](core::WindowEvent&) {
        update_grip();
        if (!m_buttons->is_shown()) {
            notify_overlay();
        }
    };
    for (const auto kind : {WindowEventKind::Resize, WindowEventKind::Maximize, WindowEventKind::Restore}) {
        m_listeners.push_back(m_window.on(kind, layout_changed));
    }
    m_listeners.push_back(m_window.on(WindowEventKind::DpiChanged, [this](core::WindowEvent& e) {
        m_dpi = e.dpi == 0 ? USER_DEFAULT_SCREEN_DPI : e.dpi;
        update_grip();
    }));
    for (const auto kind : {WindowEventKind::Focus, WindowEventKind::Blur}) {
        m_listeners.push_back(m_window.on(kind, [this](core::WindowEvent&) { notify_overlay(); }));
    }
    m_listeners.push_back(m_window.on(WindowEventKind::Destroyed, [this](core::WindowEvent&) { m_grip = nullptr; }));
    m_buttons->set_layout_listener([this]() { notify_overlay(); });
    update_grip();
}

Titlebar::~Titlebar() {
    for (auto& attached : m_webviews) {
        if (attached.controller) {
            attached.controller->remove_AcceleratorKeyPressed(attached.token);
        }
    }
    m_webviews.clear();
    if (m_hook != 0) {
        m_window.remove_message_hook(m_hook);
    }
    for (const auto id : m_listeners) {
        m_window.off(id);
    }
    m_buttons.reset();
    if (m_grip != nullptr && IsWindow(m_grip)) {
        DestroyWindow(m_grip);
    }
    m_grip = nullptr;
}

bool Titlebar::set_overlay(std::optional<TitlebarOverlayOptions> options) {
    if (!options.has_value()) {
        m_buttons->hide();
        return true;
    }
    return m_buttons->show(*options);
}

TitlebarOverlayInfo Titlebar::make_overlay_info(
    const CaptionLayout& layout, int client_width, bool visible, bool maximized, bool active) noexcept {
    TitlebarOverlayInfo info{};
    info.dpi = layout.dpi;
    info.rtl = layout.rtl;
    info.maximized = maximized;
    info.active = active;
    info.visible = visible;
    if (!visible) {
        info.width = to_dip(client_width, layout.dpi);
        return info;
    }
    const RECT& area = layout.titlebar_area;
    info.x = to_dip(area.left, layout.dpi);
    info.y = to_dip(area.top, layout.dpi);
    info.width = to_dip(area.right - area.left, layout.dpi);
    info.height = to_dip(area.bottom - area.top, layout.dpi);
    const RECT& buttons = layout.overlay_visual;
    info.buttons_x = to_dip(buttons.left, layout.dpi);
    info.buttons_y = to_dip(buttons.top, layout.dpi);
    info.buttons_width = to_dip(buttons.right - buttons.left, layout.dpi);
    info.buttons_height = to_dip(buttons.bottom - buttons.top, layout.dpi);
    return info;
}

TitlebarOverlayInfo Titlebar::overlay_info() const {
    const RECT client = m_window.client_rect();
    const bool active = m_window.hwnd() != nullptr && GetActiveWindow() == m_window.hwnd();
    if (m_buttons->is_shown()) {
        return make_overlay_info(m_buttons->layout(), client.right - client.left, m_buttons->is_visible(),
            m_window.is_maximized(), active);
    }
    const CaptionLayout layout = compute_caption_layout(client.right - client.left, m_dpi, 32, 46, false, false);
    return make_overlay_info(layout, client.right - client.left, false, m_window.is_maximized(), active);
}

void Titlebar::notify_overlay() {
    if (m_overlay_listeners.empty()) {
        return;
    }
    const TitlebarOverlayInfo info = overlay_info();
    const auto listeners = m_overlay_listeners;
    for (const auto& listener : listeners) {
        if (listener) {
            listener(info);
        }
    }
}

int Titlebar::caption_height_px() const noexcept {
    if (m_buttons->is_shown()) {
        return m_buttons->layout().height;
    }
    const int dip = m_window.config().titlebar_height > 0 ? m_window.config().titlebar_height : 32;
    return MulDiv(dip, static_cast<int>(m_dpi), USER_DEFAULT_SCREEN_DPI);
}

UINT Titlebar::show_system_menu(POINT screen_pt) {
    return titlebar::show_system_menu(m_window.hwnd(), screen_pt, m_tracker);
}

UINT Titlebar::show_system_menu_at_caption() {
    if (m_window.hwnd() == nullptr) {
        return 0;
    }
    POINT pt{0, caption_height_px()};
    ClientToScreen(m_window.hwnd(), &pt); // (0, y) is the visual right edge of a mirrored (RTL) window
    return show_system_menu(pt);
}

bool Titlebar::handle_system_key(SystemKeyAction action) {
    switch (action) {
    case SystemKeyAction::SystemMenu:
        show_system_menu_at_caption();
        return true;
    case SystemKeyAction::Close:
        m_window.close();
        return true;
    case SystemKeyAction::None:
        break;
    }
    return false;
}

std::optional<LRESULT> Titlebar::on_message(UINT msg, WPARAM w_param, LPARAM l_param) {
    if (m_system_key_msg != 0 && msg == m_system_key_msg) {
        handle_system_key(static_cast<SystemKeyAction>(w_param));
        return 0;
    }
    const bool headless = m_window.is_headless() && !m_window.is_fullscreen();
    switch (msg) {
    case WM_NCRBUTTONDOWN:
        if (headless && m_options.system_menu_on_right_click && w_param == HTCAPTION) {
            return 0;
        }
        break;
    case WM_NCRBUTTONUP:
        if (headless && m_options.system_menu_on_right_click && w_param == HTCAPTION) {
            show_system_menu(POINT{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)});
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if (headless && m_options.system_keys && (w_param & 0xFFF0) == SC_KEYMENU && l_param == VK_SPACE) {
            show_system_menu_at_caption();
            return 0;
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

bool Titlebar::attach_webview(core::WebView& webview) {
    if (!m_options.system_keys) {
        return false;
    }
    ICoreWebView2Controller* controller = webview.controller();
    if (controller == nullptr) {
        return false;
    }
    const auto existing = std::find_if(m_webviews.begin(), m_webviews.end(),
        [&webview](const Attached& a) { return a.id == webview.id(); });
    if (existing != m_webviews.end()) {
        if (existing->controller.Get() == controller) {
            return true;
        }
        existing->controller->remove_AcceleratorKeyPressed(existing->token); // controller was recreated
        m_webviews.erase(existing);
    }
    Attached attached{.id = webview.id(), .controller = controller};
    const HWND hwnd = m_window.hwnd();
    const UINT message = m_system_key_msg;
    const HRESULT hr = controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [hwnd, message](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                UINT key = 0;
                COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
                if (FAILED(args->get_KeyEventKind(&kind)) || FAILED(args->get_VirtualKey(&key)) ||
                    FAILED(args->get_PhysicalKeyStatus(&status))) {
                    return S_OK;
                }
                const SystemKeyAction action = classify_system_key(static_cast<int>(kind), key, status.WasKeyDown != FALSE);
                if (action != SystemKeyAction::None) {
                    args->put_Handled(TRUE);
                    // Leave the WebView2 callback before running a modal menu loop.
                    PostMessageW(hwnd, message, static_cast<WPARAM>(action), 0);
                }
                return S_OK;
            })
            .Get(),
        &attached.token);
    if (!BOSON_HR_CHECK(hr)) {
        return false;
    }
    m_webviews.push_back(std::move(attached));
    return true;
}

void Titlebar::detach_webview(core::WebView& webview) noexcept {
    const auto it = std::find_if(m_webviews.begin(), m_webviews.end(),
        [&webview](const Attached& a) { return a.id == webview.id(); });
    if (it == m_webviews.end()) {
        return;
    }
    if (it->controller) {
        it->controller->remove_AcceleratorKeyPressed(it->token);
    }
    m_webviews.erase(it);
}

// ---- Top resize grip --------------------------------------------------------------------------------------

void Titlebar::update_grip() {
    const HWND parent = m_window.hwnd();
    if (!m_options.top_resize_grip || parent == nullptr || !m_window.is_headless()) {
        return;
    }
    if (m_grip == nullptr) {
        const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = &Titlebar::grip_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_SIZENS);
        wc.lpszClassName = kGripClassName;
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "RegisterClassExW(BosonTopResizeGrip)");
            return;
        }
        // Alpha-1 layered child: invisible, but it takes hit testing above the WebView's child HWNDs.
        m_grip = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kGripClassName, L"", WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 0, 0, parent, nullptr, instance, this);
        if (m_grip == nullptr) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW(BosonTopResizeGrip)");
            return;
        }
        SetLayeredWindowAttributes(m_grip, 0, 1, LWA_ALPHA);
        SetPropW(m_grip, kKeepOnTopProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
        SetWindowPos(m_grip, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (m_buttons->hwnd() != nullptr) {
            SetWindowPos(m_buttons->hwnd(), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    const bool visible = !m_window.is_maximized() && !m_window.is_minimized() && !has_fullscreen_style(parent);
    const RECT client = m_window.client_rect();
    const int band = std::max(1, GetSystemMetricsForDpi(SM_CYSIZEFRAME, m_dpi));
    SetWindowPos(m_grip, nullptr, 0, 0, client.right - client.left, band,
        SWP_NOZORDER | SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

LRESULT CALLBACK Titlebar::grip_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Titlebar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const HWND parent = GetParent(hwnd);
    switch (msg) {
    case WM_NCHITTEST: {
        if (self == nullptr || parent == nullptr) {
            return HTTRANSPARENT;
        }
        RECT rc{};
        GetWindowRect(parent, &rc);
        const POINT pt{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        const int corner = resize_frame_size(self->m_dpi).cx * 2;
        if (pt.x < rc.left + corner) return HTTOPLEFT;
        if (pt.x >= rc.right - corner) return HTTOPRIGHT;
        return HTTOP;
    }
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (parent != nullptr && is_sizing_hit(static_cast<LRESULT>(w_param))) {
            ReleaseCapture();
            SendMessageW(parent, msg, w_param, l_param);
            return 0;
        }
        break;
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONUP:
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        RemovePropW(hwnd, kKeepOnTopProp);
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

} // namespace boson::titlebar
