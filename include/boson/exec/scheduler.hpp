#pragma once

#include <algorithm>
#include <cstdint>
#include <thread>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

namespace boson::exec {

/// @brief Manages the background worker thread pool via NVIDIA stdexec.
class Scheduler {
public:
    /// @brief Returns the default worker thread count for this machine.
    [[nodiscard]] static std::uint32_t default_thread_count() noexcept {
        return std::max(2u, std::thread::hardware_concurrency());
    }

    /// @brief Constructs a static thread pool with the given number of threads.
    explicit Scheduler(std::uint32_t thread_count = default_thread_count())
        : m_thread_count(std::max(1u, thread_count)),
          m_pool(m_thread_count) {}

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;
    ~Scheduler() = default;

    /// @brief Returns a stdexec scheduler handle for dispatching background work.
    [[nodiscard]] auto get_scheduler() noexcept {
        return m_pool.get_scheduler();
    }

    /// @brief Returns the number of worker threads in the pool.
    [[nodiscard]] std::size_t thread_count() const noexcept {
        return static_cast<std::size_t>(m_thread_count);
    }

private:
    std::uint32_t m_thread_count;
    ::exec::static_thread_pool m_pool;
};

}  // namespace boson::exec
