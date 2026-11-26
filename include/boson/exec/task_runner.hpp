#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <exec/async_scope.hpp>

#include "boson/exec/scheduler.hpp"
#include "boson/exec/ui_marshal.hpp"

namespace boson::exec {

/// @brief Dispatches IPC tasks onto the background Scheduler and marshals results to UiMarshal.
class TaskRunner {
public:
    /// @brief Constructs a TaskRunner bound to a Scheduler and UiMarshal.
    explicit TaskRunner(Scheduler& scheduler, UiMarshal& marshal);

    TaskRunner(const TaskRunner&) = delete;
    TaskRunner& operator=(const TaskRunner&) = delete;
    TaskRunner(TaskRunner&&) = delete;
    TaskRunner& operator=(TaskRunner&&) = delete;

    /// @brief Waits for all in-flight spawned tasks to finish before destruction.
    ~TaskRunner();

    /// @brief Schedules work on the background thread pool and enqueues its JSON result to UiMarshal.
    void dispatch(std::uint64_t callback_id, std::function<std::string()> work);

    /// @brief Same as dispatch(), but hands the JSON result to on_complete on the UI thread instead of the
    /// UiMarshal delivery callback, so each response can be routed to the WebView that sent the request.
    void dispatch(
        std::uint64_t callback_id,
        std::function<std::string()> work,
        std::function<void(std::string)> on_complete);

    /// @brief Runs fire-and-forget background work (no JSON envelope), e.g. a poller that emits events.
    void spawn(std::function<void()> work);

private:
    Scheduler& m_scheduler;
    UiMarshal& m_marshal;
    ::exec::async_scope m_scope;
};

}  // namespace boson::exec
