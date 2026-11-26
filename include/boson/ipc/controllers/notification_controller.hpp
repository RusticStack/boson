#pragma once

#include <boson/core/result.hpp>
#include <boson/core/window.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/controllers/shell_common.hpp>
#include <boson/ipc/export.hpp>
#include <boson/native/notification.hpp>
#include <boson/shell/toast.hpp>
#include <boson/shell/toast_activation.hpp>

#include <glaze/glaze.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace boson::ipc {

struct ToastButtonSpec {
    std::string content;
    std::string arguments;
    std::string activation = "foreground"; ///< foreground | background | protocol
    std::string input_id;
    std::string image;
    bool context_menu = false;
};

struct ToastSelectionSpec {
    std::string id;
    std::string content;
};

struct ToastInputSpec {
    std::string id;
    std::string title;
    std::string placeholder;
    std::string default_value;
    std::vector<ToastSelectionSpec> options;
};

struct ToastProgressSpec {
    std::string title;
    std::optional<double> value; ///< 0..1; null = indeterminate
    std::string value_label;
    std::string status;
};

struct ToastRequest {
    std::string title;
    std::string body;
    std::string attribution;
    std::string app_logo;
    bool app_logo_circle = false;
    std::string hero_image;
    std::string inline_image;
    std::string launch_arguments;
    std::vector<ToastButtonSpec> buttons;
    std::vector<ToastInputSpec> inputs;
    std::optional<ToastProgressSpec> progress;
    std::string scenario = "default"; ///< default | reminder | alarm | incoming_call | urgent
    std::string duration = "default"; ///< default | short | long
    bool silent = false;
    std::string sound;
    bool loop_sound = false;
    std::string tag;
    std::string group;
    bool suppress_popup = false;
    std::optional<std::int64_t> expires_after_seconds;
    /// When the WinRT toast cannot be shown, fall back to a Shell_NotifyIcon balloon (title/body only).
    bool fallback = true;
    std::string level = "info"; ///< balloon icon for the fallback
};

struct ToastResponse {
    bool ok = false;
    bool used_fallback = false;
    std::string tag;
    std::string group;
    std::string error;
};

struct ToastUpdateRequest {
    std::string tag;
    std::string group;
    ToastProgressSpec progress;
};

struct ToastTargetRequest {
    std::string tag;
    std::string group;
};

struct ToastSupportResponse {
    bool supported = false;
    std::string app_user_model_id;
    std::string setting; ///< enabled | disabled_for_application | disabled_for_user | disabled_by_group_policy | disabled_by_manifest | unknown
};

struct ToastActivatedEvent {
    std::string tag;
    std::string group;
    std::string arguments;
    std::map<std::string, std::string> user_input;
    std::string source; ///< "toast" (in-process event) | "activator" (COM INotificationActivationCallback)
};

struct ToastDismissedEvent {
    std::string tag;
    std::string group;
    std::string reason; ///< user_canceled | application_hidden | timed_out
};

struct ToastFailedEvent {
    std::string tag;
    std::string group;
    std::int32_t error = 0;
};

namespace shell_detail {

[[nodiscard]] inline shell::ToastActivationType parse_toast_activation(std::string_view s) {
    if (s.empty() || s == "foreground") return shell::ToastActivationType::Foreground;
    if (s == "background") return shell::ToastActivationType::Background;
    if (s == "protocol") return shell::ToastActivationType::Protocol;
    throw std::invalid_argument("Unsupported activation type: " + std::string(s));
}

[[nodiscard]] inline shell::ToastScenario parse_toast_scenario(std::string_view s) {
    if (s.empty() || s == "default") return shell::ToastScenario::Default;
    if (s == "reminder") return shell::ToastScenario::Reminder;
    if (s == "alarm") return shell::ToastScenario::Alarm;
    if (s == "incoming_call" || s == "incomingCall") return shell::ToastScenario::IncomingCall;
    if (s == "urgent") return shell::ToastScenario::Urgent;
    throw std::invalid_argument("Unsupported scenario: " + std::string(s));
}

[[nodiscard]] inline shell::ToastDuration parse_toast_duration(std::string_view s) {
    if (s.empty() || s == "default") return shell::ToastDuration::Default;
    if (s == "short") return shell::ToastDuration::Short;
    if (s == "long") return shell::ToastDuration::Long;
    throw std::invalid_argument("Unsupported duration: " + std::string(s));
}

[[nodiscard]] inline shell::ToastProgress to_progress(const ToastProgressSpec& spec) {
    return shell::ToastProgress{.title = spec.title, .value = spec.value, .value_label = spec.value_label, .status = spec.status};
}

[[nodiscard]] inline shell::ToastContent to_toast_content(const ToastRequest& req) {
    if (req.buttons.size() > shell::kMaxToastButtons) {
        throw std::invalid_argument("At most 5 toast buttons are supported");
    }
    if (req.inputs.size() > shell::kMaxToastInputs) {
        throw std::invalid_argument("At most 5 toast inputs are supported");
    }
    shell::ToastContent content;
    content.title = req.title;
    content.body = req.body;
    content.attribution = req.attribution;
    content.app_logo = req.app_logo;
    content.app_logo_circle = req.app_logo_circle;
    content.hero_image = req.hero_image;
    content.inline_image = req.inline_image;
    content.launch_arguments = req.launch_arguments;
    for (const auto& b : req.buttons) {
        content.buttons.push_back(shell::ToastButton{
            .content = b.content,
            .arguments = b.arguments,
            .activation = parse_toast_activation(b.activation),
            .input_id = b.input_id,
            .image = b.image,
            .context_menu = b.context_menu,
        });
    }
    for (const auto& i : req.inputs) {
        shell::ToastInput input{.id = i.id, .title = i.title, .placeholder = i.placeholder, .default_value = i.default_value, .options = {}};
        for (const auto& o : i.options) {
            input.options.push_back(shell::ToastSelectionOption{.id = o.id, .content = o.content});
        }
        content.inputs.push_back(std::move(input));
    }
    if (req.progress.has_value()) {
        content.progress = to_progress(*req.progress);
    }
    content.scenario = parse_toast_scenario(req.scenario);
    content.duration = parse_toast_duration(req.duration);
    content.silent = req.silent;
    content.sound = req.sound;
    content.loop_sound = req.loop_sound;
    content.tag = req.tag;
    content.group = req.group;
    content.suppress_popup = req.suppress_popup;
    if (req.expires_after_seconds.has_value() && *req.expires_after_seconds > 0) {
        content.expires_after = std::chrono::seconds{*req.expires_after_seconds};
    }
    return content;
}

[[nodiscard]] inline std::string toast_setting_name(shell::ToastSetting setting) {
    switch (setting) {
    case shell::ToastSetting::Enabled: return "enabled";
    case shell::ToastSetting::DisabledForApplication: return "disabled_for_application";
    case shell::ToastSetting::DisabledForUser: return "disabled_for_user";
    case shell::ToastSetting::DisabledByGroupPolicy: return "disabled_by_group_policy";
    case shell::ToastSetting::DisabledByManifest: return "disabled_by_manifest";
    case shell::ToastSetting::Unknown: return "unknown";
    }
    return "unknown";
}

[[nodiscard]] inline std::string hresult_text(HRESULT hr) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

} // namespace shell_detail

/// @brief WinRT toasts with a Shell_NotifyIcon balloon fallback. Construct on the UI thread after
/// set_app_user_model_id() and window.create(). Events: "notification:activated", "notification:dismissed",
/// "notification:failed" (after connect_events(controller, bridge)).
class NotificationController {
public:
    BOSON_CONTROLLER("Notification");

    NotificationController(core::Window& window, exec::UiMarshal& marshal, std::wstring app_id = {})
        : m_window(window), m_marshal(marshal), m_toasts(std::make_unique<shell::ToastNotifier>(std::move(app_id))),
          m_fallback(std::make_unique<native::Notifier>(window.hwnd())) {
        m_toasts->set_event_callback([this](const shell::ToastEvent& e) {
            switch (e.kind) {
            case shell::ToastEventKind::Activated:
                m_events.emit("notification:activated", ToastActivatedEvent{
                    .tag = e.tag, .group = e.group, .arguments = e.arguments, .user_input = e.user_input, .source = "toast"});
                break;
            case shell::ToastEventKind::Dismissed:
                m_events.emit("notification:dismissed", ToastDismissedEvent{
                    .tag = e.tag,
                    .group = e.group,
                    .reason = e.reason == shell::ToastDismissReason::TimedOut            ? "timed_out"
                              : e.reason == shell::ToastDismissReason::ApplicationHidden ? "application_hidden"
                                                                                         : "user_canceled",
                });
                break;
            case shell::ToastEventKind::Failed:
                m_events.emit("notification:failed", ToastFailedEvent{
                    .tag = e.tag, .group = e.group, .error = static_cast<std::int32_t>(e.error)});
                break;
            }
        });
    }

    ~NotificationController() { m_toasts->set_event_callback(nullptr); }

    NotificationController(const NotificationController&) = delete;
    NotificationController& operator=(const NotificationController&) = delete;

    friend void connect_events(NotificationController& controller, Bridge& bridge) noexcept { controller.m_events.set(&bridge); }
    /// @brief Forward a COM activation (ToastActivator handler) to the frontend as "notification:activated".
    friend void deliver_activation(NotificationController& controller, const shell::ToastActivation& activation) {
        controller.m_events.emit("notification:activated", ToastActivatedEvent{
            .tag = {}, .group = {}, .arguments = activation.arguments, .user_input = activation.user_input, .source = "activator"});
    }
    friend shell::ToastNotifier& native_toasts(NotificationController& controller) noexcept { return *controller.m_toasts; }

    [[nodiscard]] ToastResponse show(ToastRequest req) const {
        const shell::ToastContent content = shell_detail::to_toast_content(req);
        const shell::ToastShowResult shown = m_toasts->show(content);
        if (shown.ok) {
            return ToastResponse{.ok = true, .used_fallback = false, .tag = shown.tag, .group = shown.group, .error = {}};
        }
        ToastResponse response{.ok = false, .used_fallback = false, .tag = shown.tag, .group = shown.group,
                               .error = shell_detail::hresult_text(shown.hr)};
        if (req.fallback) {
            native::NotificationOptions opts{
                .title = req.title,
                .body = req.body,
                .level = req.level == "warning" ? native::NotificationLevel::Warning
                         : req.level == "error" ? native::NotificationLevel::Error
                         : req.level == "none"  ? native::NotificationLevel::None
                                                : native::NotificationLevel::Info,
                .silent = req.silent,
            };
            response.ok = m_marshal.invoke_sync([this, opts = std::move(opts)]() {
                m_fallback->set_owner(m_window.hwnd());
                return m_fallback->show(opts);
            });
            response.used_fallback = response.ok;
        }
        return response;
    }

    [[nodiscard]] ShellResult update_progress(ToastUpdateRequest req) const {
        if (req.tag.empty()) {
            throw std::invalid_argument("tag must not be empty");
        }
        return ShellResult{.ok = m_toasts->update_progress(req.tag, req.group, shell_detail::to_progress(req.progress))};
    }

    [[nodiscard]] ShellResult hide(ToastTargetRequest req) const { return ShellResult{.ok = m_toasts->hide(req.tag, req.group)}; }
    [[nodiscard]] ShellResult remove(ToastTargetRequest req) const { return ShellResult{.ok = m_toasts->remove(req.tag, req.group)}; }
    [[nodiscard]] ShellResult clear_all() const { return ShellResult{.ok = m_toasts->clear_all()}; }

    [[nodiscard]] ToastSupportResponse support() const {
        return ToastSupportResponse{
            .supported = shell::ToastNotifier::is_supported(),
            .app_user_model_id = core::wide_to_utf8(m_toasts->app_user_model_id()),
            .setting = shell_detail::toast_setting_name(m_toasts->setting()),
        };
    }

private:
    core::Window& m_window;
    exec::UiMarshal& m_marshal;
    std::unique_ptr<shell::ToastNotifier> m_toasts;
    std::unique_ptr<native::Notifier> m_fallback; // UI thread only
    shell_detail::EventSink m_events;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::NotificationController> {
    using T = boson::ipc::NotificationController;
    static constexpr auto value = glz::object(
        "show", &T::show,
        "update_progress", &T::update_progress,
        "hide", &T::hide,
        "remove", &T::remove,
        "clear_all", &T::clear_all,
        "support", &T::support);
};
