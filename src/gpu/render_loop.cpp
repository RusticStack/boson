#include <boson/gpu/render_loop.hpp>

#include "surface_core.hpp"

#include <boson/core/result.hpp>
#include <boson/gpu/device.hpp>

#include <algorithm>
#include <array>

namespace boson::gpu {

namespace {

constexpr DWORD kRecoveryRetryMs = 50;
constexpr DWORD kOcclusionProbeMs = 250;

bool is_lost(HRESULT hr) noexcept {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

} // namespace

RenderLoop::RenderLoop(Device& device)
    : m_device(device) {
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HMODULE dcomp = GetModuleHandleW(L"dcomp.dll"); dcomp != nullptr) {
        m_clock = reinterpret_cast<WaitForCompositorClockFn>(
            reinterpret_cast<void*>(GetProcAddress(dcomp, "DCompositionWaitForCompositorClock")));
    }
    m_thread = std::jthread([this](std::stop_token stop) { run(std::move(stop)); });
}

RenderLoop::~RenderLoop() {
    m_thread.request_stop();
    wake();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    {
        std::scoped_lock guard(m_mutex);
        m_cores.clear();
    }
    if (m_thread_handle != nullptr) {
        CloseHandle(m_thread_handle);
    }
    if (m_wake != nullptr) {
        CloseHandle(m_wake);
    }
}

void RenderLoop::add(std::shared_ptr<detail::SurfaceCore> core) {
    {
        std::scoped_lock guard(m_mutex);
        m_cores.push_back(std::move(core));
    }
    wake();
}

void RenderLoop::remove(const detail::SurfaceCore* core) noexcept {
    {
        std::scoped_lock guard(m_mutex);
        std::erase_if(m_cores, [core](const auto& c) { return c.get() == core; });
    }
    wake();
}

void RenderLoop::wake() noexcept {
    if (m_wake != nullptr) {
        SetEvent(m_wake);
    }
}

double RenderLoop::cpu_time_ms() const noexcept {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (m_thread_handle == nullptr || GetThreadTimes(m_thread_handle, &creation, &exit, &kernel, &user) == FALSE) {
        return 0.0;
    }
    const auto ticks = [](FILETIME t) {
        return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
}

void RenderLoop::run(std::stop_token stop) {
    SetThreadDescription(GetCurrentThread(), L"Boson GPU render");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                    THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    m_thread_handle = self;
    m_thread_id.store(GetCurrentThreadId(), std::memory_order_release);

    std::vector<std::shared_ptr<detail::SurfaceCore>> cores;
    std::vector<detail::SurfaceCore*> waiting; // cores whose waitable is in handles[2..]
    std::array<HANDLE, MAXIMUM_WAIT_OBJECTS> handles{};

    while (!stop.stop_requested()) {
        m_wakeups.fetch_add(1, std::memory_order_relaxed);
        {
            std::scoped_lock guard(m_mutex);
            cores.assign(m_cores.begin(), m_cores.end());
        }

        if (m_device.recovery_pending() && !m_device.retry_recovery()) {
            // Lost device not re-created yet (a D3D12 device is re-created only once nothing references the
            // removed one): retry every 50 ms; this is the only timed wait, and only while the device is lost.
            WaitForSingleObject(m_wake, kRecoveryRetryMs);
            continue;
        }

        handles[0] = m_wake;
        handles[1] = m_device.removed_event();
        DWORD count = 2;
        waiting.clear();
        bool pending = false;
        bool ready = false;
        bool occluded = false;
        {
            const auto guard = m_device.lock();
            for (const auto& core : cores) {
                core->close_retired();
                if (!core->wants_frame()) {
                    continue;
                }
                if (core->occluded()) {
                    core->probe_occlusion();
                    if (core->occluded()) {
                        occluded = true;
                        continue;
                    }
                }
                pending = true;
                if (core->ready_without_wait()) {
                    ready = true;
                } else if (count < handles.size()) {
                    handles[count++] = core->waitable();
                    waiting.push_back(core.get());
                } else {
                    ready = true; // more than 62 waiting surfaces: poll the rest with timeout 0
                }
            }
        }

        DWORD timeout = INFINITE;
        if (occluded && !pending) {
            timeout = kOcclusionProbeMs;
        }
        DWORD result = WAIT_TIMEOUT;
        if (ready) {
            result = WaitForMultipleObjects(count, handles.data(), FALSE, 0);
        } else if (pending && m_clock != nullptr) {
            // Returns on any handle or on the next compositor clock tick (Windows 11).
            result = m_clock(count, handles.data(), timeout);
        } else {
            result = WaitForMultipleObjects(pending ? count : 2, handles.data(), FALSE, timeout);
        }
        if (stop.stop_requested()) {
            break;
        }
        if (WaitForSingleObject(m_device.removed_event(), 0) == WAIT_OBJECT_0 && !m_device.recovery_pending()) {
            m_device.handle_device_lost(DXGI_ERROR_DEVICE_REMOVED);
            continue;
        }
        if (!pending) {
            continue;
        }

        const auto guard = m_device.lock();
        const DWORD signaled = result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count ? result - WAIT_OBJECT_0 : MAXDWORD;
        for (const auto& core : cores) {
            if (!core->wants_frame() || core->occluded()) {
                continue;
            }
            bool already = false;
            if (signaled >= 2 && signaled != MAXDWORD) {
                already = waiting[signaled - 2] == core.get(); // WaitForMultipleObjects took that count already
            }
            if (!core->ready_without_wait() && !core->try_acquire(already)) {
                continue;
            }
            const HRESULT hr = core->render();
            if (is_lost(hr)) {
                m_device.handle_device_lost(hr);
                break; // everything is re-created; start over
            }
        }
    }
    m_thread_id.store(0, std::memory_order_release);
}

} // namespace boson::gpu
