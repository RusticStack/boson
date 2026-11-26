#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace boson::gpu {

class Device;

namespace detail {
class SurfaceCore;
} // namespace detail

/// @brief The render thread of one Device. It sleeps in WaitForMultipleObjects on a wake event and the
/// device-removed event while no surface wants a frame (zero CPU when idle); while frames are pending it also
/// waits on the swap chains' frame-latency waitable objects - through DCompositionWaitForCompositorClock on
/// Windows 11, so wake-ups line up with the compositor clock - and renders every surface whose buffer is ready.
///
/// A dedicated std::jthread (not the stdexec pool): it has to block on Win32 kernel handles and must never be
/// starved by IPC work.
class RenderLoop {
public:
    explicit RenderLoop(Device& device);
    ~RenderLoop();

    RenderLoop(const RenderLoop&) = delete;
    RenderLoop& operator=(const RenderLoop&) = delete;
    RenderLoop(RenderLoop&&) = delete;
    RenderLoop& operator=(RenderLoop&&) = delete;

    void add(std::shared_ptr<detail::SurfaceCore> core);
    void remove(const detail::SurfaceCore* core) noexcept;
    /// @brief Wakes the thread (request_frame calls this). Any thread, lock-free.
    void wake() noexcept;

    /// @brief Loop iterations so far (each one is a wake-up of the render thread).
    [[nodiscard]] std::uint64_t wakeups() const noexcept { return m_wakeups.load(std::memory_order_relaxed); }
    /// @brief User + kernel CPU time of the render thread in milliseconds (GetThreadTimes).
    [[nodiscard]] double cpu_time_ms() const noexcept;
    [[nodiscard]] bool uses_compositor_clock() const noexcept { return m_clock != nullptr; }
    [[nodiscard]] DWORD thread_id() const noexcept { return m_thread_id.load(std::memory_order_acquire); }

private:
    using WaitForCompositorClockFn = DWORD(WINAPI*)(UINT count, const HANDLE* handles, DWORD timeout_ms);

    void run(std::stop_token stop);

    Device& m_device;
    HANDLE m_wake = nullptr;
    HANDLE m_thread_handle = nullptr;
    WaitForCompositorClockFn m_clock = nullptr;
    std::mutex m_mutex;
    std::vector<std::shared_ptr<detail::SurfaceCore>> m_cores; ///< guarded by m_mutex
    std::atomic<std::uint64_t> m_wakeups{0};
    std::atomic<DWORD> m_thread_id{0};
    std::jthread m_thread; ///< last member: joined before the rest is destroyed
};

} // namespace boson::gpu
