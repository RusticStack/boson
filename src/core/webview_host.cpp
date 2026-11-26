#include <boson/core/webview_host.hpp>

#include <boson/core/result.hpp>

#include <windowsx.h>

#include <algorithm>

namespace boson::core {

namespace {

constexpr WPARAM kAnyButton = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;

bool is_button_down(UINT msg) noexcept {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

bool is_button_up(UINT msg) noexcept {
    return msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP;
}

} // namespace

WebViewHost::WebViewHost(Window& window, std::shared_ptr<Environment> env, HostingMode mode)
    : m_window(window)
    , m_env(std::move(env))
    , m_mode(mode) {
    if (m_mode == HostingMode::Composition) {
        m_composition = CompositionHost::create(m_window.hwnd());
        if (!m_composition) {
            hr_check(E_NOINTERFACE, "WebViewHost: DirectComposition unavailable, falling back to windowed hosting");
            m_mode = HostingMode::Windowed;
        }
    }

    m_listeners.push_back(m_window.on(WindowEventKind::Resize, [this](WindowEvent&) { relayout(); }));
    m_listeners.push_back(m_window.on(WindowEventKind::DpiChanged, [this](WindowEvent&) { relayout(); }));
    m_listeners.push_back(m_window.on(WindowEventKind::Move, [this](WindowEvent&) {
        for (const auto& webview : m_webviews) {
            webview->notify_parent_window_position_changed();
        }
    }));
    m_listeners.push_back(m_window.on(WindowEventKind::Minimize, [this](WindowEvent&) {
        m_minimized = true;
        for (const auto& webview : m_webviews) {
            webview->set_parent_minimized(true);
        }
    }));
    const auto restore = [this](WindowEvent&) {
        if (!m_minimized) {
            return;
        }
        m_minimized = false;
        for (const auto& webview : m_webviews) {
            webview->set_parent_minimized(false);
        }
    };
    m_listeners.push_back(m_window.on(WindowEventKind::Restore, restore));
    m_listeners.push_back(m_window.on(WindowEventKind::Maximize, restore));

    m_hook = m_window.add_message_hook([this](UINT msg, WPARAM w_param, LPARAM l_param) {
        return on_message(msg, w_param, l_param);
    });
}

WebViewHost::~WebViewHost() {
    m_sleep_timer.stop();
    m_graveyard_timer.stop();
    for (const auto id : m_listeners) {
        m_window.off(id);
    }
    if (m_hook != 0) {
        m_window.remove_message_hook(m_hook);
    }
    if (m_capture != nullptr && GetCapture() == m_window.hwnd()) {
        ReleaseCapture();
    }
    m_hover = nullptr;
    m_capture = nullptr;
    m_graveyard.clear();
    m_webviews.clear();
}

WebView& WebViewHost::create(WebViewConfig config, WebViewCallbacks callbacks) {
    config.hosting = m_mode;
    if (m_mode == HostingMode::Composition) {
        config.composition = m_composition;
    }
    auto webview = std::make_unique<WebView>(m_env, std::move(config));
    WebView& ref = *webview;
    m_webviews.push_back(std::move(webview));

    if (m_mode == HostingMode::Composition) {
        WebView* raw = &ref;
        ref.set_cursor_changed_callback([this, raw]() {
            if (m_hover == raw) {
                POINT pt{};
                GetCursorPos(&pt);
                if (WindowFromPoint(pt) == m_window.hwnd()) {
                    SetCursor(raw->cursor());
                }
            }
        });
    }

    auto lookup = callbacks.asset_lookup ? std::move(callbacks.asset_lookup) : m_default_lookup;
    const LayoutContext context = layout_context();
    ref.set_bounds(context.client_px);
    ref.initialize(m_window.hwnd(), std::move(lookup), std::move(callbacks.on_message), std::move(callbacks.on_ready));
    if (m_minimized) {
        ref.set_parent_minimized(true);
    }
    relayout();
    return ref;
}

void WebViewHost::destroy(WebView& webview) {
    const auto it = std::ranges::find_if(m_webviews, [&webview](const auto& p) { return p.get() == &webview; });
    if (it == m_webviews.end()) {
        return;
    }
    if (m_hover == &webview) {
        m_hover = nullptr;
    }
    if (m_capture == &webview) {
        m_capture = nullptr;
        ReleaseCapture();
    }
    forget_sleeping(webview.id());
    std::erase(m_sleep_exempt, webview.id());
    if (m_on_destroy) {
        m_on_destroy(webview);
    }
    webview.set_visible(false);
    m_graveyard.push_back(std::move(*it));
    m_webviews.erase(it);
    m_graveyard_timer.start(std::chrono::milliseconds{1}, [this]() { flush_graveyard(); });
}

void WebViewHost::flush_graveyard() {
    std::vector<std::unique_ptr<WebView>> dead;
    dead.swap(m_graveyard);
    for (auto& webview : dead) {
        webview->close();
    }
}

WebView* WebViewHost::find(std::uint64_t id) const noexcept {
    for (const auto& webview : m_webviews) {
        if (webview->id() == id) {
            return webview.get();
        }
    }
    return nullptr;
}

WebView* WebViewHost::webview_at(POINT client_px) const noexcept {
    const auto hit = [client_px](const WebView* webview) {
        const RECT bounds = webview->bounds();
        return webview->is_visible() && !webview->is_closed() && PtInRect(&bounds, client_px) != FALSE;
    };
    if (m_composition) {
        return m_composition->find_top_down(hit);
    }
    for (auto it = m_webviews.rbegin(); it != m_webviews.rend(); ++it) {
        if (hit(it->get())) {
            return it->get();
        }
    }
    return nullptr;
}

LayoutContext WebViewHost::layout_context() const noexcept {
    LayoutContext context{};
    context.client_px = m_window.client_rect();
    context.dpi = m_window.dpi();
    context.minimized = m_window.is_minimized();
    return context;
}

void WebViewHost::set_layout(LayoutFn layout) {
    m_layout = std::move(layout);
    relayout();
}

void WebViewHost::relayout() {
    const LayoutContext context = layout_context();
    if (context.minimized || context.width() <= 0 || context.height() <= 0) {
        return;
    }
    if (m_layout) {
        m_layout(*this, context);
    } else {
        for (const auto& webview : m_webviews) {
            webview->set_bounds(context.client_px);
        }
    }
    if (m_composition) {
        m_composition->commit();
    }
}

// ---- Sleep policy ----------------------------------------------------------------------------------------

void WebViewHost::set_sleep_policy(std::optional<SleepPolicy> policy) {
    m_sleep_policy = policy;
    m_sleep_timer.stop();
    if (m_sleep_policy.has_value()) {
        m_sleep_timer.start(m_sleep_policy->check_interval, [this]() { check_sleep_now(); }, true);
    }
}

void WebViewHost::set_sleep_exempt(WebView& webview, bool exempt) {
    std::erase(m_sleep_exempt, webview.id());
    if (exempt) {
        m_sleep_exempt.push_back(webview.id());
    }
}

bool WebViewHost::is_sleeping(const WebView& webview) const noexcept {
    // A WebView that was shown since it was put to sleep is awake (set_visible(true) resumed it), even before
    // the next check_sleep_now() cleans the entry up; its hidden_since() moved on when it was hidden again.
    return !webview.is_visible() && std::ranges::any_of(m_sleeping, [&webview](const SleepingEntry& entry) {
        return entry.id == webview.id() && entry.hidden_since == webview.hidden_since();
    });
}

void WebViewHost::forget_sleeping(std::uint64_t id) noexcept {
    std::erase_if(m_sleeping, [id](const SleepingEntry& entry) { return entry.id == id; });
}

void WebViewHost::check_sleep_now() {
    if (!m_sleep_policy.has_value()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    for (const auto& ptr : m_webviews) {
        WebView& webview = *ptr;
        const std::uint64_t id = webview.id();
        if (webview.is_visible()) {
            forget_sleeping(id); // set_visible(true) already resumed it and restored the memory target
            continue;
        }
        if (!is_sleeping(webview)) {
            forget_sleeping(id); // woken and hidden again since it was put to sleep: eligible again
        }
        if (!webview.is_ready() || is_sleeping(webview) ||
            std::ranges::find(m_sleep_exempt, id) != m_sleep_exempt.end() ||
            webview.last_known_playing_audio() ||
            now - webview.hidden_since() < m_sleep_policy->hidden_timeout) {
            continue;
        }
        m_sleeping.push_back(SleepingEntry{.id = id, .hidden_since = webview.hidden_since()});
        if (m_sleep_policy->lower_memory_target) {
            webview.set_memory_usage_target(MemoryUsageTarget::Low);
        }
        if (m_sleep_policy->suspend) {
            webview.try_suspend([this, id](HRESULT hr, bool suspended) {
                WebView* target = find(id);
                if (target == nullptr) {
                    return;
                }
                if (FAILED(hr) || !suspended) {
                    hr_check(FAILED(hr) ? hr : E_FAIL, "SleepPolicy: TrySuspend did not suspend");
                }
                if (m_on_slept) {
                    m_on_slept(*target);
                }
            });
        } else if (m_on_slept) {
            m_on_slept(webview);
        }
    }
}

// ---- Composition input -----------------------------------------------------------------------------------

std::optional<LRESULT> WebViewHost::on_message(UINT msg, WPARAM w_param, LPARAM l_param) {
    if (msg == WM_NCHITTEST) {
        return on_nc_hit_test(l_param);
    }
    if (!m_composition) {
        return std::nullopt;
    }
    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSELEAVE) {
        return on_mouse(msg, w_param, l_param);
    }
    switch (msg) {
    case WM_SETCURSOR:
        if (LOWORD(l_param) == HTCLIENT && m_hover != nullptr) {
            SetCursor(m_hover->cursor());
            return TRUE;
        }
        break;
    case WM_CAPTURECHANGED:
        m_capture = nullptr;
        break;
    case WM_SETFOCUS:
        if (m_hover != nullptr) {
            m_hover->focus();
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

void WebViewHost::set_hover(WebView* webview) {
    if (m_hover == webview) {
        return;
    }
    if (m_hover != nullptr) {
        m_hover->send_mouse_input(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, POINT{});
    }
    m_hover = webview;
    if (m_hover != nullptr) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(TRACKMOUSEEVENT);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = m_window.hwnd();
        TrackMouseEvent(&tme);
    }
}

std::optional<LRESULT> WebViewHost::on_mouse(UINT msg, WPARAM w_param, LPARAM l_param) {
    if (msg == WM_MOUSELEAVE) {
        if (m_capture == nullptr) {
            set_hover(nullptr);
        }
        return 0;
    }

    POINT pt{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        ScreenToClient(m_window.hwnd(), &pt); // wheel coordinates are in screen space
    }

    WebView* target = m_capture != nullptr ? m_capture : webview_at(pt);
    if (m_capture == nullptr) {
        set_hover(target);
    }
    if (target == nullptr) {
        return std::nullopt;
    }

    if (is_button_down(msg) && m_capture == nullptr) {
        SetCapture(m_window.hwnd());
        m_capture = target;
    }

    UINT32 mouse_data = 0;
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        mouse_data = static_cast<UINT32>(static_cast<std::int32_t>(GET_WHEEL_DELTA_WPARAM(w_param)));
    } else if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP || msg == WM_XBUTTONDBLCLK) {
        mouse_data = GET_XBUTTON_WPARAM(w_param);
    }

    const RECT bounds = target->bounds();
    const POINT local{pt.x - bounds.left, pt.y - bounds.top};
    target->send_mouse_input(
        static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(msg),
        static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(GET_KEYSTATE_WPARAM(w_param)),
        mouse_data,
        local);

    if (is_button_up(msg) && (GET_KEYSTATE_WPARAM(w_param) & kAnyButton) == 0 && m_capture != nullptr) {
        m_capture = nullptr;
        ReleaseCapture();
        set_hover(webview_at(pt));
    }
    return 0;
}

std::optional<LRESULT> WebViewHost::on_nc_hit_test(LPARAM l_param) const {
    if (!m_composition) {
        return std::nullopt; // windowed WebViews answer CSS app-region themselves
    }
    POINT pt{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    ScreenToClient(m_window.hwnd(), &pt);
    WebView* target = webview_at(pt);
    if (target == nullptr) {
        return std::nullopt;
    }
    const RECT bounds = target->bounds();
    const auto kind = target->non_client_region_at(POINT{pt.x - bounds.left, pt.y - bounds.top});
    if (!kind.has_value()) {
        return std::nullopt;
    }
    switch (*kind) {
    case COREWEBVIEW2_NON_CLIENT_REGION_KIND_CAPTION: return HTCAPTION;
    case COREWEBVIEW2_NON_CLIENT_REGION_KIND_MINIMIZE: return HTMINBUTTON;
    case COREWEBVIEW2_NON_CLIENT_REGION_KIND_MAXIMIZE: return HTMAXBUTTON;
    case COREWEBVIEW2_NON_CLIENT_REGION_KIND_CLOSE: return HTCLOSE;
    default: break;
    }
    return std::nullopt;
}

} // namespace boson::core
