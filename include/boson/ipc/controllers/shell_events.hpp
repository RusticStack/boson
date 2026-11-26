#pragma once

#include <boson/ipc/controllers/notification_controller.hpp>
#include <boson/ipc/controllers/power_controller.hpp>
#include <boson/ipc/controllers/taskbar_controller.hpp>

#include <glaze/glaze.hpp>

namespace boson::ipc {

/// @brief Event map of the shell-integration controllers, for TsGenerator::generate_with_events.
struct ShellIntegrationEvents {
    ThumbnailClickEvent thumbnail_click;
    ToastActivatedEvent notification_activated;
    ToastDismissedEvent notification_dismissed;
    ToastFailedEvent notification_failed;
    PowerEventPayload power_suspend;
    PowerEventPayload power_resume;
    PowerEventPayload power_source_changed;
    PowerEventPayload power_battery_changed;
    PowerEventPayload power_battery_saver_changed;
    PowerEventPayload power_display_changed;
    PowerEventPayload power_lock;
    PowerEventPayload power_unlock;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::ShellIntegrationEvents> {
    using T = boson::ipc::ShellIntegrationEvents;
    static constexpr auto value = glz::object(
        "taskbar:thumbnail-click", &T::thumbnail_click,
        "notification:activated", &T::notification_activated,
        "notification:dismissed", &T::notification_dismissed,
        "notification:failed", &T::notification_failed,
        "power:suspend", &T::power_suspend,
        "power:resume", &T::power_resume,
        "power:source-changed", &T::power_source_changed,
        "power:battery-changed", &T::power_battery_changed,
        "power:battery-saver-changed", &T::power_battery_saver_changed,
        "power:display-changed", &T::power_display_changed,
        "power:lock", &T::power_lock,
        "power:unlock", &T::power_unlock);
};
