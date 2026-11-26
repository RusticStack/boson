#include <boson/app/window_manager.hpp>

#include <boson/app/lifecycle.hpp>
#include <boson/core/result.hpp>
#include <boson/ipc/request_context.hpp>

#include "../desktop/desktop_util.hpp"

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <exception>
#include <mutex>
#include <utility>

namespace boson::app {

namespace {

constexpr UINT kWmInvoke = WM_APP + 0x40;
constexpr UINT kWmReap = WM_APP + 0x41;

bool is_label_char(char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
           ch == ':' || ch == '/' || ch == '.';
}

std::wstring label_script(std::string_view label) {
    // Labels are restricted to [A-Za-z0-9_-:/.], so they need no escaping inside a JS string literal.
    std::wstring script = L"window.__BOSON_WINDOW__ = Object.freeze({ label: \"";
    script += core::utf8_to_wide(label);
    script += L"\" });";
    return script;
}

RECT centered_on(RECT anchor, RECT window, HWND anchor_hwnd) noexcept {
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (GetMonitorInfoW(MonitorFromWindow(anchor_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT& work = mi.rcWork;
        x = std::clamp(x, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - width));
        y = std::clamp(y, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - height));
    }
    return RECT{x, y, x + width, y + height};
}

} // namespace

std::string_view to_string(WindowManagerEventKind kind) noexcept {
    switch (kind) {
    case WindowManagerEventKind::Created: return "created";
    case WindowManagerEventKind::Closed: return "closed";
    case WindowManagerEventKind::Focus: return "focus";
    case WindowManagerEventKind::Blur: return "blur";
    case WindowManagerEventKind::Moved: return "moved";
    case WindowManagerEventKind::Resized: return "resized";
    case WindowManagerEventKind::Maximized: return "maximized";
    case WindowManagerEventKind::Minimized: return "minimized";
    case WindowManagerEventKind::Restored: return "restored";
    case WindowManagerEventKind::Shown: return "shown";
    case WindowManagerEventKind::Hidden: return "hidden";
    case WindowManagerEventKind::CloseRequested: return "close-requested";
    }
    return "unknown";
}

// ---- ManagedWindow ---------------------------------------------------------------------------------------

ManagedWindow::ManagedWindow(PassKey, WindowManager& manager, ManagedWindowConfig config)
    : m_manager(manager),
      m_config(std::move(config)),
      m_window(m_config.window) {}

ManagedWindow::~ManagedWindow() {
    m_close_timer.stop();
    for (auto& waiter : m_close_waiters) {
        waiter(S_OK, m_destroyed);
    }
    m_close_waiters.clear();
    m_state.reset();
    m_host.reset();
    for (const auto id : m_listeners) {
        m_window.off(id);
    }
}

core::WebView* ManagedWindow::main_webview() const noexcept {
    if (!m_host || m_host->webviews().empty()) {
        return nullptr;
    }
    return m_host->webviews().front().get();
}

core::WebView& ManagedWindow::add_webview(core::WebViewConfig config, core::WebViewCallbacks callbacks) {
    if (!m_host) {
        m_host = std::make_unique<core::WebViewHost>(m_window, m_manager.environment(), m_config.hosting);
        if (m_manager.m_options.asset_lookup) {
            m_host->set_default_asset_lookup(m_manager.m_options.asset_lookup);
        }
        m_host->set_on_destroy([this](core::WebView& webview) {
            if (ipc::Bridge* bridge = m_manager.m_options.bridge; bridge != nullptr) {
                bridge->detach(webview);
            }
            m_manager.unregister_webview(webview.id());
        });
    }
    if (m_config.transparent) {
        config.transparent_background = true;
    }
    if (m_manager.m_options.inject_label_script) {
        config.scripts_on_document_created.push_back(label_script(m_config.label));
    }
    core::WebView& webview = m_host->create(std::move(config), std::move(callbacks));
    m_manager.register_webview(*this, webview);
    return webview;
}

void ManagedWindow::remove_webview(core::WebView& webview) {
    if (m_host) {
        m_host->destroy(webview);
    }
}

// ---- WindowManager ---------------------------------------------------------------------------------------

WindowManager::WindowManager(WindowManagerOptions options)
    : m_options(std::move(options)) {
    m_message_window = std::make_unique<desktop::detail::HiddenWindow>(
        [this](UINT msg, WPARAM, LPARAM) -> std::optional<LRESULT> {
            if (msg == kWmInvoke) {
                if (m_on_invoke) {
                    m_on_invoke();
                }
                return 0;
            }
            if (msg == kWmReap) {
                reap();
                return 0;
            }
            return std::nullopt;
        },
        true);
    m_message_hwnd = m_message_window->hwnd();
}

WindowManager::~WindowManager() {
    // The Bridge may already be gone (it is normally declared after the manager): never touch it here.
    m_options.bridge = nullptr;
    m_suppress_last_window = true;
    for (auto& window : m_windows) {
        window->m_force_close = true;
        if (window->hwnd() != nullptr) {
            window->m_window.destroy();
        }
    }
    m_windows.clear();
    for (const auto& entry : m_disabled_owners) {
        if (IsWindow(entry.owner)) {
            EnableWindow(entry.owner, TRUE);
        }
    }
    m_disabled_owners.clear();
    m_taskbar_owner.reset();
    m_message_hwnd = nullptr;
    m_message_window.reset();
}

bool WindowManager::is_valid_label(std::string_view label) noexcept {
    return !label.empty() && label.size() <= 64 && std::ranges::all_of(label, is_label_char);
}

const std::shared_ptr<core::Environment>& WindowManager::environment() {
    if (!m_options.environment) {
        m_options.environment = core::Environment::create(m_options.environment_config);
    }
    return m_options.environment;
}

void WindowManager::set_bridge(ipc::Bridge* bridge) {
    if (bridge == m_options.bridge) {
        return;
    }
    if (m_options.bridge != nullptr) {
        for (const auto& window : m_windows) {
            if (window->m_host) {
                for (const auto& webview : window->m_host->webviews()) {
                    m_options.bridge->detach(*webview);
                }
            }
        }
    }
    m_options.bridge = bridge;
    if (bridge == nullptr) {
        return;
    }
    for (const auto& window : m_windows) {
        if (window->m_destroyed || !window->m_host || !window->m_config.attach_bridge) {
            continue;
        }
        for (const auto& webview : window->m_host->webviews()) {
            ipc::BridgeAttachOptions opts = m_options.attach_options ? m_options.attach_options(*window, *webview) : ipc::BridgeAttachOptions{};
            opts.window_label = window->label();
            bridge->attach(*webview, std::move(opts));
        }
    }
}

std::string WindowManager::resolve_url(std::string_view url) const {
    if (url.empty()) {
        url = "index.html";
    }
    if (url.find("://") != std::string_view::npos || url.starts_with("about:") || url.starts_with("data:")) {
        return std::string(url);
    }
    while (url.starts_with('/')) {
        url.remove_prefix(1);
    }
    std::string out;
    if (m_options.dev_url.has_value() && !m_options.dev_url->empty()) {
        std::string_view base{*m_options.dev_url};
        while (base.ends_with('/')) {
            base.remove_suffix(1);
        }
        out.assign(base);
    } else {
        out = "boson://app";
    }
    out.push_back('/');
    out.append(url);
    return out;
}

HWND WindowManager::taskbar_owner() {
    if (!m_taskbar_owner) {
        m_taskbar_owner = std::make_unique<desktop::detail::HiddenWindow>(desktop::detail::HiddenWindow::Handler{}, false);
    }
    return m_taskbar_owner->hwnd();
}

WindowResult WindowManager::create(ManagedWindowConfig config) {
    if (m_quitting) {
        return std::unexpected(std::string("the application is quitting"));
    }
    if (!is_valid_label(config.label)) {
        return std::unexpected("invalid window label '" + config.label + "' (1-64 chars of A-Z a-z 0-9 - _ : / .)");
    }
    if (find(config.label) != nullptr) {
        return std::unexpected("a window labelled '" + config.label + "' already exists");
    }
    HWND owner = nullptr;
    if (!config.parent.empty()) {
        const ManagedWindow* parent = find(config.parent);
        if (parent == nullptr || parent->hwnd() == nullptr) {
            return std::unexpected("unknown parent window '" + config.parent + "'");
        }
        owner = parent->hwnd();
    } else if (config.modal) {
        return std::unexpected(std::string("a modal window needs a parent"));
    }
    if (owner == nullptr && config.skip_taskbar) {
        owner = taskbar_owner();
    }
    config.window.owner = owner;
    config.window.quit_on_destroy = false;
    if (config.transparent) {
        config.window.backdrop = core::BackdropType::Transparent;
    }

    auto created = std::make_unique<ManagedWindow>(ManagedWindow::PassKey{}, *this, std::move(config));
    ManagedWindow& window = *created;
    window.m_owner = owner;
    if (!window.m_window.create(m_options.instance, SW_HIDE)) {
        return std::unexpected(std::string("CreateWindowExW failed"));
    }
    m_windows.push_back(std::move(created));
    wire(window);

    if (window.m_config.persist_state) {
        desktop::WindowStateOptions state{};
        state.app_name = m_options.app_name.empty() ? std::wstring(L"Boson") : m_options.app_name;
        state.window_id = core::utf8_to_wide(window.label());
        if (!m_options.state_directory.empty()) {
            state.file = m_options.state_directory / desktop::default_window_state_path(L"_", state.window_id).filename();
        }
        state.runner = m_options.runner;
        window.m_state = std::make_unique<desktop::WindowStateKeeper>(window.m_window, std::move(state));
    }

    place_new_window(window, window.is_modal() ? owner : nullptr);
    if (window.is_modal()) {
        auto it = std::ranges::find(m_disabled_owners, owner, &OwnerDisable::owner);
        if (it == m_disabled_owners.end()) {
            m_disabled_owners.push_back(OwnerDisable{.owner = owner, .count = 0});
            it = std::prev(m_disabled_owners.end());
        }
        if (it->count++ == 0) {
            EnableWindow(owner, FALSE);
        }
        window.m_disabled_owner = true;
    }

    if (window.m_config.webview.has_value()) {
        core::WebViewConfig webview_config = *window.m_config.webview;
        window.add_webview(std::move(webview_config));
    }

    emit(WindowManagerEventKind::Created, window);
    if (window.m_config.visible) {
        show(window.label(), window.m_config.focus);
    }
    return &window;
}

void WindowManager::place_new_window(ManagedWindow& window, HWND center_owner) {
    const ManagedWindowConfig& cfg = window.m_config;
    const RECT current = window.m_window.bounds();
    const bool explicit_position = cfg.x.has_value() && cfg.y.has_value();
    if (center_owner != nullptr && (cfg.center || !explicit_position)) {
        RECT owner_rect{};
        GetWindowRect(center_owner, &owner_rect);
        window.m_window.set_bounds(centered_on(owner_rect, current, center_owner));
        return;
    }
    if (cfg.center) {
        if (window.m_owner != nullptr && window.m_owner != (m_taskbar_owner ? m_taskbar_owner->hwnd() : nullptr)) {
            RECT owner_rect{};
            GetWindowRect(window.m_owner, &owner_rect);
            window.m_window.set_bounds(centered_on(owner_rect, current, window.m_owner));
        } else {
            window.m_window.center();
        }
        return;
    }
    if (explicit_position) {
        const int width = current.right - current.left;
        const int height = current.bottom - current.top;
        window.m_window.set_bounds(RECT{*cfg.x, *cfg.y, *cfg.x + width, *cfg.y + height});
    }
}

void WindowManager::wire(ManagedWindow& window) {
    using Kind = core::WindowEventKind;
    ManagedWindow* w = &window;
    const auto forward = [this, w](Kind kind, WindowManagerEventKind out) {
        w->m_listeners.push_back(w->m_window.on(kind, [this, w, out](core::WindowEvent&) {
            if (!w->m_destroyed) {
                emit(out, *w);
            }
        }));
    };
    forward(Kind::Focus, WindowManagerEventKind::Focus);
    forward(Kind::Blur, WindowManagerEventKind::Blur);
    forward(Kind::Move, WindowManagerEventKind::Moved);
    forward(Kind::Resize, WindowManagerEventKind::Resized);
    forward(Kind::Maximize, WindowManagerEventKind::Maximized);
    forward(Kind::Minimize, WindowManagerEventKind::Minimized);
    forward(Kind::Restore, WindowManagerEventKind::Restored);
    w->m_listeners.push_back(w->m_window.on(Kind::CloseRequested, [this, w](core::WindowEvent& e) { on_close_message(*w, e); }));
    w->m_listeners.push_back(w->m_window.on(Kind::Destroyed, [this, w](core::WindowEvent&) { on_destroyed(*w); }));
}

void WindowManager::emit(WindowManagerEventKind kind, const ManagedWindow& window, std::uint64_t request_id) {
    if (m_handlers.empty()) {
        return;
    }
    WindowManagerEvent event{
        .kind = kind,
        .label = window.label(),
        .bounds = window.hwnd() != nullptr ? window.m_window.bounds() : RECT{},
        .dpi = window.hwnd() != nullptr ? window.m_window.dpi() : USER_DEFAULT_SCREEN_DPI,
        .close_request_id = request_id,
    };
    ++m_dispatch_depth;
    for (std::size_t i = 0; i < m_handlers.size(); ++i) {
        if (m_handlers[i].event) {
            EventListener fn = m_handlers[i].event;
            fn(event);
        }
    }
    --m_dispatch_depth;
    if (m_dispatch_depth == 0) {
        std::erase_if(m_handlers, [](const Handler& h) { return !h.event && !h.close; });
    }
}

WindowManager::ListenerId WindowManager::on_close_requested(CloseRequestHandler handler) {
    const ListenerId id = m_next_id++;
    m_handlers.push_back(Handler{.id = id, .close = std::move(handler), .event = {}});
    return id;
}

WindowManager::ListenerId WindowManager::on_event(EventListener listener) {
    const ListenerId id = m_next_id++;
    m_handlers.push_back(Handler{.id = id, .close = {}, .event = std::move(listener)});
    return id;
}

bool WindowManager::off(ListenerId id) noexcept {
    for (auto& handler : m_handlers) {
        if (handler.id == id) {
            handler.close = nullptr;
            handler.event = nullptr;
            if (m_dispatch_depth == 0) {
                std::erase_if(m_handlers, [](const Handler& h) { return !h.event && !h.close; });
            }
            return true;
        }
    }
    return false;
}

void WindowManager::complete_waiters(ManagedWindow& window, bool closed) {
    std::vector<exec::Completion<bool>> waiters;
    waiters.swap(window.m_close_waiters);
    for (auto& waiter : waiters) {
        waiter(S_OK, closed);
    }
}

void WindowManager::on_close_message(ManagedWindow& window, core::WindowEvent& event) {
    if (window.m_force_close) {
        release_owner(window);
        return; // let WM_CLOSE destroy the window
    }
    event.cancel = true;
    if (window.m_pending_close.has_value()) {
        return; // already waiting for an answer
    }

    CloseRequest request(window.label(), m_next_close_id++, m_quitting);
    ++m_dispatch_depth;
    for (std::size_t i = 0; i < m_handlers.size(); ++i) {
        if (m_handlers[i].close) {
            CloseRequestHandler fn = m_handlers[i].close;
            try {
                fn(request);
            } catch (const std::exception& ex) {
                core::hr_check(E_FAIL, std::string("WindowManager: close handler threw: ") + ex.what());
            } catch (...) {
                core::hr_check(E_FAIL, "WindowManager: close handler threw");
            }
        }
    }
    --m_dispatch_depth;
    if (m_dispatch_depth == 0) {
        std::erase_if(m_handlers, [](const Handler& h) { return !h.event && !h.close; });
    }

    if (request.m_prevented && !m_quitting) {
        complete_waiters(window, false);
        return;
    }
    int outstanding = m_quitting ? 0 : request.m_deferrals;
    const bool ask_page = window.m_close_guard && !m_quitting;
    if (ask_page) {
        ++outstanding;
    }
    if (outstanding > 0) {
        window.m_pending_close = ManagedWindow::PendingClose{.id = request.id(), .outstanding = outstanding};
        if (window.m_config.close_confirm_timeout.count() > 0) {
            const std::uint64_t id = request.id();
            ManagedWindow* w = &window;
            window.m_close_timer.start(window.m_config.close_confirm_timeout, [this, w, id]() {
                if (w->m_pending_close.has_value() && w->m_pending_close->id == id) {
                    w->m_pending_close.reset();
                    finish_close(*w, true);
                }
            });
        }
        if (ask_page) {
            emit(WindowManagerEventKind::CloseRequested, window, request.id());
        }
        return;
    }

    if (window.m_config.hide_on_close && !m_quitting) {
        hide(window.label());
        complete_waiters(window, false);
        return;
    }
    // Re-enable the owner before the window goes away, so activation returns to it (not to another app).
    release_owner(window);
    window.m_force_close = true;
    event.cancel = false;
}

void WindowManager::finish_close(ManagedWindow& window, bool allow_hide) {
    window.m_close_timer.stop();
    window.m_pending_close.reset();
    if (allow_hide && window.m_config.hide_on_close && !m_quitting) {
        hide(window.label());
        complete_waiters(window, false);
        return;
    }
    window.m_force_close = true;
    release_owner(window);
    window.m_window.destroy();
}

void WindowManager::cancel_close(ManagedWindow& window) {
    window.m_close_timer.stop();
    window.m_pending_close.reset();
    complete_waiters(window, false);
}

bool WindowManager::resolve_close(std::string_view label, std::uint64_t request_id, bool allow) {
    ManagedWindow* window = find(label);
    if (window == nullptr || !window->m_pending_close.has_value() || window->m_pending_close->id != request_id) {
        return false;
    }
    if (!allow) {
        cancel_close(*window);
        return true;
    }
    if (--window->m_pending_close->outstanding <= 0) {
        finish_close(*window, true);
    }
    return true;
}

bool WindowManager::set_close_guard(std::string_view label, bool enabled) {
    ManagedWindow* window = find(label);
    if (window == nullptr) {
        return false;
    }
    window->m_close_guard = enabled;
    return true;
}

bool WindowManager::set_skip_taskbar(std::string_view label, bool enabled) {
    ManagedWindow* window = find(label);
    if (window == nullptr || window->hwnd() == nullptr) {
        return false;
    }
    Microsoft::WRL::ComPtr<ITaskbarList> taskbar;
    if (!core::hr_check(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar)),
                        "WindowManager: CoCreateInstance(TaskbarList)") ||
        !core::hr_check(taskbar->HrInit(), "WindowManager: ITaskbarList::HrInit")) {
        return false;
    }
    const HRESULT hr = enabled ? taskbar->DeleteTab(window->hwnd()) : taskbar->AddTab(window->hwnd());
    return core::hr_check(hr, enabled ? "WindowManager: ITaskbarList::DeleteTab" : "WindowManager: ITaskbarList::AddTab");
}

void WindowManager::release_owner(ManagedWindow& window) noexcept {
    if (!window.m_disabled_owner) {
        return;
    }
    window.m_disabled_owner = false;
    const auto it = std::ranges::find(m_disabled_owners, window.m_owner, &OwnerDisable::owner);
    if (it == m_disabled_owners.end()) {
        return;
    }
    if (--it->count <= 0) {
        if (IsWindow(it->owner)) {
            EnableWindow(it->owner, TRUE);
        }
        m_disabled_owners.erase(it);
    }
}

void WindowManager::on_destroyed(ManagedWindow& window) {
    if (window.m_destroyed) {
        return;
    }
    window.m_destroyed = true;
    window.m_close_timer.stop();
    window.m_pending_close.reset();
    release_owner(window);
    if (window.m_host) {
        for (const auto& webview : window.m_host->webviews()) {
            if (m_options.bridge != nullptr) {
                m_options.bridge->detach(*webview);
            }
            unregister_webview(webview->id());
        }
    }
    complete_waiters(window, true);
    if (!m_reap_posted && m_message_hwnd != nullptr) {
        m_reap_posted = PostMessageW(m_message_hwnd, kWmReap, 0, 0) != FALSE;
    }
}

void WindowManager::reap() {
    m_reap_posted = false;
    std::vector<std::unique_ptr<ManagedWindow>> dead;
    for (auto it = m_windows.begin(); it != m_windows.end();) {
        if ((*it)->m_destroyed) {
            dead.push_back(std::move(*it));
            it = m_windows.erase(it);
        } else {
            ++it;
        }
    }
    if (dead.empty()) {
        return;
    }
    for (const auto& window : dead) {
        emit(WindowManagerEventKind::Closed, *window);
    }
    dead.clear();

    if (!m_windows.empty()) {
        return;
    }
    if (m_suppress_last_window) {
        m_suppress_last_window = false;
        return;
    }
    if (m_quitting || !m_options.quit_on_last_window_closed) {
        return;
    }
    if (m_options.on_last_window_closed) {
        m_options.on_last_window_closed();
        return;
    }
    m_quitting = true;
    std::vector<std::shared_ptr<core::Environment>> envs;
    if (m_options.environment) {
        envs.push_back(m_options.environment);
    }
    app::quit(std::move(envs));
}

ManagedWindow* WindowManager::find(std::string_view label) const noexcept {
    for (const auto& window : m_windows) {
        if (!window->m_destroyed && window->label() == label) {
            return window.get();
        }
    }
    return nullptr;
}

ManagedWindow* WindowManager::find_by_webview(std::uint64_t webview_id) const noexcept {
    for (const auto& window : m_windows) {
        if (window->m_destroyed || !window->m_host) {
            continue;
        }
        if (window->m_host->find(webview_id) != nullptr) {
            return window.get();
        }
    }
    return nullptr;
}

ManagedWindow* WindowManager::find_by_hwnd(HWND hwnd) const noexcept {
    if (hwnd == nullptr) {
        return nullptr;
    }
    for (const auto& window : m_windows) {
        if (!window->m_destroyed && window->hwnd() == hwnd) {
            return window.get();
        }
    }
    return nullptr;
}

ManagedWindow* WindowManager::focused() const noexcept {
    return find_by_hwnd(GetForegroundWindow());
}

std::vector<std::string> WindowManager::labels() const {
    std::vector<std::string> out;
    out.reserve(m_windows.size());
    for (const auto& window : m_windows) {
        if (!window->m_destroyed) {
            out.push_back(window->label());
        }
    }
    return out;
}

std::size_t WindowManager::size() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(m_windows, [](const auto& w) { return !w->m_destroyed; }));
}

bool WindowManager::show(std::string_view label, bool focus) {
    ManagedWindow* window = find(label);
    if (window == nullptr || window->hwnd() == nullptr) {
        return false;
    }
    const bool was_visible = window->m_window.is_visible();
    if (window->m_state && !window->m_state_applied) {
        window->m_state_applied = true;
        window->m_state->restore(focus ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    } else if (window->m_window.is_minimized()) {
        window->m_window.show(focus ? SW_RESTORE : SW_SHOWNOACTIVATE);
    } else {
        window->m_window.show(focus ? SW_SHOW : SW_SHOWNOACTIVATE);
    }
    window->m_state_applied = true;
    if (focus) {
        SetForegroundWindow(window->hwnd());
    }
    if (!was_visible && window->m_window.is_visible()) {
        emit(WindowManagerEventKind::Shown, *window);
    }
    return true;
}

bool WindowManager::hide(std::string_view label) {
    ManagedWindow* window = find(label);
    if (window == nullptr || window->hwnd() == nullptr) {
        return false;
    }
    const bool was_visible = window->m_window.is_visible();
    window->m_window.hide();
    if (was_visible) {
        emit(WindowManagerEventKind::Hidden, *window);
    }
    return true;
}

bool WindowManager::focus(std::string_view label) {
    ManagedWindow* window = find(label);
    if (window == nullptr || window->hwnd() == nullptr) {
        return false;
    }
    // A window disabled by a modal child hands the focus to that child (as clicking it would).
    if (!IsWindowEnabled(window->hwnd())) {
        for (const auto& other : m_windows) {
            if (!other->m_destroyed && other->is_modal() && other->m_owner == window->hwnd()) {
                window->m_window.show(SW_SHOWNA);
                other->m_window.focus();
                return true;
            }
        }
    }
    const bool was_visible = window->m_window.is_visible();
    window->m_state_applied = true;
    window->m_window.focus();
    if (!was_visible) {
        emit(WindowManagerEventKind::Shown, *window);
    }
    return true;
}

bool WindowManager::close(std::string_view label) {
    ManagedWindow* window = find(label);
    if (window == nullptr || window->hwnd() == nullptr) {
        return false;
    }
    window->m_window.close();
    return true;
}

bool WindowManager::destroy(std::string_view label) {
    ManagedWindow* window = find(label);
    if (window == nullptr || window->hwnd() == nullptr) {
        return false;
    }
    finish_close(*window, false);
    return true;
}

exec::CallbackSender<bool> WindowManager::close_async(std::string label) {
    return exec::CallbackSender<bool>(
        [this, label = std::move(label)](exec::Completion<bool> done) {
            ManagedWindow* window = find(label);
            if (window == nullptr || window->hwnd() == nullptr) {
                done(S_OK, false);
                return;
            }
            window->m_close_waiters.push_back(std::move(done));
            window->m_window.close();
        },
        "WindowManager::close_async");
}

void WindowManager::destroy_all() {
    m_suppress_last_window = true;
    // Index loop: destroying an owner also destroys its owned windows (their entries are marked destroyed).
    for (std::size_t i = 0; i < m_windows.size(); ++i) {
        ManagedWindow& window = *m_windows[i];
        if (!window.m_destroyed && window.hwnd() != nullptr) {
            finish_close(window, false);
        }
    }
    if (!m_reap_posted) {
        m_suppress_last_window = false; // nothing to reap: do not swallow a later last-window close
    }
}

void WindowManager::quit(int exit_code, std::chrono::milliseconds timeout) {
    m_quitting = true;
    destroy_all();
    std::vector<std::shared_ptr<core::Environment>> envs;
    if (m_options.environment) {
        envs.push_back(m_options.environment);
    }
    app::quit(std::move(envs), timeout, exit_code);
}

void WindowManager::register_webview(const ManagedWindow& window, core::WebView& webview) {
    {
        const std::unique_lock lock(m_registry_mutex);
        m_webview_labels.emplace_back(webview.id(), window.label());
    }
    if (m_options.bridge != nullptr && window.m_config.attach_bridge) {
        ipc::BridgeAttachOptions opts = m_options.attach_options ? m_options.attach_options(window, webview) : ipc::BridgeAttachOptions{};
        opts.window_label = window.label();
        m_options.bridge->attach(webview, std::move(opts));
    }
}

void WindowManager::unregister_webview(std::uint64_t webview_id) noexcept {
    const std::unique_lock lock(m_registry_mutex);
    std::erase_if(m_webview_labels, [webview_id](const auto& entry) { return entry.first == webview_id; });
}

void WindowManager::wake_ui() const noexcept {
    if (m_message_hwnd != nullptr) {
        PostMessageW(m_message_hwnd, kWmInvoke, 0, 0);
    }
}

void WindowManager::set_invoke_callback(std::function<void()> fn) {
    m_on_invoke = std::move(fn);
}

std::optional<std::string> WindowManager::label_of_webview(std::uint64_t webview_id) const {
    const std::shared_lock lock(m_registry_mutex);
    for (const auto& [id, label] : m_webview_labels) {
        if (id == webview_id) {
            return label;
        }
    }
    return std::nullopt;
}

std::optional<std::string> WindowManager::current_label() const {
    const std::uint64_t sender = ipc::current_sender_id();
    if (sender == 0) {
        return std::nullopt;
    }
    return label_of_webview(sender);
}

} // namespace boson::app
