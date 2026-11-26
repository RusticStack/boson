#include <boson/core/ui_timer.hpp>

#include <boson/core/result.hpp>

#include <unordered_map>

namespace boson::core {

namespace {

// Thread timers are only dispatched on the thread that created them, so a thread_local map needs no lock.
thread_local std::unordered_map<UINT_PTR, UiTimer*> t_timers;

} // namespace

UiTimer::~UiTimer() {
    stop();
}

bool UiTimer::start(std::chrono::milliseconds interval, std::function<void()> callback, bool repeat) {
    stop();
    const auto ms = interval.count() < USER_TIMER_MINIMUM ? USER_TIMER_MINIMUM : static_cast<UINT>(interval.count());
    m_callback = std::move(callback);
    m_repeat = repeat;
    m_id = SetTimer(nullptr, 0, ms, &UiTimer::timer_proc);
    if (m_id == 0) {
        hr_check(HRESULT_FROM_WIN32(GetLastError()), "SetTimer");
        return false;
    }
    t_timers[m_id] = this;
    return true;
}

void UiTimer::stop() noexcept {
    if (m_id != 0) {
        KillTimer(nullptr, m_id);
        t_timers.erase(m_id);
        m_id = 0;
    }
}

void CALLBACK UiTimer::timer_proc(HWND /*hwnd*/, UINT /*msg*/, UINT_PTR id, DWORD /*time*/) {
    const auto it = t_timers.find(id);
    if (it == t_timers.end()) {
        KillTimer(nullptr, id);
        return;
    }
    UiTimer* self = it->second;
    // Move the callback out first: it may destroy or restart this timer.
    std::function<void()> callback = self->m_callback;
    if (!self->m_repeat) {
        self->stop();
    }
    if (callback) {
        callback();
    }
}

} // namespace boson::core
