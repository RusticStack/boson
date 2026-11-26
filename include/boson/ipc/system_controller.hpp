#pragma once

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/export.hpp>
#include <boson/native/dialog.hpp>
#include <boson/native/notification.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <optional>
#include <cctype>
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

class SystemController {
public:
    BOSON_CONTROLLER("Boson");

    SystemController(
        core::Window& window,
        core::WebView& webview,
        exec::UiMarshal& marshal)
        : m_window(window)
        , m_webview(webview)
        , m_marshal(marshal)
        , m_notifier(window.hwnd()) {}

    void start_drag() const {
        m_marshal.post_task([this]() {
            m_window.start_drag();
        });
    }

    void minimize() const {
        m_marshal.post_task([this]() {
            m_window.minimize();
        });
    }

    [[nodiscard]] BoolResponse maximize_or_restore() const {
        return m_marshal.invoke_sync([this]() {
            m_window.maximize_or_restore();
            return BoolResponse{.value = m_window.is_maximized()};
        });
    }

    void close() const {
        m_marshal.post_task([this]() {
            m_window.close();
        });
    }

    [[nodiscard]] BoolResponse is_maximized() const {
        return m_marshal.invoke_sync([this]() {
            return BoolResponse{.value = m_window.is_maximized()};
        });
    }

    void set_title(TitleRequest req) const {
        m_marshal.post_task([this, title = std::move(req.title)]() {
            m_window.set_title(core::utf8_to_wide(title));
        });
    }

    void set_always_on_top(ToggleRequest req) const {
        m_marshal.post_task([this, enabled = req.enabled]() {
            m_window.set_always_on_top(enabled);
        });
    }

    void set_fullscreen(ToggleRequest req) const {
        m_marshal.post_task([this, enabled = req.enabled]() {
            m_window.set_fullscreen(enabled);
        });
    }

    void set_dark_mode(ToggleRequest req) const {
        m_marshal.post_task([this, enabled = req.enabled]() {
            m_window.set_dark_mode(enabled);
        });
    }

    [[nodiscard]] BackdropStatusResponse set_backdrop(BackdropRequest req) const {
        const core::BackdropType type = parse_backdrop_type(req.backdrop);
        return m_marshal.invoke_sync([this, type, name = std::move(req.backdrop)]() {
            m_window.set_backdrop(type);
            const bool transparent_wv = m_window.is_transparent_backdrop();
            m_webview.set_transparent_background(transparent_wv);
            return BackdropStatusResponse{
                .backdrop = name,
                .transparent_webview = transparent_wv,
            };
        });
    }

    void set_opacity(OpacityRequest req) const {
        const float clamped = static_cast<float>(std::clamp(req.opacity, 0.1, 1.0));
        m_marshal.post_task([this, clamped]() {
            if (clamped < 0.999f && m_window.backdrop() != core::BackdropType::Translucent) {
                m_window.set_backdrop(core::BackdropType::Translucent);
                m_webview.set_transparent_background(false);
            }
            m_window.set_opacity(clamped);
        });
    }

    [[nodiscard]] BoolResponse notify(NotifyRequest req) {
        native::NotificationOptions opts{
            .title = std::move(req.title),
            .body = std::move(req.body),
            .level = parse_notification_level(req.level),
            .silent = req.silent,
        };
        return m_marshal.invoke_sync([this, opts = std::move(opts)]() mutable {
            m_notifier.set_owner(m_window.hwnd());
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
        return m_marshal.invoke_sync([this, opts = std::move(opts)]() {
            const auto res = native::open_file_dialog(m_window.hwnd(), opts);
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
        return m_marshal.invoke_sync([this, opts = std::move(opts)]() {
            const auto res = native::save_file_dialog(m_window.hwnd(), opts);
            return FileDialogResponse{
                .cancelled = res.cancelled,
                .paths = res.paths,
            };
        });
    }

    void go_back() const {
        m_marshal.post_task([this]() { m_webview.go_back(); });
    }

    void go_forward() const {
        m_marshal.post_task([this]() { m_webview.go_forward(); });
    }

    void reload() const {
        m_marshal.post_task([this]() { m_webview.reload(); });
    }

    void open_devtools() const {
        m_marshal.post_task([this]() { m_webview.open_devtools(); });
    }

    void set_zoom(ZoomRequest req) const {
        m_marshal.post_task([this, factor = req.factor]() { m_webview.set_zoom_factor(factor); });
    }

    void set_min_size(SizeRequest req) const {
        m_marshal.post_task([this, req]() { m_window.set_min_size(req.width, req.height); });
    }

    void center() const {
        m_marshal.post_task([this]() { m_window.center(); });
    }

    /// @brief Marks a DOM element as the caption maximize button so Windows 11 Snap Layouts appears when
    /// hovering it. The rect is mapped from CSS pixels of this WebView to window client pixels.
    void set_maximize_button_rect(ElementRectRequest req) const {
        m_marshal.post_task([this, req]() {
            if (!req.visible || req.width <= 0.0 || req.height <= 0.0) {
                m_window.set_maximize_button_rect(std::nullopt);
                return;
            }
            const double scale = static_cast<double>(m_window.dpi()) / USER_DEFAULT_SCREEN_DPI *
                                 m_webview.zoom_factor().value_or(1.0);
            const RECT origin = m_webview.bounds();
            const auto px = [scale](double css) { return static_cast<LONG>(css * scale + 0.5); };
            m_window.set_maximize_button_rect(RECT{
                origin.left + px(req.x),
                origin.top + px(req.y),
                origin.left + px(req.x + req.width),
                origin.top + px(req.y + req.height),
            });
        });
    }

private:
    core::Window& m_window;
    core::WebView& m_webview;
    exec::UiMarshal& m_marshal;
    native::Notifier m_notifier;
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
