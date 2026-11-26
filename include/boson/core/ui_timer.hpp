#pragma once

#include <windows.h>

#include <chrono>
#include <functional>

namespace boson::core {

/// @brief RAII Win32 thread timer (SetTimer with a null HWND). Fires on the thread that started it, from
/// that thread's message loop, so callbacks run on the UI thread without any extra window.
class UiTimer {
public:
    UiTimer() noexcept = default;
    ~UiTimer();

    UiTimer(const UiTimer&) = delete;
    UiTimer& operator=(const UiTimer&) = delete;
    UiTimer(UiTimer&&) = delete;
    UiTimer& operator=(UiTimer&&) = delete;

    /// @brief Starts (or restarts) the timer. A one-shot timer stops itself before invoking the callback.
    bool start(std::chrono::milliseconds interval, std::function<void()> callback, bool repeat = false);
    void stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept { return m_id != 0; }

private:
    static void CALLBACK timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);

    UINT_PTR m_id = 0;
    bool m_repeat = false;
    std::function<void()> m_callback;
};

}  // namespace boson::core
