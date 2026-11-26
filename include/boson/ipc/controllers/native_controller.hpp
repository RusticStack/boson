#pragma once

#include <boson/core/environment.hpp>
#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/controllers/shell_common.hpp>
#include <boson/ipc/export.hpp>
#include <boson/native/dialog.hpp>
#include <boson/native/message_box.hpp>
#include <boson/native/metrics.hpp>
#include <boson/native/permissions.hpp>
#include <boson/native/session.hpp>
#include <boson/native/system_info.hpp>
#include <boson/native/web_notifications.hpp>
#include <boson/native/zoom.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace boson::ipc {

// ---- Dialog -----------------------------------------------------------------------------------------------

struct DialogFilter {
    std::string name;
    std::vector<std::string> extensions; ///< "txt", "md", "*"
};

struct FileDialogRequest {
    std::string title;
    std::string default_path;  ///< folder, or folder + file name
    std::string default_name;
    std::string button_label;
    std::vector<DialogFilter> filters;
    std::uint32_t filter_index = 0;
    bool multiple = false;     ///< open only
    bool show_hidden = false;
    bool confirm_overwrite = true; ///< save only
    std::string default_extension; ///< save only
    bool add_to_recent = true;
    bool modal = true;         ///< owned by (and modal to) the window; false = no owner
};

struct FileDialogReply {
    bool cancelled = true;
    std::vector<std::string> paths;
    std::string path;          ///< paths[0] or ""
    std::uint32_t filter_index = 0;
    std::string error;
};

struct MessageBoxRequest {
    std::string title;
    std::string message;
    std::string detail;
    std::string type = "none"; ///< none | info | warning | error | question | shield
    std::vector<std::string> buttons;
    int default_id = 0;
    int cancel_id = -1;
    std::string checkbox_label;
    bool checkbox_checked = false;
    std::string footer;
    bool command_links = false;
    bool modal = true;
};

struct MessageBoxReply {
    int response = 0;
    bool checkbox_checked = false;
    bool cancelled = false;
};

struct ErrorBoxRequest {
    std::string title;
    std::string content;
};

namespace native_detail {

[[nodiscard]] inline native::FileDialogOptions to_file_dialog_options(FileDialogRequest req) {
    native::FileDialogOptions options;
    options.title = std::move(req.title);
    options.default_path = std::move(req.default_path);
    options.default_name = std::move(req.default_name);
    options.button_label = std::move(req.button_label);
    options.filter_index = req.filter_index;
    options.allow_multiple = req.multiple;
    options.show_hidden = req.show_hidden;
    options.confirm_overwrite = req.confirm_overwrite;
    options.default_extension = std::move(req.default_extension);
    options.add_to_recent = req.add_to_recent;
    options.filters.reserve(req.filters.size());
    for (DialogFilter& filter : req.filters) {
        options.filters.push_back(native::FileFilter{.name = std::move(filter.name), .extensions = std::move(filter.extensions)});
    }
    return options;
}

[[nodiscard]] inline FileDialogReply to_reply(native::FileDialogResult result) {
    FileDialogReply reply;
    reply.cancelled = result.cancelled;
    reply.filter_index = result.filter_index;
    if (!result.paths.empty()) {
        reply.path = result.paths.front();
    }
    reply.paths = std::move(result.paths);
    if (FAILED(result.hr)) {
        char code[32]{};
        std::snprintf(code, sizeof(code), "HRESULT 0x%08lX", static_cast<unsigned long>(result.hr));
        reply.error = code;
    }
    return reply;
}

/// Runs fn on the UI thread from its own message-loop dispatch (native::defer_to_message_loop) and blocks the
/// calling pool thread until it returns. Modal loops started by fn keep the UI thread pumping.
template <typename Fn>
auto run_modal(exec::UiMarshal& marshal, Fn fn) -> std::invoke_result_t<Fn&> {
    using Result = std::invoke_result_t<Fn&>;
    auto promise = std::make_shared<std::promise<Result>>();
    std::future<Result> future = promise->get_future();
    marshal.post_task([promise, fn = std::move(fn)]() mutable {
        native::defer_to_message_loop([promise, fn = std::move(fn)]() mutable {
            try {
                promise->set_value(fn());
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
    });
    return future.get();
}

} // namespace native_detail

/// @brief JS "Dialog.*": file open / save / folder pickers and task-dialog message boxes, modal to the
/// window. Each call holds its pool thread until the user closes the dialog; the UI thread keeps pumping in
/// the dialog's modal loop, which starts from its own message dispatch (never nested in other UI work).
class DialogController {
public:
    BOSON_CONTROLLER("Dialog");

    DialogController(core::Window& window, exec::UiMarshal& marshal) : m_window(window), m_marshal(marshal) {}

    [[nodiscard]] FileDialogReply open(FileDialogRequest req) const { return show(native::FileDialogKind::Open, std::move(req)); }
    [[nodiscard]] FileDialogReply save(FileDialogRequest req) const { return show(native::FileDialogKind::Save, std::move(req)); }
    [[nodiscard]] FileDialogReply pick_folder(FileDialogRequest req) const {
        return show(native::FileDialogKind::Folder, std::move(req));
    }

    [[nodiscard]] MessageBoxReply message_box(MessageBoxRequest req) const {
        const auto icon = native::parse_message_box_icon(req.type);
        if (!icon.has_value()) {
            throw std::invalid_argument("Unsupported message box type: " + req.type);
        }
        native::MessageBoxOptions options;
        options.title = std::move(req.title);
        options.message = std::move(req.message);
        options.detail = std::move(req.detail);
        options.icon = *icon;
        options.buttons = std::move(req.buttons);
        options.default_id = req.default_id;
        options.cancel_id = req.cancel_id;
        options.checkbox_label = std::move(req.checkbox_label);
        options.checkbox_checked = req.checkbox_checked;
        options.footer = std::move(req.footer);
        options.command_links = req.command_links;
        const bool modal = req.modal;
        const native::MessageBoxResult result = native_detail::run_modal(m_marshal, [this, modal, options = std::move(options)]() {
            return native::show_message_box(modal ? m_window.hwnd() : nullptr, options);
        });
        return MessageBoxReply{.response = result.response, .checkbox_checked = result.checkbox_checked, .cancelled = result.cancelled};
    }

    void error_box(ErrorBoxRequest req) const {
        native_detail::run_modal(m_marshal, [this, req = std::move(req)]() {
            native::show_error_box(m_window.hwnd(), req.title, req.content);
            return true;
        });
    }

private:
    [[nodiscard]] FileDialogReply show(native::FileDialogKind kind, FileDialogRequest req) const {
        const bool modal = req.modal;
        native::FileDialogOptions options = native_detail::to_file_dialog_options(std::move(req));
        return native_detail::to_reply(native_detail::run_modal(m_marshal, [this, kind, modal, options = std::move(options)]() {
            return native::show_file_dialog(kind, modal ? m_window.hwnd() : nullptr, options);
        }));
    }

    core::Window& m_window;
    exec::UiMarshal& m_marshal;
};

// ---- Permissions ------------------------------------------------------------------------------------------

struct PermissionTarget {
    std::string origin; ///< URI or "scheme://host[:port]"; "*" = every origin
    std::string kind;   ///< camera | microphone | geolocation | notifications | clipboard_read | ...
};

struct PermissionSetRequest {
    std::string origin;
    std::string kind;
    std::string decision; ///< allow | deny | ask | default
};

struct PermissionOriginRequest {
    std::string origin; ///< empty = every origin
};

struct PermissionQueryReply {
    std::string decision;       ///< effective decision
    bool remembered = false;    ///< from a stored rule
};

struct PermissionRespondRequest {
    std::uint64_t id = 0;
    std::string decision = "deny";
    bool remember = false;
};

struct PermissionPromptEvent {
    std::uint64_t id = 0;
    std::uint64_t webview_id = 0;
    std::string origin;
    std::string uri;
    std::string kind;
    bool user_initiated = false;
};

struct PermissionChangedEvent {
    std::vector<native::PermissionRule> rules;
};

struct PromptHandlerRequest {
    bool enabled = true;
};

namespace native_detail {

[[nodiscard]] inline core::PermissionKind kind_or_throw(std::string_view name) {
    const auto kind = native::parse_permission_kind(name);
    if (!kind.has_value()) {
        throw std::invalid_argument("Unknown permission kind: " + std::string(name));
    }
    return *kind;
}

[[nodiscard]] inline native::PermissionDecision decision_or_throw(std::string_view name) {
    const auto decision = native::parse_permission_decision(name);
    if (!decision.has_value()) {
        throw std::invalid_argument("Unknown permission decision: " + std::string(name));
    }
    return *decision;
}

[[nodiscard]] inline PermissionPromptEvent to_prompt_event(const native::PermissionPrompt& p) {
    return PermissionPromptEvent{
        .id = p.id,
        .webview_id = p.webview_id,
        .origin = p.origin,
        .uri = p.uri,
        .kind = std::string(native::to_string(p.kind)),
        .user_initiated = p.user_initiated,
    };
}

} // namespace native_detail

/// @brief JS "Permissions.*" over a native::PermissionManager (rules, queries and the JS prompt hook).
/// Events: "permission:prompt" (after set_prompt_handler({enabled: true})), "permission:changed".
/// Construct on the UI thread; call connect_events(controller, bridge) once the Bridge exists.
class PermissionController {
public:
    BOSON_CONTROLLER("Permissions");

    PermissionController(native::PermissionManager& manager, exec::UiMarshal& marshal) : m_manager(manager), m_marshal(marshal) {
        m_manager.set_change_callback([this]() {
            m_events.emit("permission:changed", PermissionChangedEvent{.rules = m_manager.rules()});
        });
    }
    ~PermissionController() { m_manager.set_change_callback({}); }

    PermissionController(const PermissionController&) = delete;
    PermissionController& operator=(const PermissionController&) = delete;

    friend void connect_events(PermissionController& controller, Bridge& bridge) noexcept { controller.m_events.set(&bridge); }

    [[nodiscard]] std::vector<native::PermissionRule> list() const { return m_manager.rules(); }

    [[nodiscard]] PermissionQueryReply query(PermissionTarget req) const {
        const core::PermissionKind kind = native_detail::kind_or_throw(req.kind);
        const auto remembered = m_manager.rule(req.origin, kind);
        return PermissionQueryReply{
            .decision = std::string(native::to_string(m_manager.resolve(req.origin, kind))),
            .remembered = remembered.has_value(),
        };
    }

    [[nodiscard]] ShellResult set(PermissionSetRequest req) const {
        const core::PermissionKind kind = native_detail::kind_or_throw(req.kind);
        const native::PermissionDecision decision = native_detail::decision_or_throw(req.decision);
        const bool ok = m_manager.set(req.origin, kind, decision);
        return ShellResult{.ok = ok, .error = ok ? std::string{} : std::string("invalid origin or storage failure")};
    }

    [[nodiscard]] ShellResult reset(PermissionTarget req) const {
        return ShellResult{.ok = m_manager.reset(req.origin, native_detail::kind_or_throw(req.kind)), .error = {}};
    }

    [[nodiscard]] ShellResult clear(PermissionOriginRequest req) const {
        return ShellResult{.ok = m_manager.clear(req.origin), .error = {}};
    }

    [[nodiscard]] std::vector<PermissionPromptEvent> pending() const {
        return m_marshal.invoke_sync([this]() {
            std::vector<PermissionPromptEvent> out;
            for (const native::PermissionPrompt& p : m_manager.pending()) {
                out.push_back(native_detail::to_prompt_event(p));
            }
            return out;
        });
    }

    /// @brief Answers a "permission:prompt" event.
    [[nodiscard]] ShellResult respond(PermissionRespondRequest req) const {
        const native::PermissionDecision decision = native_detail::decision_or_throw(req.decision);
        const bool ok = m_marshal.invoke_sync([this, &req, decision]() { return m_manager.answer(req.id, decision, req.remember); });
        return ShellResult{.ok = ok, .error = ok ? std::string{} : std::string("no pending prompt with this id")};
    }

    /// @brief enabled: Ask requests raise "permission:prompt" and wait for respond(). Disabled (the default):
    /// Ask falls back to WebView2's own prompt. Pending prompts are answered "default" on disable.
    void set_prompt_handler(PromptHandlerRequest req) const {
        m_marshal.invoke_sync([this, enabled = req.enabled]() {
            if (enabled) {
                m_manager.set_prompt_handler([this](const native::PermissionPrompt& prompt) {
                    m_events.emit("permission:prompt", native_detail::to_prompt_event(prompt));
                });
                return;
            }
            m_manager.set_prompt_handler({});
            for (const native::PermissionPrompt& p : m_manager.pending()) {
                m_manager.answer(p.id, native::PermissionDecision::Default, false);
            }
        });
    }

private:
    native::PermissionManager& m_manager;
    exec::UiMarshal& m_marshal;
    shell_detail::EventSink m_events;
};

// ---- Zoom -------------------------------------------------------------------------------------------------

struct ZoomFactorRequest {
    double factor = 1.0;
};

struct ZoomLevelRequest {
    double level = 0.0; ///< Chromium zoom level: factor = 1.2^level
};

struct ZoomState {
    std::uint64_t webview_id = 0;
    double factor = 1.0;
    double level = 0.0;
};

/// @brief JS "Zoom.*" for one WebView (ZoomFactor, preset steps, Electron-style levels). Event "zoom:changed"
/// after connect_events(controller, bridge) (UI thread; it takes over events().on_zoom_factor_changed).
class ZoomController {
public:
    BOSON_CONTROLLER("Zoom");

    ZoomController(core::WebView& webview, exec::UiMarshal& marshal) : m_webview(webview), m_marshal(marshal) {}

    friend void connect_events(ZoomController& controller, Bridge& bridge) {
        controller.m_events.set(&bridge);
        core::WebView& webview = controller.m_webview;
        webview.events().on_zoom_factor_changed = [&controller, id = webview.id()](double factor) {
            controller.m_events.emit("zoom:changed", ZoomState{.webview_id = id, .factor = factor, .level = native::zoom_factor_to_level(factor)});
        };
    }

    [[nodiscard]] ZoomState get() const {
        return m_marshal.invoke_sync([this]() { return state(m_webview.zoom_factor().value_or(1.0)); });
    }
    [[nodiscard]] ZoomState set(ZoomFactorRequest req) const { return apply([f = req.factor](double) { return f; }); }
    [[nodiscard]] ZoomState set_level(ZoomLevelRequest req) const {
        return apply([f = native::zoom_level_to_factor(req.level)](double) { return f; });
    }
    [[nodiscard]] ZoomState zoom_in() const { return apply([](double f) { return native::next_zoom_level(f, 1); }); }
    [[nodiscard]] ZoomState zoom_out() const { return apply([](double f) { return native::next_zoom_level(f, -1); }); }
    [[nodiscard]] ZoomState reset() const { return apply([](double) { return 1.0; }); }

private:
    [[nodiscard]] ZoomState state(double factor) const {
        return ZoomState{.webview_id = m_webview.id(), .factor = factor, .level = native::zoom_factor_to_level(factor)};
    }

    template <typename Fn>
    [[nodiscard]] ZoomState apply(Fn next) const {
        return m_marshal.invoke_sync([this, next]() {
            const double target = next(m_webview.zoom_factor().value_or(1.0));
            if (!(target >= native::kMinZoom && target <= native::kMaxZoom)) {
                throw std::invalid_argument("zoom factor must be between 0.25 and 5");
            }
            m_webview.set_zoom_factor(target);
            const ZoomState now = state(m_webview.zoom_factor().value_or(target));
            // put_ZoomFactor does not raise ZoomFactorChanged; report programmatic changes here.
            m_events.emit("zoom:changed", now);
            return now;
        });
    }

    core::WebView& m_webview;
    exec::UiMarshal& m_marshal;
    shell_detail::EventSink m_events;
};

// ---- Native (app metrics, locale, system preferences, session end, close to tray) -------------------------

struct ShutdownBlockRequest {
    std::string reason; ///< empty clears the block
};

struct CloseToTrayRequest {
    bool enabled = true;
};

struct SessionQueryEndEvent {
    bool logoff = false;
    bool close_app = false;
    bool critical = false;
    bool blocked = false;
    std::string block_reason;
};

struct SessionEndEvent {
    bool ending = false;
    bool logoff = false;
    bool close_app = false;
    bool critical = false;
};

struct TrayHiddenEvent {
    bool hidden = false;
};

struct WebNotificationEventPayload {
    std::uint64_t id = 0;
    std::uint64_t webview_id = 0;
    std::string origin;
    std::string title;
    std::string body;
    std::string tag;
};

/// @brief JS "Native.*": app + WebView2 process metrics, locale, system preferences, shutdown blocking and
/// close-to-tray. Construct on the UI thread after window.create(); environments are the WebView2
/// environments whose process trees metrics() reports. Events after connect_events(controller, bridge):
/// "system:preferences-changed", "system:locale-changed", "session:query-end", "session:end",
/// "tray:window-hidden".
class NativeController {
public:
    BOSON_CONTROLLER("Native");

    NativeController(core::Window& window, exec::UiMarshal& marshal, std::vector<std::shared_ptr<core::Environment>> environments = {})
        : m_window(window)
        , m_marshal(marshal)
        , m_environments(std::move(environments))
        , m_session(std::make_unique<native::SessionEndWatcher>(window))
        , m_preferences(std::make_unique<native::SystemPreferencesWatcher>(window)) {
        m_session->set_query_callback([this](native::SessionEndWatcher::Query& query) {
            m_events.emit("session:query-end", SessionQueryEndEvent{
                .logoff = query.info.logoff,
                .close_app = query.info.close_app,
                .critical = query.info.critical,
                .blocked = !query.allow,
                .block_reason = m_session->block_reason(),
            });
        });
        m_session->set_end_callback([this](bool ending, const native::SessionEndInfo& info) {
            m_events.emit("session:end", SessionEndEvent{.ending = ending, .logoff = info.logoff, .close_app = info.close_app, .critical = info.critical});
            if (ending && m_on_session_end) {
                m_on_session_end();
            }
        });
        m_preferences->set_preferences_callback([this](const native::SystemPreferences& prefs) { m_events.emit("system:preferences-changed", prefs); });
        m_preferences->set_locale_callback([this](const native::LocaleInfo& locale) { m_events.emit("system:locale-changed", locale); });
    }

    NativeController(const NativeController&) = delete;
    NativeController& operator=(const NativeController&) = delete;

    friend void connect_events(NativeController& controller, Bridge& bridge) noexcept { controller.m_events.set(&bridge); }

    /// @brief "web-notification:shown" / "web-notification:clicked" / "web-notification:closed" (UI thread).
    friend void connect_web_notifications(NativeController& controller, native::WebNotificationHost& host) {
        host.set_event_callback([&controller](const native::WebNotification& n, native::WebNotificationEvent event) {
            std::string name = "web-notification:";
            name += native::to_string(event);
            controller.m_events.emit(name, WebNotificationEventPayload{
                .id = n.id, .webview_id = n.webview_id, .origin = n.origin, .title = n.title, .body = n.body, .tag = n.tag});
        });
    }

    /// @brief UI thread, before the Router dispatches: enables Native.set_close_to_tray / hide_to_tray.
    void set_close_to_tray(native::CloseToTray* close_to_tray) {
        m_close_to_tray = close_to_tray;
        if (m_close_to_tray != nullptr) {
            m_close_to_tray->set_hidden_callback([this](bool hidden) { m_events.emit("tray:window-hidden", TrayHiddenEvent{.hidden = hidden}); });
        }
    }
    /// @brief C++ hook run from WM_ENDSESSION(ending): flush state synchronously here.
    void set_session_end_handler(std::function<void()> fn) { m_on_session_end = std::move(fn); }
    /// @brief UI thread: replaces the environments metrics() reports (e.g. after creating a second one).
    void set_environments(std::vector<std::shared_ptr<core::Environment>> environments) { m_environments = std::move(environments); }
    [[nodiscard]] native::SessionEndWatcher& session() noexcept { return *m_session; }
    [[nodiscard]] native::SystemPreferencesWatcher& preferences_watcher() noexcept { return *m_preferences; }

    /// @brief Electron app.getAppMetrics(): this process + every WebView2 process (memory, CPU since the
    /// previous call). PIDs are read on the UI thread; the per-process queries run on the pool thread.
    [[nodiscard]] native::AppMetrics metrics() {
        const auto processes = m_marshal.invoke_sync([this]() { return native::collect_webview_processes(m_environments); });
        return m_sampler.sample(processes);
    }

    [[nodiscard]] native::LocaleInfo locale() const { return native::query_locale(); }
    [[nodiscard]] native::SystemPreferences system_preferences() const { return native::query_system_preferences(); }

    [[nodiscard]] ShellResult set_shutdown_block_reason(ShutdownBlockRequest req) const {
        const bool ok = m_marshal.invoke_sync([this, &req]() { return m_session->set_block_reason(req.reason); });
        return ShellResult{.ok = ok, .error = ok ? std::string{} : std::string("ShutdownBlockReasonCreate failed")};
    }

    [[nodiscard]] ShellResult set_close_to_tray_enabled(CloseToTrayRequest req) const {
        return m_marshal.invoke_sync([this, enabled = req.enabled]() {
            if (m_close_to_tray == nullptr) {
                return ShellResult{.ok = false, .error = "close-to-tray is not configured by the app"};
            }
            m_close_to_tray->set_enabled(enabled);
            return ShellResult{.ok = true, .error = {}};
        });
    }

    [[nodiscard]] ShellResult hide_to_tray() const {
        return m_marshal.invoke_sync([this]() {
            if (m_close_to_tray == nullptr) {
                return ShellResult{.ok = false, .error = "close-to-tray is not configured by the app"};
            }
            m_close_to_tray->hide_to_tray();
            return ShellResult{.ok = true, .error = {}};
        });
    }

    [[nodiscard]] ShellResult restore_from_tray() const {
        return m_marshal.invoke_sync([this]() {
            if (m_close_to_tray == nullptr) {
                return ShellResult{.ok = false, .error = "close-to-tray is not configured by the app"};
            }
            m_close_to_tray->restore();
            return ShellResult{.ok = true, .error = {}};
        });
    }

private:
    core::Window& m_window;
    exec::UiMarshal& m_marshal;
    std::vector<std::shared_ptr<core::Environment>> m_environments; // UI thread
    std::unique_ptr<native::SessionEndWatcher> m_session;           // UI thread
    std::unique_ptr<native::SystemPreferencesWatcher> m_preferences;// UI thread
    native::CloseToTray* m_close_to_tray = nullptr;                 // UI thread
    std::function<void()> m_on_session_end;
    native::MetricsSampler m_sampler;
    shell_detail::EventSink m_events;
};

/// @brief Event map of the native controllers, for TsGenerator::generate_with_events.
struct NativeEvents {
    PermissionPromptEvent permission_prompt;
    PermissionChangedEvent permission_changed;
    ZoomState zoom_changed;
    native::SystemPreferences preferences_changed;
    native::LocaleInfo locale_changed;
    SessionQueryEndEvent session_query_end;
    SessionEndEvent session_end;
    TrayHiddenEvent tray_window_hidden;
    WebNotificationEventPayload web_notification_shown;
    WebNotificationEventPayload web_notification_clicked;
    WebNotificationEventPayload web_notification_closed;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::DialogController> {
    using T = boson::ipc::DialogController;
    static constexpr auto value = glz::object(
        "open", &T::open,
        "save", &T::save,
        "pick_folder", &T::pick_folder,
        "message_box", &T::message_box,
        "error_box", &T::error_box);
};

template <>
struct glz::meta<boson::ipc::PermissionController> {
    using T = boson::ipc::PermissionController;
    static constexpr auto value = glz::object(
        "list", &T::list,
        "query", &T::query,
        "set", &T::set,
        "reset", &T::reset,
        "clear", &T::clear,
        "pending", &T::pending,
        "respond", &T::respond,
        "set_prompt_handler", &T::set_prompt_handler);
};

template <>
struct glz::meta<boson::ipc::ZoomController> {
    using T = boson::ipc::ZoomController;
    static constexpr auto value = glz::object(
        "get", &T::get,
        "set", &T::set,
        "set_level", &T::set_level,
        "zoom_in", &T::zoom_in,
        "zoom_out", &T::zoom_out,
        "reset", &T::reset);
};

template <>
struct glz::meta<boson::ipc::NativeController> {
    using T = boson::ipc::NativeController;
    static constexpr auto value = glz::object(
        "metrics", &T::metrics,
        "locale", &T::locale,
        "system_preferences", &T::system_preferences,
        "set_shutdown_block_reason", &T::set_shutdown_block_reason,
        "set_close_to_tray_enabled", &T::set_close_to_tray_enabled,
        "hide_to_tray", &T::hide_to_tray,
        "restore_from_tray", &T::restore_from_tray);
};

template <>
struct glz::meta<boson::ipc::NativeEvents> {
    using T = boson::ipc::NativeEvents;
    static constexpr auto value = glz::object(
        "permission:prompt", &T::permission_prompt,
        "permission:changed", &T::permission_changed,
        "zoom:changed", &T::zoom_changed,
        "system:preferences-changed", &T::preferences_changed,
        "system:locale-changed", &T::locale_changed,
        "session:query-end", &T::session_query_end,
        "session:end", &T::session_end,
        "tray:window-hidden", &T::tray_window_hidden,
        "web-notification:shown", &T::web_notification_shown,
        "web-notification:clicked", &T::web_notification_clicked,
        "web-notification:closed", &T::web_notification_closed);
};
