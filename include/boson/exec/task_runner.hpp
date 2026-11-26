#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <stdexec/execution.hpp>

#include <exec/async_scope.hpp>

#include "boson/exec/scheduler.hpp"
#include "boson/exec/ui_marshal.hpp"

namespace boson::exec {

namespace detail {
/// {"id":<callback_id>,"status":"error","error":{"code":-32000,"message":<message>}}; never throws.
[[nodiscard]] std::string make_task_error_envelope(std::uint64_t callback_id, std::string_view message) noexcept;
} // namespace detail

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

    /// @brief Allocation-lean form of dispatch(callback_id, work, on_complete) for the IPC hot path: work is any
    /// callable returning the response JSON (kept in the sender's operation state, no std::function), and the
    /// response travels to on_complete on the UI thread inside the UiMarshal queue item (no extra closure).
    template <typename Work>
    void dispatch_reply(std::uint64_t callback_id, Work&& work, std::function<void(std::string)> on_complete) {
        auto sender =
            stdexec::schedule(m_scheduler.get_scheduler()) |
            stdexec::then([callback_id, task = std::forward<Work>(work)]() mutable noexcept -> std::string {
                try {
                    return task();
                } catch (const std::exception& e) {
                    return detail::make_task_error_envelope(callback_id, e.what());
                } catch (...) {
                    return detail::make_task_error_envelope(callback_id, "Unknown native exception");
                }
            }) |
            stdexec::then([&marshal = m_marshal, done = std::move(on_complete)](std::string payload) mutable noexcept {
                try {
                    if (done) {
                        marshal.post_reply(std::move(done), std::move(payload));
                    } else {
                        marshal.enqueue(std::move(payload));
                    }
                } catch (...) {
                    // Never allow exceptions to escape background worker into stdexec::async_scope.
                }
            });
        m_scope.spawn(std::move(sender));
    }

    /// @brief Runs fire-and-forget background work (no JSON envelope), e.g. a poller that emits events.
    void spawn(std::function<void()> work);

private:
    Scheduler& m_scheduler;
    UiMarshal& m_marshal;
    ::exec::async_scope m_scope;
};

}  // namespace boson::exec
