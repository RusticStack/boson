#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <boson/core/trace.hpp>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

namespace boson::exec {

/// @brief Manages the background worker thread pool via NVIDIA stdexec.
///
/// The pool is created on first use (the first dispatched task), not in the constructor: an application that
/// builds its Scheduler before the window is up no longer creates one thread per core on the start-up path, and
/// one that never dispatches never creates any.
class Scheduler {
public:
    /// @brief Upper bound of the default pool size (see default_thread_count()).
    static constexpr std::uint32_t kMaxDefaultThreads = 6;

    /// @brief Returns the default worker thread count for this machine: half the logical processors, clamped
    /// to [2, kMaxDefaultThreads].
    ///
    /// IPC handlers are short and the UI thread never runs them, so a pool the size of the machine only adds
    /// idle threads: every worker costs ~40 KB of private working set, ~56 KB of commit, a kernel stack and a
    /// handle for the life of the process (measured: 24 -> 6 threads saves ~0.75 MB WS / ~1 MB commit with no
    /// change in IPC throughput, docs/PERFORMANCE.md). Pass an explicit count to Scheduler for a workload
    /// that blocks pool threads.
    [[nodiscard]] static std::uint32_t default_thread_count() noexcept {
        return std::clamp(std::thread::hardware_concurrency() / 2, 2u, kMaxDefaultThreads);
    }

    /// @brief Remembers the worker thread count; the static thread pool itself starts on first use.
    explicit Scheduler(std::uint32_t thread_count = default_thread_count())
        : m_thread_count(std::max(1u, thread_count)) {}

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;
    ~Scheduler() = default;

    /// @brief Returns a stdexec scheduler handle for dispatching background work (starts the pool once).
    [[nodiscard]] auto get_scheduler() {
        std::call_once(m_started, [this]() {
            trace::mark("scheduler.pool.begin");
            m_pool = std::make_unique<::exec::static_thread_pool>(m_thread_count);
            trace::mark("scheduler.pool.end");
        });
        return m_pool->get_scheduler();
    }

    /// @brief Returns the number of worker threads in the pool.
    [[nodiscard]] std::size_t thread_count() const noexcept {
        return static_cast<std::size_t>(m_thread_count);
    }

private:
    std::uint32_t m_thread_count;
    std::once_flag m_started;
    std::unique_ptr<::exec::static_thread_pool> m_pool;
};

}  // namespace boson::exec
