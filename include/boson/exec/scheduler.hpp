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
    /// @brief Returns the default worker thread count for this machine.
    [[nodiscard]] static std::uint32_t default_thread_count() noexcept {
        return std::max(2u, std::thread::hardware_concurrency());
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
