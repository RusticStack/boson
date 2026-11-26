#pragma once

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/export.hpp>
#include <boson/ipc/request_context.hpp>
#include <boson/native/dialog.hpp>
#include <boson/native/notification.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <optional>
#include <cctype>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace boson::ipc {

struct TitleRequest {
    std::string title;
};

struct ToggleRequest {
    bool enabled = false;
};

struct BackdropRequest {
    std::string backdrop = "mica";
};

struct OpacityRequest {
    double opacity = 1.0;
};

struct NotifyRequest {
    std::string title;
    std::string body;
    std::string level = "info";
    bool silent = false;
};

struct OpenDialogRequest {
    std::string title = "Select File";
    bool allow_multiple = false;
    bool pick_folder = false;
};

struct SaveDialogRequest {
    std::string title = "Save File";
    std::string default_name;
};

struct FileDialogResponse {
    bool cancelled = true;
    std::vector<std::string> paths;
};

struct BoolResponse {
    bool value = false;
};

struct ZoomRequest {
    double factor = 1.0;
};

struct SizeRequest {
    int width = 0;
    int height = 0;
};

/// @brief A DOM rect in CSS pixels relative to the calling WebView (getBoundingClientRect()).
struct ElementRectRequest {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool visible = true;
};

struct BackdropStatusResponse {
    std::string backdrop;
    bool transparent_webview = false;
};

[[nodiscard]] inline core::BackdropType parse_backdrop_type(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (const char ch : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (lower == "none") return core::BackdropType::None;
    if (lower == "mica") return core::BackdropType::Mica;
    if (lower == "mica_alt" || lower == "micaalt" || lower == "tabbed") return core::BackdropType::MicaAlt;
    if (lower == "acrylic") return core::BackdropType::Acrylic;
    if (lower == "transparent") return core::BackdropType::Transparent;
    if (lower == "translucent") return core::BackdropType::Translucent;
    throw std::invalid_argument("Unsupported backdrop type: " + std::string(name));
}

[[nodiscard]] inline native::NotificationLevel parse_notification_level(std::string_view level) {
    if (level == "warning" || level == "warn") return native::NotificationLevel::Warning;
    if (level == "error") return native::NotificationLevel::Error;
    if (level == "none") return native::NotificationLevel::None;
    return native::NotificationLevel::Info;
}

/// @brief The window and WebView a SystemController call acts on.
struct SenderTarget {
    core::Window* window = nullptr;
    core::WebView* webview = nullptr;
};

/// @brief JS "Boson.*": window and page control for the calling window.
///
/// Fixed form: bound to one Window + WebView. Resolver form (multi-window apps): every call is routed to the
/// window of the WebView that sent it (ipc::current_sender_id()), e.g. with app::sender_resolver(manager).
class SystemController {
public:
    BOSON_CONTROLLER("Boson");

    /// Runs on the UI thread with the sender's WebView id; returns nulls when that WebView is gone.
    using TargetResolver = std::function<SenderTarget(std::uint64_t sender_webview_id)>;

    SystemController(
        core::Window& window,
        core::WebView& webview,
        exec::UiMarshal& marshal)
        : m_window(&window)
        , m_webview(&webview)
        , m_marshal(marshal)
        , m_notifier(window.hwnd()) {}

    SystemController(exec::UiMarshal& marshal, TargetResolver resolver)
        : m_marshal(marshal)
        , m_resolver(std::move(resolver)) {}

    void start_drag() const {
        post_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            w.start_drag();
        });
    }

    void minimize() const {
        post_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            w.minimize();
        });
    }

    [[nodiscard]] BoolResponse maximize_or_restore() const {
        return invoke_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            w.maximize_or_restore();
            return BoolResponse{.value = w.is_maximized()};
        });
    }

    void close() const {
        post_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            w.close();
        });
    }

    [[nodiscard]] BoolResponse is_maximized() const {
        return invoke_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            return BoolResponse{.value = w.is_maximized()};
        });
    }

    [[nodiscard]] BoolResponse is_minimized() const {
        return invoke_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            return BoolResponse{.value = w.is_minimized()};
        });
    }

    [[nodiscard]] BoolResponse is_fullscreen() const {
        return invoke_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            return BoolResponse{.value = w.is_fullscreen()};
        });
    }

    /// @brief Restores a minimized or maximized window to its normal size and position (SW_RESTORE).
    void restore() const {
        post_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            w.show(SW_RESTORE);
        });
    }

    /// @brief Application quit request from the frontend (Wails Application.Quit, a Quit menu item...).
    /// Runs the handler from set_quit_handler() on the UI thread, e.g. one that calls boson::app::quit(envs);
    /// without one it closes this window (which posts WM_QUIT when WindowConfig::quit_on_destroy is set).
    void quit() const {
        post_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            if (m_on_quit) {
                m_on_quit();
            } else {
                w.close();
            }
        });
    }

    /// @brief Overrides what quit() does. Set it before the Router starts dispatching.
    void set_quit_handler(std::function<void()> fn) {
        m_on_quit = std::move(fn);
    }

    void set_title(TitleRequest req) const {
        post_ui([this, title = std::move(req.title)]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            w.set_title(core::utf8_to_wide(title));
        });
    }

    void set_always_on_top(ToggleRequest req) const {
        post_ui([this, enabled = req.enabled]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            w.set_always_on_top(enabled);
        });
    }

    void set_fullscreen(ToggleRequest req) const {
        post_ui([this, enabled = req.enabled]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            w.set_fullscreen(enabled);
        });
    }

    void set_dark_mode(ToggleRequest req) const {
        post_ui([this, enabled = req.enabled]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            w.set_dark_mode(enabled);
        });
    }

    [[nodiscard]] BackdropStatusResponse set_backdrop(BackdropRequest req) const {
        const core::BackdropType type = parse_backdrop_type(req.backdrop);
        return invoke_ui([this, type, name = std::move(req.backdrop)]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            w.set_backdrop(type);
            const bool transparent_wv = w.is_transparent_backdrop();
            v.set_transparent_background(transparent_wv);
            return BackdropStatusResponse{
                .backdrop = name,
                .transparent_webview = transparent_wv,
            };
        });
    }

    void set_opacity(OpacityRequest req) const {
        const float clamped = static_cast<float>(std::clamp(req.opacity, 0.1, 1.0));
        post_ui([this, clamped]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            if (clamped < 0.999f && w.backdrop() != core::BackdropType::Translucent) {
                w.set_backdrop(core::BackdropType::Translucent);
                v.set_transparent_background(false);
            }
            w.set_opacity(clamped);
        });
    }

    [[nodiscard]] BoolResponse notify(NotifyRequest req) {
        native::NotificationOptions opts{
            .title = std::move(req.title),
            .body = std::move(req.body),
            .level = parse_notification_level(req.level),
            .silent = req.silent,
        };
        return invoke_ui([this, opts = std::move(opts)]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) mutable {
            m_notifier.set_owner(w.hwnd());
            return BoolResponse{.value = m_notifier.show(opts)};
        });
    }

    [[nodiscard]] FileDialogResponse open_file_dialog(OpenDialogRequest req) const {
        native::FileDialogOptions opts{
            .title = std::move(req.title),
            .allow_multiple = req.allow_multiple,
            .pick_folder = req.pick_folder,
            .default_name = {},
        };
        return invoke_ui([this, opts = std::move(opts)]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            const auto res = native::open_file_dialog(w.hwnd(), opts);
            return FileDialogResponse{
                .cancelled = res.cancelled,
                .paths = res.paths,
            };
        });
    }

    [[nodiscard]] FileDialogResponse save_file_dialog(SaveDialogRequest req) const {
        native::FileDialogOptions opts{
            .title = std::move(req.title),
            .allow_multiple = false,
            .pick_folder = false,
            .default_name = std::move(req.default_name),
        };
        return invoke_ui([this, opts = std::move(opts)]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            const auto res = native::save_file_dialog(w.hwnd(), opts);
            return FileDialogResponse{
                .cancelled = res.cancelled,
                .paths = res.paths,
            };
        });
    }

    void go_back() const {
        post_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) { v.go_back(); });
    }

    void go_forward() const {
        post_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) { v.go_forward(); });
    }

    void reload() const {
        post_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) { v.reload(); });
    }

    void open_devtools() const {
        post_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) { v.open_devtools(); });
    }

    void set_zoom(ZoomRequest req) const {
        post_ui([this, factor = req.factor]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) { v.set_zoom_factor(factor); });
    }

    void set_min_size(SizeRequest req) const {
        post_ui([this, req]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) { w.set_min_size(req.width, req.height); });
    }

    void center() const {
        post_ui([this]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) { w.center(); });
    }

    /// @brief Marks a DOM element as the caption maximize button so Windows 11 Snap Layouts appears when
    /// hovering it. The rect is mapped from CSS pixels of this WebView to window client pixels.
    void set_maximize_button_rect(ElementRectRequest req) const {
        post_ui([this, req]([[maybe_unused]] core::Window& w, [[maybe_unused]] core::WebView& v) {
            if (!req.visible || req.width <= 0.0 || req.height <= 0.0) {
                w.set_maximize_button_rect(std::nullopt);
                return;
            }
            const double scale = static_cast<double>(w.dpi()) / USER_DEFAULT_SCREEN_DPI *
                                 v.zoom_factor().value_or(1.0);
            const RECT origin = v.bounds();
            const auto px = [scale](double css) { return static_cast<LONG>(css * scale + 0.5); };
            w.set_maximize_button_rect(RECT{
                origin.left + px(req.x),
                origin.top + px(req.y),
                origin.left + px(req.x + req.width),
                origin.top + px(req.y + req.height),
            });
        });
    }

private:
    [[nodiscard]] SenderTarget target(std::uint64_t sender) const {
        if (m_resolver) {
            return m_resolver(sender);
        }
        return SenderTarget{.window = m_window, .webview = m_webview};
    }

    /// Posts fn(window, webview) to the UI thread for the calling window; dropped when it no longer exists.
    template <typename Fn>
    void post_ui(Fn&& fn) const {
        m_marshal.post_task([this, sender = current_sender_id(), fn = std::forward<Fn>(fn)]() mutable {
            const SenderTarget t = target(sender);
            if (t.window != nullptr && t.webview != nullptr) {
                fn(*t.window, *t.webview);
            }
        });
    }

    /// Runs fn(window, webview) on the UI thread and waits for its result (pool thread only).
    template <typename Fn>
    auto invoke_ui(Fn&& fn) const {
        return m_marshal.invoke_sync([this, sender = current_sender_id(), &fn]() mutable {
            const SenderTarget t = target(sender);
            if (t.window == nullptr || t.webview == nullptr) {
                throw std::runtime_error("the calling window no longer exists");
            }
            return fn(*t.window, *t.webview);
        });
    }

    core::Window* m_window = nullptr;
    core::WebView* m_webview = nullptr;
    exec::UiMarshal& m_marshal;
    TargetResolver m_resolver;
    native::Notifier m_notifier;
    std::function<void()> m_on_quit;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::SystemController> {
    using T = boson::ipc::SystemController;
    static constexpr auto value = glz::object(
        "start_drag", &T::start_drag,
        "minimize", &T::minimize,
        "maximize_or_restore", &T::maximize_or_restore,
        "close", &T::close,
        "is_maximized", &T::is_maximized,
        "is_minimized", &T::is_minimized,
        "is_fullscreen", &T::is_fullscreen,
        "restore", &T::restore,
        "quit", &T::quit,
        "set_title", &T::set_title,
        "set_always_on_top", &T::set_always_on_top,
        "set_fullscreen", &T::set_fullscreen,
        "set_dark_mode", &T::set_dark_mode,
        "set_backdrop", &T::set_backdrop,
        "set_opacity", &T::set_opacity,
        "notify", &T::notify,
        "open_file_dialog", &T::open_file_dialog,
        "save_file_dialog", &T::save_file_dialog,
        "go_back", &T::go_back,
        "go_forward", &T::go_forward,
        "reload", &T::reload,
        "open_devtools", &T::open_devtools,
        "set_zoom", &T::set_zoom,
        "set_min_size", &T::set_min_size,
        "center", &T::center,
        "set_maximize_button_rect", &T::set_maximize_button_rect);
};
