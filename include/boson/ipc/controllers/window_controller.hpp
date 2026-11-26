#pragma once

#include <windows.h>

#include <boson/app/window_manager.hpp>
#include <boson/core/result.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/export.hpp>
#include <boson/ipc/request_context.hpp>
#include <boson/ipc/system_controller.hpp>
#include <boson/security/capabilities.hpp>
#include <boson/security/origin.hpp>

#include <glaze/glaze.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boson::ipc {

/// "Window.create" parameters. Sizes and positions are DIPs; omitted fields keep the WindowConfig defaults.
struct WindowCreateRequest {
    std::string label;
    /// "" = index.html; relative paths resolve against the dev server or boson://app/ (WindowManager::resolve_url).
    std::string url;
    std::string title;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<int> min_width;
    std::optional<int> min_height;
    std::optional<int> x;
    std::optional<int> y;
    bool center = false;
    bool frameless = false;
    bool headless_titlebar = false;
    std::string backdrop; ///< "" | "none" | "mica" | "mica_alt" | "acrylic" | "transparent" | "translucent"
    std::optional<bool> dark_mode;
    bool always_on_top = false;
    bool skip_taskbar = false;
    bool transparent = false;
    bool visible = true;
    bool focus = true;
    /// Owner label; "" = top-level (a modal window without parent is owned by the calling window).
    std::string parent;
    bool modal = false;
    bool persist_state = false;
    bool hide_on_close = false;
};

/// Every "Window.*" call that targets one window; label "" = the calling window.
struct WindowLabelRequest {
    std::string label;
};

struct WindowTitleRequest {
    std::string label;
    std::string title;
};

/// DIPs; omitted fields keep the current value.
struct WindowBoundsRequest {
    std::string label;
    std::optional<int> x;
    std::optional<int> y;
    std::optional<int> width;
    std::optional<int> height;
};

struct WindowSizeRequest {
    std::string label;
    int width = 0;
    int height = 0;
};

struct WindowFlagRequest {
    std::string label;
    bool enabled = false;
};

struct WindowShowRequest {
    std::string label;
    bool focus = true;
};

/// "Window.navigate": url resolves like Window.create's (same allow-list).
struct WindowNavigateRequest {
    std::string label;
    std::string url;
};

struct WindowConfirmCloseRequest {
    std::string label;
    std::uint64_t request_id = 0;
    bool allow = true;
};

/// "Window.emit": target = window label, "*" = every window, "" = the calling window.
struct WindowEmitRequest {
    std::string target;
    std::string event;
    glz::raw_json payload{};
};

struct WindowInfo {
    std::string label;
    std::string title;
    std::string url;
    int x = 0;      ///< DIPs (screen pixels / scale_factor of the window's monitor)
    int y = 0;
    int width = 0;
    int height = 0;
    double scale_factor = 1.0;
    bool visible = false;
    bool focused = false;
    bool enabled = true; ///< false while a modal child is open
    bool maximized = false;
    bool minimized = false;
    bool fullscreen = false;
    bool modal = false;
    std::string parent;
    bool close_guard = false;
    std::vector<std::uint64_t> webview_ids;
};

struct WindowListResponse {
    std::vector<WindowInfo> windows;
};

struct WindowOkResponse {
    bool ok = false;
};

// ---- Events ---------------------------------------------------------------------------------------------

struct WindowLabelEvent {
    std::string label;
};

/// "window:moved" / "window:resized": outer bounds in DIPs.
struct WindowBoundsEvent {
    std::string label;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double scale_factor = 1.0;
};

/// "window:close-requested" (only to the window being closed, when its close guard is on). Answer with
/// Window.confirm_close { label, request_id, allow }.
struct WindowCloseRequestedEvent {
    std::string label;
    std::uint64_t request_id = 0;
};

/// Event registry for TsGenerator::generate_with_events.
struct WindowManagerEvents {
    WindowLabelEvent created;
    WindowLabelEvent closed;
    WindowLabelEvent focus;
    WindowLabelEvent blur;
    WindowBoundsEvent moved;
    WindowBoundsEvent resized;
    WindowLabelEvent maximized;
    WindowLabelEvent minimized;
    WindowLabelEvent restored;
    WindowLabelEvent shown;
    WindowLabelEvent hidden;
    WindowCloseRequestedEvent close_requested;
};

struct WindowControllerOptions {
    /// Label allow-list for Window.create: a page may create window L only when a capability matching its
    /// origin lists L in Capability::windows. Null = pages cannot create windows (C++ still can).
    std::shared_ptr<const security::CapabilityPolicy> policy;
    /// Origin patterns (security::OriginPattern) of absolute URLs a page may open in a new window, beyond
    /// boson://app and the dev server. Such windows are not attached to the Bridge.
    std::vector<std::string> allowed_url_origins;
    /// Upper bound on live windows for Window.create.
    std::size_t max_windows = 32;
    /// false: window:moved / window:resized go to the moved window only. true: to every window.
    bool broadcast_geometry = false;
};

/// @brief SystemController resolver that routes "Boson.*" calls to the window of the calling WebView.
[[nodiscard]] inline SystemController::TargetResolver sender_resolver(app::WindowManager& manager) {
    return [&manager](std::uint64_t sender) -> SenderTarget {
        app::ManagedWindow* window = manager.find_by_webview(sender);
        if (window == nullptr || window->host() == nullptr) {
            return {};
        }
        return SenderTarget{.window = &window->window(), .webview = window->host()->find(sender)};
    };
}

/// @brief JS "Window.*": window-by-label operations over an app::WindowManager, and the window:* events.
///
/// Methods run on the router's pool thread and marshal to the UI thread (never call them on the UI thread).
/// Construct and destroy on the UI thread, after the manager and before it goes away.
class WindowController {
public:
    BOSON_CONTROLLER("Window");

    /// Events and Window.emit go through manager.bridge() (WindowManager::set_bridge), so the controller can be
    /// built before the Router and Bridge that contain it.
    WindowController(app::WindowManager& manager, exec::UiMarshal& marshal, WindowControllerOptions options = {})
        : m_manager(manager), m_marshal(marshal), m_options(std::move(options)) {
        for (const auto& pattern : m_options.allowed_url_origins) {
            security::OriginPattern compiled(pattern);
            if (compiled.valid()) {
                m_url_origins.push_back(std::move(compiled));
            }
        }
        m_listener = m_manager.on_event([this](const app::WindowManagerEvent& e) { forward(e); });
    }

    ~WindowController() { m_manager.off(m_listener); }

    WindowController(const WindowController&) = delete;
    WindowController& operator=(const WindowController&) = delete;

    [[nodiscard]] WindowInfo create(WindowCreateRequest req) const {
        return on_ui([this, req = std::move(req)](std::uint64_t sender) {
            app::ManagedWindow* caller = m_manager.find_by_webview(sender);
            const std::string source = caller_source(caller, sender);
            if (!m_options.policy || !m_options.policy->may_create_window(req.label, source)) {
                throw std::runtime_error("permission denied: no capability lets this page create window '" + req.label + "'");
            }
            if (m_manager.size() >= m_options.max_windows) {
                throw std::runtime_error("too many windows");
            }
            app::ManagedWindowConfig config = to_config(req, caller);
            auto created = m_manager.create(std::move(config));
            if (!created) {
                throw std::runtime_error(created.error());
            }
            return info(**created);
        });
    }

    [[nodiscard]] WindowListResponse list() const {
        return on_ui([this](std::uint64_t) {
            WindowListResponse out;
            out.windows.reserve(m_manager.windows().size());
            for (const auto& window : m_manager.windows()) {
                if (!window->is_destroyed()) {
                    out.windows.push_back(info(*window));
                }
            }
            return out;
        });
    }

    [[nodiscard]] WindowInfo get(WindowLabelRequest req) const {
        return on_ui([this, &req](std::uint64_t sender) { return info(require(req.label, sender)); });
    }

    /// The calling window.
    [[nodiscard]] WindowInfo current() const {
        return on_ui([this](std::uint64_t sender) { return info(require({}, sender)); });
    }

    void show(WindowShowRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { m_manager.show(require(req.label, sender).label(), req.focus); });
    }

    void hide(WindowLabelRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { m_manager.hide(require(req.label, sender).label()); });
    }

    void focus(WindowLabelRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { m_manager.focus(require(req.label, sender).label()); });
    }

    /// Requests a close (close handlers / close guard / hide_on_close apply).
    void close(WindowLabelRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { m_manager.close(require(req.label, sender).label()); });
    }

    /// Closes without asking (Electron's destroy()).
    void destroy(WindowLabelRequest req) const {
        on_ui([this, &req](std::uint64_t sender) {
            const std::string label = require(req.label, sender).label();
            m_manager.destroy(label);
        });
    }

    /// Navigates the window's main WebView (Electron loadURL / loadFile). Same URL allow-list as create.
    void navigate(WindowNavigateRequest req) const {
        on_ui([this, &req](std::uint64_t sender) {
            app::ManagedWindow& window = require(req.label, sender);
            const std::string url = m_manager.resolve_url(req.url);
            const bool app_url = is_app_url(url);
            if (!app_url && !security::matches_any(m_url_origins, url)) {
                throw std::runtime_error("permission denied: window URL '" + url + "' is not allowed");
            }
            core::WebView* webview = window.main_webview();
            if (webview == nullptr || !webview->navigate(url)) {
                throw std::runtime_error("the window has no ready WebView to navigate");
            }
        });
    }

    void minimize(WindowLabelRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { require(req.label, sender).window().minimize(); });
    }

    void maximize(WindowLabelRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { require(req.label, sender).window().show(SW_MAXIMIZE); });
    }

    void unmaximize(WindowLabelRequest req) const {
        on_ui([this, &req](std::uint64_t sender) {
            core::Window& window = require(req.label, sender).window();
            if (window.is_maximized()) {
                window.show(SW_RESTORE);
            }
        });
    }

    void restore(WindowLabelRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { require(req.label, sender).window().show(SW_RESTORE); });
    }

    void center(WindowLabelRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { require(req.label, sender).window().center(); });
    }

    void set_title(WindowTitleRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { require(req.label, sender).window().set_title(core::utf8_to_wide(req.title)); });
    }

    [[nodiscard]] WindowInfo set_bounds(WindowBoundsRequest req) const {
        return on_ui([this, &req](std::uint64_t sender) {
            app::ManagedWindow& managed = require(req.label, sender);
            core::Window& window = managed.window();
            if (window.is_maximized() || window.is_minimized()) {
                window.show(SW_RESTORE);
            }
            const RECT current = window.bounds();
            const int dpi = static_cast<int>(window.dpi());
            const auto px = [dpi](int dip) { return MulDiv(dip, dpi, USER_DEFAULT_SCREEN_DPI); };
            const int left = req.x ? px(*req.x) : current.left;
            const int top = req.y ? px(*req.y) : current.top;
            const int width = req.width ? px(*req.width) : current.right - current.left;
            const int height = req.height ? px(*req.height) : current.bottom - current.top;
            if (width <= 0 || height <= 0) {
                throw std::invalid_argument("width and height must be positive");
            }
            window.set_bounds(RECT{left, top, left + width, top + height});
            return info(managed);
        });
    }

    void set_min_size(WindowSizeRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { require(req.label, sender).window().set_min_size(req.width, req.height); });
    }

    void set_always_on_top(WindowFlagRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { require(req.label, sender).window().set_always_on_top(req.enabled); });
    }

    void set_fullscreen(WindowFlagRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { require(req.label, sender).window().set_fullscreen(req.enabled); });
    }

    [[nodiscard]] WindowOkResponse set_skip_taskbar(WindowFlagRequest req) const {
        return on_ui([this, &req](std::uint64_t sender) {
            return WindowOkResponse{.ok = m_manager.set_skip_taskbar(require(req.label, sender).label(), req.enabled)};
        });
    }

    /// While enabled, closing the window emits "window:close-requested" to it and waits for confirm_close.
    void set_close_guard(WindowFlagRequest req) const {
        on_ui([this, &req](std::uint64_t sender) { m_manager.set_close_guard(require(req.label, sender).label(), req.enabled); });
    }

    [[nodiscard]] WindowOkResponse confirm_close(WindowConfirmCloseRequest req) const {
        return on_ui([this, &req](std::uint64_t sender) {
            const std::string label = require(req.label, sender).label();
            return WindowOkResponse{.ok = m_manager.resolve_close(label, req.request_id, req.allow)};
        });
    }

    /// Sends an event to a window by label ("*" = all, "" = the caller). "window:*" names are reserved.
    [[nodiscard]] WindowOkResponse emit(WindowEmitRequest req) const {
        if (req.event.empty() || req.event.starts_with("window:")) {
            throw std::invalid_argument("invalid or reserved event name: " + req.event);
        }
        Bridge* bridge = m_manager.bridge();
        if (bridge == nullptr) {
            throw std::runtime_error("no Bridge is set on the WindowManager");
        }
        const std::string_view payload = req.payload.str.empty() ? std::string_view{"null"} : std::string_view{req.payload.str};
        if (req.target == "*") {
            return WindowOkResponse{.ok = bridge->emit_json(Bridge::kAllWebViews, req.event, payload)};
        }
        std::string target = req.target;
        if (target.empty()) {
            const auto label = m_manager.current_label();
            if (!label.has_value()) {
                throw std::runtime_error("the calling window is unknown");
            }
            target = *label;
        }
        return WindowOkResponse{.ok = bridge->emit_json_to_window(target, req.event, payload)};
    }

    /// @brief JSON snapshot of one window (UI thread).
    [[nodiscard]] static WindowInfo info(app::ManagedWindow& managed) {
        const core::Window& window = managed.window();
        const RECT b = window.bounds();
        const int dpi = static_cast<int>(window.hwnd() != nullptr ? window.dpi() : USER_DEFAULT_SCREEN_DPI);
        const auto dip = [dpi](int px) { return MulDiv(px, USER_DEFAULT_SCREEN_DPI, dpi); };
        WindowInfo out{
            .label = managed.label(),
            .title = {},
            .url = {},
            .x = dip(b.left),
            .y = dip(b.top),
            .width = dip(b.right - b.left),
            .height = dip(b.bottom - b.top),
            .scale_factor = static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI,
            .visible = window.is_visible(),
            .focused = window.hwnd() != nullptr && GetForegroundWindow() == window.hwnd(),
            .enabled = window.hwnd() != nullptr && IsWindowEnabled(window.hwnd()) != FALSE,
            .maximized = window.is_maximized(),
            .minimized = window.is_minimized(),
            .fullscreen = window.is_fullscreen(),
            .modal = managed.is_modal(),
            .parent = managed.parent_label(),
            .close_guard = managed.close_guard(),
            .webview_ids = {},
        };
        if (window.hwnd() != nullptr) {
            const int length = GetWindowTextLengthW(window.hwnd());
            if (length > 0) {
                std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
                const int copied = GetWindowTextW(window.hwnd(), title.data(), length + 1);
                title.resize(static_cast<std::size_t>(std::max(copied, 0)));
                out.title = core::wide_to_utf8(title);
            }
        }
        if (core::WebView* main = managed.main_webview(); main != nullptr) {
            out.url = main->source().value_or(std::string{});
        }
        if (core::WebViewHost* host = managed.host(); host != nullptr) {
            for (const auto& webview : host->webviews()) {
                out.webview_ids.push_back(webview->id());
            }
        }
        return out;
    }

private:
    /// Runs fn(sender_webview_id) on the UI thread and waits for its result.
    template <typename Fn>
    auto on_ui(Fn&& fn) const {
        return m_marshal.invoke_sync([sender = current_sender_id(), &fn]() { return fn(sender); });
    }

    app::ManagedWindow& require(std::string_view label, std::uint64_t sender) const {
        app::ManagedWindow* window = label.empty() ? m_manager.find_by_webview(sender) : m_manager.find(label);
        if (window == nullptr) {
            throw std::runtime_error(label.empty() ? std::string("the calling window is unknown")
                                                   : "no window labelled '" + std::string(label) + "'");
        }
        return *window;
    }

    static std::string caller_source(app::ManagedWindow* caller, std::uint64_t sender) {
        if (caller == nullptr || caller->host() == nullptr) {
            return {};
        }
        const core::WebView* webview = caller->host()->find(sender);
        return webview != nullptr ? webview->source().value_or(std::string{}) : std::string{};
    }

    [[nodiscard]] bool is_app_url(std::string_view url) const {
        if (url.starts_with("boson://app/")) {
            return true;
        }
        const auto& dev = m_manager.options().dev_url;
        return dev.has_value() && !dev->empty() && security::normalized_origin(url) == security::normalized_origin(*dev);
    }

    app::ManagedWindowConfig to_config(const WindowCreateRequest& req, const app::ManagedWindow* caller) const {
        const std::string url = m_manager.resolve_url(req.url);
        const bool app_url = is_app_url(url);
        if (!app_url && !security::matches_any(m_url_origins, url)) {
            throw std::runtime_error("permission denied: window URL '" + url + "' is not allowed");
        }

        app::ManagedWindowConfig config;
        config.label = req.label;
        if (!req.title.empty()) {
            config.window.title = core::utf8_to_wide(req.title);
        }
        config.window.width = req.width.value_or(config.window.width);
        config.window.height = req.height.value_or(config.window.height);
        config.window.min_width = req.min_width.value_or(0);
        config.window.min_height = req.min_height.value_or(0);
        config.window.borderless = req.frameless;
        config.window.headless_titlebar = req.headless_titlebar;
        config.window.dark_mode = req.dark_mode.value_or(config.window.dark_mode);
        config.window.always_on_top = req.always_on_top;
        if (!req.backdrop.empty()) {
            config.window.backdrop = parse_backdrop_type(req.backdrop);
        }
        if (req.x.has_value() && req.y.has_value()) {
            // DIPs on the primary monitor's scale (the window's own monitor is not known yet).
            const UINT dpi = GetDpiForSystem();
            config.x = MulDiv(*req.x, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
            config.y = MulDiv(*req.y, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        }
        config.center = req.center;
        config.visible = req.visible;
        config.focus = req.focus;
        config.skip_taskbar = req.skip_taskbar;
        config.transparent = req.transparent;
        config.parent = req.parent;
        config.modal = req.modal;
        if (config.modal && config.parent.empty() && caller != nullptr) {
            config.parent = caller->label();
        }
        config.persist_state = req.persist_state;
        config.hide_on_close = req.hide_on_close;
        config.attach_bridge = app_url;

        core::WebViewConfig webview = m_manager.options().default_webview;
        webview.initial_url = url;
        webview.dev_url.reset();
        if (app_url && !url.starts_with("boson://")) {
            webview.dev_url = url; // dev server page: lets the Bridge accept its origin
        }
        if (!app_url) {
            webview.serve_boson_assets = false;
        }
        config.webview = std::move(webview);
        return config;
    }

    void forward(const app::WindowManagerEvent& e) {
        using Kind = app::WindowManagerEventKind;
        Bridge* bridge = m_manager.bridge();
        if (bridge == nullptr) {
            return;
        }
        const std::string_view name = event_name(e.kind);
        switch (e.kind) {
        case Kind::Moved:
        case Kind::Resized: {
            const int dpi = static_cast<int>(e.dpi == 0 ? USER_DEFAULT_SCREEN_DPI : e.dpi);
            const auto dip = [dpi](int px) { return MulDiv(px, USER_DEFAULT_SCREEN_DPI, dpi); };
            const WindowBoundsEvent payload{
                .label = std::string(e.label),
                .x = dip(e.bounds.left),
                .y = dip(e.bounds.top),
                .width = dip(e.bounds.right - e.bounds.left),
                .height = dip(e.bounds.bottom - e.bounds.top),
                .scale_factor = static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI,
            };
            if (m_options.broadcast_geometry) {
                bridge->emit(name, payload);
            } else {
                bridge->emit_to(e.label, name, payload);
            }
            break;
        }
        case Kind::CloseRequested:
            bridge->emit_to(e.label, name, WindowCloseRequestedEvent{.label = std::string(e.label), .request_id = e.close_request_id});
            break;
        default:
            bridge->emit(name, WindowLabelEvent{.label = std::string(e.label)});
            break;
        }
    }

    static std::string_view event_name(app::WindowManagerEventKind kind) noexcept {
        using Kind = app::WindowManagerEventKind;
        switch (kind) {
        case Kind::Created: return "window:created";
        case Kind::Closed: return "window:closed";
        case Kind::Focus: return "window:focus";
        case Kind::Blur: return "window:blur";
        case Kind::Moved: return "window:moved";
        case Kind::Resized: return "window:resized";
        case Kind::Maximized: return "window:maximized";
        case Kind::Minimized: return "window:minimized";
        case Kind::Restored: return "window:restored";
        case Kind::Shown: return "window:shown";
        case Kind::Hidden: return "window:hidden";
        case Kind::CloseRequested: return "window:close-requested";
        }
        return "window:unknown";
    }

    app::WindowManager& m_manager;
    exec::UiMarshal& m_marshal;
    WindowControllerOptions m_options;
    std::vector<security::OriginPattern> m_url_origins;
    app::WindowManager::ListenerId m_listener = 0;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::WindowManagerEvents> {
    using T = boson::ipc::WindowManagerEvents;
    static constexpr auto value = glz::object(
        "window:created", &T::created,
        "window:closed", &T::closed,
        "window:focus", &T::focus,
        "window:blur", &T::blur,
        "window:moved", &T::moved,
        "window:resized", &T::resized,
        "window:maximized", &T::maximized,
        "window:minimized", &T::minimized,
        "window:restored", &T::restored,
        "window:shown", &T::shown,
        "window:hidden", &T::hidden,
        "window:close-requested", &T::close_requested);
};

template <>
struct glz::meta<boson::ipc::WindowController> {
    using T = boson::ipc::WindowController;
    static constexpr auto value = glz::object(
        "create", &T::create,
        "list", &T::list,
        "get", &T::get,
        "current", &T::current,
        "show", &T::show,
        "hide", &T::hide,
        "focus", &T::focus,
        "close", &T::close,
        "destroy", &T::destroy,
        "navigate", &T::navigate,
        "minimize", &T::minimize,
        "maximize", &T::maximize,
        "unmaximize", &T::unmaximize,
        "restore", &T::restore,
        "center", &T::center,
        "set_title", &T::set_title,
        "set_bounds", &T::set_bounds,
        "set_min_size", &T::set_min_size,
        "set_always_on_top", &T::set_always_on_top,
        "set_fullscreen", &T::set_fullscreen,
        "set_skip_taskbar", &T::set_skip_taskbar,
        "set_close_guard", &T::set_close_guard,
        "confirm_close", &T::confirm_close,
        "emit", &T::emit);
};
