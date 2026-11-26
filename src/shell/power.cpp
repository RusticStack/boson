#include <boson/shell/power.hpp>

#include <boson/core/result.hpp>

#include <WtsApi32.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace boson::shell {

namespace {

// Local copies so no INITGUID translation unit is needed (values from winnt.h).
constexpr GUID kAcDcPowerSource{0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};
constexpr GUID kBatteryPercentageRemaining{0xA7AD8041, 0xB45A, 0x4CAE, {0x87, 0xA3, 0xEE, 0xCB, 0xB4, 0x68, 0xA9, 0xE1}};
constexpr GUID kConsoleDisplayState{0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
constexpr GUID kPowerSavingStatus{0xe00958c0, 0xc213, 0x4ace, {0xac, 0x77, 0xfe, 0xcc, 0xed, 0x2e, 0xee, 0xa5}};
constexpr GUID kEnergySaverStatus{0x550e8400, 0xe29b, 0x41d4, {0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00}};

DWORD setting_dword(const POWERBROADCAST_SETTING& setting) noexcept {
    if (setting.DataLength < sizeof(DWORD)) {
        return 0;
    }
    DWORD value = 0;
    std::memcpy(&value, setting.Data, sizeof(DWORD));
    return value;
}

} // namespace

PowerStatus power_status() noexcept {
    PowerStatus status;
    SYSTEM_POWER_STATUS sps{};
    if (!::GetSystemPowerStatus(&sps)) {
        return status;
    }
    status.on_ac = sps.ACLineStatus != 0; // 255 (unknown) counts as AC: desktops without battery report 1
    status.has_battery = (sps.BatteryFlag & 128) == 0 && sps.BatteryFlag != 255;
    if (status.has_battery && sps.BatteryLifePercent <= 100) {
        status.battery_percent = sps.BatteryLifePercent;
    }
    status.charging = (sps.BatteryFlag & 8) != 0;
    status.battery_saver = sps.SystemStatusFlag != 0;
    if (status.has_battery && sps.BatteryLifeTime != static_cast<DWORD>(-1)) {
        status.battery_seconds_remaining = sps.BatteryLifeTime;
    }
    return status;
}

std::chrono::milliseconds user_idle_time() noexcept {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!::GetLastInputInfo(&info)) {
        return std::chrono::milliseconds{0};
    }
    // Both are 32-bit tick counts; unsigned subtraction handles the 49.7-day wrap.
    const DWORD now = static_cast<DWORD>(::GetTickCount64());
    return std::chrono::milliseconds{static_cast<DWORD>(now - info.dwTime)};
}

std::optional<bool> is_session_locked() noexcept {
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, &buffer, &bytes) ||
        buffer == nullptr) {
        return std::nullopt;
    }
    std::optional<bool> locked;
    if (bytes >= sizeof(WTSINFOEXW)) {
        const auto* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
        if (info->Level == 1) {
            const LONG flags = info->Data.WTSInfoExLevel1.SessionFlags;
            if (flags == WTS_SESSIONSTATE_LOCK) {
                locked = true;
            } else if (flags == WTS_SESSIONSTATE_UNLOCK) {
                locked = false;
            }
        }
    }
    ::WTSFreeMemory(buffer);
    return locked;
}

// ---- PowerMonitor ----------------------------------------------------------------------------------------

PowerMonitor::PowerMonitor(core::Window& window) : m_window(window), m_status(power_status()) {
    m_locked.store(shell::is_session_locked().value_or(false), std::memory_order_relaxed);
    m_hook = m_window.add_message_hook([this](UINT msg, WPARAM w_param, LPARAM l_param) {
        return on_message(msg, w_param, l_param);
    });
    const HWND hwnd = m_window.hwnd();
    if (hwnd == nullptr) {
        return;
    }
    m_suspend_notify = ::RegisterSuspendResumeNotification(hwnd, DEVICE_NOTIFY_WINDOW_HANDLE);
    for (const GUID* guid : {&kAcDcPowerSource, &kBatteryPercentageRemaining, &kConsoleDisplayState, &kPowerSavingStatus,
                             &kEnergySaverStatus}) {
        if (HPOWERNOTIFY handle = ::RegisterPowerSettingNotification(hwnd, guid, DEVICE_NOTIFY_WINDOW_HANDLE)) {
            m_setting_notify.push_back(handle);
        }
    }
    m_session_registered = ::WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION) != FALSE;
}

PowerMonitor::~PowerMonitor() {
    m_window.remove_message_hook(m_hook);
    for (HPOWERNOTIFY handle : m_setting_notify) {
        ::UnregisterPowerSettingNotification(handle);
    }
    if (m_suspend_notify != nullptr) {
        ::UnregisterSuspendResumeNotification(m_suspend_notify);
    }
    const HWND hwnd = m_window.hwnd();
    if (m_session_registered && hwnd != nullptr && ::IsWindow(hwnd)) {
        ::WTSUnRegisterSessionNotification(hwnd);
    }
}

PowerMonitor::ListenerId PowerMonitor::on(Listener listener) {
    const ListenerId id = m_next_id++;
    m_listeners.push_back(Entry{id, std::move(listener)});
    return id;
}

bool PowerMonitor::off(ListenerId id) noexcept {
    for (auto& entry : m_listeners) {
        if (entry.id == id && entry.fn) {
            entry.fn = nullptr; // compacted in emit(), so off() is safe inside a listener
            return true;
        }
    }
    return false;
}

PowerStatus PowerMonitor::last_status() const {
    std::scoped_lock lock(m_status_mutex);
    return m_status;
}

void PowerMonitor::refresh_status() {
    const PowerStatus fresh = power_status();
    std::scoped_lock lock(m_status_mutex);
    m_status = fresh;
}

void PowerMonitor::emit(PowerEventKind kind) {
    PowerEvent event;
    event.kind = kind;
    event.status = last_status();
    event.display = display_state();
    // Index loop: listeners may subscribe during dispatch.
    const std::size_t count = m_listeners.size();
    for (std::size_t i = 0; i < count && i < m_listeners.size(); ++i) {
        if (m_listeners[i].fn) {
            const Listener fn = m_listeners[i].fn;
            fn(event);
        }
    }
    std::erase_if(m_listeners, [](const Entry& e) { return !e.fn; });
}

void PowerMonitor::handle_power_setting(const POWERBROADCAST_SETTING& setting) {
    const DWORD value = setting_dword(setting);
    if (::IsEqualGUID(setting.PowerSetting, kAcDcPowerSource)) {
        bool changed = false;
        {
            const bool on_ac = value == PoAc;
            PowerStatus fresh = power_status();
            fresh.on_ac = on_ac; // the notification is authoritative
            std::scoped_lock lock(m_status_mutex);
            changed = m_status.on_ac != on_ac;
            m_status = fresh;
        }
        if (changed) {
            emit(PowerEventKind::PowerSourceChanged);
        }
    } else if (::IsEqualGUID(setting.PowerSetting, kBatteryPercentageRemaining)) {
        bool changed = false;
        {
            PowerStatus fresh = power_status();
            fresh.battery_percent = static_cast<int>((std::min)(value, DWORD{100}));
            std::scoped_lock lock(m_status_mutex);
            changed = m_status.battery_percent != fresh.battery_percent;
            m_status = fresh;
        }
        if (changed) {
            emit(PowerEventKind::BatteryPercentChanged);
        }
    } else if (::IsEqualGUID(setting.PowerSetting, kConsoleDisplayState)) {
        const DisplayState next = value == 0 ? DisplayState::Off : (value == 2 ? DisplayState::Dimmed : DisplayState::On);
        const DisplayState previous = m_display.exchange(next, std::memory_order_relaxed);
        const bool first = !m_display_known;
        m_display_known = true;
        if (!first && previous != next) {
            emit(PowerEventKind::DisplayStateChanged);
        }
    } else if (::IsEqualGUID(setting.PowerSetting, kPowerSavingStatus) || ::IsEqualGUID(setting.PowerSetting, kEnergySaverStatus)) {
        // GUID_POWER_SAVING_STATUS: 0/1. GUID_ENERGY_SAVER_STATUS: 0 off, 1 standard, 2 high savings.
        const bool on = value != 0;
        bool changed = false;
        {
            std::scoped_lock lock(m_status_mutex);
            changed = m_status.battery_saver != on;
            m_status.battery_saver = on;
        }
        if (changed) {
            emit(PowerEventKind::BatterySaverChanged);
        }
    }
}

std::optional<LRESULT> PowerMonitor::on_message(UINT msg, WPARAM w_param, LPARAM l_param) {
    if (msg == WM_POWERBROADCAST) {
        switch (w_param) {
        case PBT_APMSUSPEND:
            m_suspended.store(true, std::memory_order_relaxed);
            m_resume_sent = false;
            emit(PowerEventKind::Suspend);
            break;
        case PBT_APMRESUMEAUTOMATIC:
        case PBT_APMRESUMESUSPEND:
            // Both arrive after a user-initiated wake; report one Resume per Suspend.
            m_suspended.store(false, std::memory_order_relaxed);
            if (!m_resume_sent) {
                m_resume_sent = true;
                refresh_status();
                emit(PowerEventKind::Resume);
            }
            break;
        case PBT_APMPOWERSTATUSCHANGE: {
            const PowerStatus fresh = power_status();
            bool ac_changed = false;
            bool percent_changed = false;
            {
                std::scoped_lock lock(m_status_mutex);
                ac_changed = fresh.on_ac != m_status.on_ac;
                percent_changed = fresh.battery_percent != m_status.battery_percent;
                const bool saver = m_status.battery_saver;
                m_status = fresh;
                m_status.battery_saver = saver || fresh.battery_saver;
            }
            if (ac_changed) emit(PowerEventKind::PowerSourceChanged);
            if (percent_changed) emit(PowerEventKind::BatteryPercentChanged);
            break;
        }
        case PBT_POWERSETTINGCHANGE:
            if (l_param != 0) {
                handle_power_setting(*reinterpret_cast<const POWERBROADCAST_SETTING*>(l_param));
            }
            break;
        default:
            break;
        }
        return std::nullopt; // DefWindowProc answers TRUE; other hooks may listen too
    }
    if (msg == WM_WTSSESSION_CHANGE) {
        if (w_param == WTS_SESSION_LOCK) {
            if (!m_locked.exchange(true, std::memory_order_relaxed)) {
                emit(PowerEventKind::SessionLocked);
            }
        } else if (w_param == WTS_SESSION_UNLOCK) {
            if (m_locked.exchange(false, std::memory_order_relaxed)) {
                emit(PowerEventKind::SessionUnlocked);
            }
        }
        return std::nullopt; // let DefWindowProc / other hooks see it
    }
    return std::nullopt;
}

// ---- PowerSaveBlocker ------------------------------------------------------------------------------------

PowerSaveBlocker::PowerSaveBlocker(PowerSaveBlockerType type, std::wstring_view reason) : m_type(type) {
    std::wstring text(reason.empty() ? std::wstring_view(L"Boson power save blocker") : reason);
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = text.data();
    m_request = ::PowerCreateRequest(&context);
    if (m_request == INVALID_HANDLE_VALUE || m_request == nullptr) {
        m_request = INVALID_HANDLE_VALUE;
        return;
    }
    bool ok = ::PowerSetRequest(m_request, PowerRequestSystemRequired) != FALSE;
    if (ok && type == PowerSaveBlockerType::PreventDisplaySleep) {
        ok = ::PowerSetRequest(m_request, PowerRequestDisplayRequired) != FALSE;
        if (!ok) {
            ::PowerClearRequest(m_request, PowerRequestSystemRequired);
        }
    }
    m_active = ok;
}

PowerSaveBlocker::~PowerSaveBlocker() {
    if (m_request == INVALID_HANDLE_VALUE) {
        return;
    }
    if (m_active) {
        if (m_type == PowerSaveBlockerType::PreventDisplaySleep) {
            ::PowerClearRequest(m_request, PowerRequestDisplayRequired);
        }
        ::PowerClearRequest(m_request, PowerRequestSystemRequired);
    }
    ::CloseHandle(m_request);
}

PowerSaveBlockers::Id PowerSaveBlockers::start(PowerSaveBlockerType type, std::wstring_view reason) {
    auto blocker = std::make_unique<PowerSaveBlocker>(type, reason);
    if (!blocker->is_active()) {
        return 0;
    }
    std::scoped_lock lock(m_mutex);
    const Id id = m_next_id++;
    if (m_next_id == 0) {
        m_next_id = 1;
    }
    m_entries.push_back(Entry{id, std::move(blocker)});
    return id;
}

bool PowerSaveBlockers::stop(Id id) {
    std::unique_ptr<PowerSaveBlocker> released;
    {
        std::scoped_lock lock(m_mutex);
        const auto it = std::find_if(m_entries.begin(), m_entries.end(), [id](const Entry& e) { return e.id == id; });
        if (it == m_entries.end()) {
            return false;
        }
        released = std::move(it->blocker);
        m_entries.erase(it);
    }
    return true;
}

bool PowerSaveBlockers::is_started(Id id) const {
    std::scoped_lock lock(m_mutex);
    return std::any_of(m_entries.begin(), m_entries.end(), [id](const Entry& e) { return e.id == id; });
}

std::size_t PowerSaveBlockers::size() const {
    std::scoped_lock lock(m_mutex);
    return m_entries.size();
}

void PowerSaveBlockers::stop_all() {
    std::vector<Entry> released;
    {
        std::scoped_lock lock(m_mutex);
        released.swap(m_entries);
    }
}

} // namespace boson::shell
