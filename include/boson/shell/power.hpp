#pragma once

#include <windows.h>

#include <boson/core/window.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::shell {

struct PowerStatus {
    bool on_ac = true;
    bool has_battery = false;
    std::optional<int> battery_percent;     ///< 0..100
    bool charging = false;
    bool battery_saver = false;             ///< Energy/Battery saver is on
    std::optional<std::uint32_t> battery_seconds_remaining;
};

/// @brief GetSystemPowerStatus. Any thread.
[[nodiscard]] PowerStatus power_status() noexcept;
/// @brief Time since the last keyboard/mouse input in this session (GetLastInputInfo). Any thread.
[[nodiscard]] std::chrono::milliseconds user_idle_time() noexcept;
/// @brief WTSQuerySessionInformation(WTSSessionInfoEx) for the current session; std::nullopt if unknown.
[[nodiscard]] std::optional<bool> is_session_locked() noexcept;

enum class DisplayState { Off, On, Dimmed };

enum class PowerEventKind {
    Suspend,             ///< system is about to sleep/hibernate
    Resume,              ///< system woke up (sent once per resume)
    PowerSourceChanged,  ///< status.on_ac changed
    BatteryPercentChanged,
    BatterySaverChanged,
    DisplayStateChanged, ///< display
    SessionLocked,
    SessionUnlocked,
};

struct PowerEvent {
    PowerEventKind kind = PowerEventKind::Suspend;
    PowerStatus status;
    DisplayState display = DisplayState::On;
};

/// @brief Turns WM_POWERBROADCAST / power-setting / WTS session messages for a Window into PowerEvents.
/// Registers RegisterSuspendResumeNotification, RegisterPowerSettingNotification (AC/DC, battery %,
/// display state, energy saver) and WTSRegisterSessionNotification for the window. UI thread only for
/// construction, on() and off(); the state accessors are thread-safe. Must not outlive the Window.
class PowerMonitor {
public:
    using ListenerId = std::uint64_t;
    using Listener = std::function<void(const PowerEvent& event)>;

    explicit PowerMonitor(core::Window& window);
    ~PowerMonitor();

    PowerMonitor(const PowerMonitor&) = delete;
    PowerMonitor& operator=(const PowerMonitor&) = delete;
    PowerMonitor(PowerMonitor&&) = delete;
    PowerMonitor& operator=(PowerMonitor&&) = delete;

    ListenerId on(Listener listener);
    bool off(ListenerId id) noexcept;

    [[nodiscard]] bool is_session_locked() const noexcept { return m_locked.load(std::memory_order_relaxed); }
    [[nodiscard]] bool is_suspended() const noexcept { return m_suspended.load(std::memory_order_relaxed); }
    [[nodiscard]] DisplayState display_state() const noexcept { return m_display.load(std::memory_order_relaxed); }
    /// @brief Last status seen by the monitor (refreshed on every power message).
    [[nodiscard]] PowerStatus last_status() const;

    /// @brief Registration diagnostics.
    [[nodiscard]] bool suspend_resume_registered() const noexcept { return m_suspend_notify != nullptr; }
    [[nodiscard]] bool session_registered() const noexcept { return m_session_registered; }
    [[nodiscard]] std::size_t power_setting_registrations() const noexcept { return m_setting_notify.size(); }

private:
    std::optional<LRESULT> on_message(UINT msg, WPARAM w_param, LPARAM l_param);
    void handle_power_setting(const POWERBROADCAST_SETTING& setting);
    void emit(PowerEventKind kind);
    void refresh_status();

    core::Window& m_window;
    core::Window::ListenerId m_hook = 0;
    HPOWERNOTIFY m_suspend_notify = nullptr;
    std::vector<HPOWERNOTIFY> m_setting_notify;
    bool m_session_registered = false;

    std::atomic<bool> m_locked{false};
    std::atomic<bool> m_suspended{false};
    std::atomic<DisplayState> m_display{DisplayState::On};
    bool m_display_known = false;
    bool m_resume_sent = true;
    mutable std::mutex m_status_mutex;
    PowerStatus m_status;

    struct Entry {
        ListenerId id = 0;
        Listener fn;
    };
    std::vector<Entry> m_listeners;
    ListenerId m_next_id = 1;
};

enum class PowerSaveBlockerType {
    PreventAppSuspension, ///< keep the system awake (PowerRequestSystemRequired)
    PreventDisplaySleep,  ///< keep the display (and system) on (PowerRequestDisplayRequired + SystemRequired)
};

/// @brief RAII power request (PowerCreateRequest / PowerSetRequest). Released on destruction; Windows also
/// drops it when the process exits.
class PowerSaveBlocker {
public:
    PowerSaveBlocker(PowerSaveBlockerType type, std::wstring_view reason);
    ~PowerSaveBlocker();

    PowerSaveBlocker(const PowerSaveBlocker&) = delete;
    PowerSaveBlocker& operator=(const PowerSaveBlocker&) = delete;
    PowerSaveBlocker(PowerSaveBlocker&&) = delete;
    PowerSaveBlocker& operator=(PowerSaveBlocker&&) = delete;

    [[nodiscard]] bool is_active() const noexcept { return m_active; }
    [[nodiscard]] PowerSaveBlockerType type() const noexcept { return m_type; }

private:
    PowerSaveBlockerType m_type;
    HANDLE m_request = INVALID_HANDLE_VALUE;
    bool m_active = false;
};

/// @brief Thread-safe set of PowerSaveBlockers addressed by id (for IPC: start/stop/is_started).
class PowerSaveBlockers {
public:
    using Id = std::uint32_t;

    PowerSaveBlockers() = default;
    PowerSaveBlockers(const PowerSaveBlockers&) = delete;
    PowerSaveBlockers& operator=(const PowerSaveBlockers&) = delete;

    /// @brief Returns the new id, or 0 when the request could not be set.
    Id start(PowerSaveBlockerType type, std::wstring_view reason);
    bool stop(Id id);
    [[nodiscard]] bool is_started(Id id) const;
    [[nodiscard]] std::size_t size() const;
    void stop_all();

private:
    struct Entry {
        Id id = 0;
        std::unique_ptr<PowerSaveBlocker> blocker;
    };
    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
    Id m_next_id = 1;
};

} // namespace boson::shell
