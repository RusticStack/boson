#pragma once

#include <boson/core/result.hpp>
#include <boson/core/window.hpp>
#include <boson/ipc/controllers/shell_common.hpp>
#include <boson/ipc/export.hpp>
#include <boson/shell/power.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace boson::ipc {

struct PowerStatusResponse {
    bool on_ac = true;
    bool has_battery = false;
    std::optional<int> battery_percent;
    bool charging = false;
    bool battery_saver = false;
    std::optional<std::uint32_t> battery_seconds_remaining;
    std::string display = "on"; ///< on | off | dimmed
    bool session_locked = false;
    std::uint64_t idle_ms = 0;
};

/// @brief Payload of every "power:*" event.
struct PowerEventPayload {
    std::string kind; ///< suspend | resume | source-changed | battery-changed | battery-saver-changed | display-changed | lock | unlock
    PowerStatusResponse status;
};

struct IdleTimeResponse {
    std::uint64_t idle_ms = 0;
};

struct IdleStateRequest {
    std::uint32_t threshold_seconds = 60;
};

struct IdleStateResponse {
    std::string state = "active"; ///< active | idle | locked
    std::uint64_t idle_ms = 0;
};

struct PowerBlockerRequest {
    std::string type = "prevent_app_suspension"; ///< prevent_app_suspension | prevent_display_sleep
    std::string reason;
};

struct PowerBlockerIdRequest {
    std::uint32_t id = 0;
};

struct PowerBlockerResponse {
    std::uint32_t id = 0; ///< 0 = failed
    bool started = false;
};

namespace shell_detail {

[[nodiscard]] inline const char* display_name(shell::DisplayState state) noexcept {
    switch (state) {
    case shell::DisplayState::Off: return "off";
    case shell::DisplayState::Dimmed: return "dimmed";
    case shell::DisplayState::On: return "on";
    }
    return "on";
}

[[nodiscard]] inline const char* power_event_name(shell::PowerEventKind kind) noexcept {
    switch (kind) {
    case shell::PowerEventKind::Suspend: return "power:suspend";
    case shell::PowerEventKind::Resume: return "power:resume";
    case shell::PowerEventKind::PowerSourceChanged: return "power:source-changed";
    case shell::PowerEventKind::BatteryPercentChanged: return "power:battery-changed";
    case shell::PowerEventKind::BatterySaverChanged: return "power:battery-saver-changed";
    case shell::PowerEventKind::DisplayStateChanged: return "power:display-changed";
    case shell::PowerEventKind::SessionLocked: return "power:lock";
    case shell::PowerEventKind::SessionUnlocked: return "power:unlock";
    }
    return "power:unknown";
}

[[nodiscard]] inline PowerStatusResponse to_status_response(const shell::PowerStatus& s, shell::DisplayState display, bool locked) {
    return PowerStatusResponse{
        .on_ac = s.on_ac,
        .has_battery = s.has_battery,
        .battery_percent = s.battery_percent,
        .charging = s.charging,
        .battery_saver = s.battery_saver,
        .battery_seconds_remaining = s.battery_seconds_remaining,
        .display = display_name(display),
        .session_locked = locked,
        .idle_ms = static_cast<std::uint64_t>(shell::user_idle_time().count()),
    };
}

[[nodiscard]] inline shell::PowerSaveBlockerType parse_blocker_type(std::string_view type) {
    if (type.empty() || type == "prevent_app_suspension") return shell::PowerSaveBlockerType::PreventAppSuspension;
    if (type == "prevent_display_sleep") return shell::PowerSaveBlockerType::PreventDisplaySleep;
    throw std::invalid_argument("Unsupported power save blocker type: " + std::string(type));
}

} // namespace shell_detail

/// @brief Power/session state, "power:*" events and power-save blockers. Construct on the UI thread after
/// window.create(); call connect_events(controller, bridge) to receive events in the frontend.
class PowerController {
public:
    BOSON_CONTROLLER("Power");

    explicit PowerController(core::Window& window) : m_monitor(std::make_unique<shell::PowerMonitor>(window)) {
        m_monitor->on([this](const shell::PowerEvent& e) {
            const char* name = shell_detail::power_event_name(e.kind);
            std::string_view kind(name);
            kind.remove_prefix(6); // "power:"
            m_events.emit(name, PowerEventPayload{
                .kind = std::string(kind),
                .status = shell_detail::to_status_response(e.status, e.display, m_monitor->is_session_locked()),
            });
        });
    }

    PowerController(const PowerController&) = delete;
    PowerController& operator=(const PowerController&) = delete;

    friend void connect_events(PowerController& controller, Bridge& bridge) noexcept { controller.m_events.set(&bridge); }
    friend shell::PowerMonitor& native_power_monitor(PowerController& controller) noexcept { return *controller.m_monitor; }
    friend shell::PowerSaveBlockers& native_power_blockers(PowerController& controller) noexcept { return controller.m_blockers; }

    [[nodiscard]] PowerStatusResponse status() const {
        return shell_detail::to_status_response(shell::power_status(), m_monitor->display_state(), m_monitor->is_session_locked());
    }

    [[nodiscard]] IdleTimeResponse idle_time() const {
        return IdleTimeResponse{.idle_ms = static_cast<std::uint64_t>(shell::user_idle_time().count())};
    }

    [[nodiscard]] IdleStateResponse idle_state(IdleStateRequest req) const {
        const auto idle = static_cast<std::uint64_t>(shell::user_idle_time().count());
        if (m_monitor->is_session_locked()) {
            return IdleStateResponse{.state = "locked", .idle_ms = idle};
        }
        const bool is_idle = idle >= static_cast<std::uint64_t>(req.threshold_seconds) * 1000u;
        return IdleStateResponse{.state = is_idle ? "idle" : "active", .idle_ms = idle};
    }

    [[nodiscard]] PowerBlockerResponse start_blocker(PowerBlockerRequest req) {
        const auto type = shell_detail::parse_blocker_type(req.type);
        const auto id = m_blockers.start(type, core::utf8_to_wide(req.reason.empty() ? "Boson app request" : req.reason));
        return PowerBlockerResponse{.id = id, .started = id != 0};
    }

    [[nodiscard]] ShellResult stop_blocker(PowerBlockerIdRequest req) { return ShellResult{.ok = m_blockers.stop(req.id)}; }

    [[nodiscard]] PowerBlockerResponse is_blocker_started(PowerBlockerIdRequest req) const {
        return PowerBlockerResponse{.id = req.id, .started = m_blockers.is_started(req.id)};
    }

private:
    std::unique_ptr<shell::PowerMonitor> m_monitor;
    shell::PowerSaveBlockers m_blockers;
    shell_detail::EventSink m_events;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::PowerController> {
    using T = boson::ipc::PowerController;
    static constexpr auto value = glz::object(
        "status", &T::status,
        "idle_time", &T::idle_time,
        "idle_state", &T::idle_state,
        "start_blocker", &T::start_blocker,
        "stop_blocker", &T::stop_blocker,
        "is_blocker_started", &T::is_blocker_started);
};
