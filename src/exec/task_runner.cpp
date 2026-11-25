#include "boson/exec/task_runner.hpp"

#include <cstdio>
#include <exception>
#include <string_view>
#include <utility>

#include <stdexec/execution.hpp>

namespace boson::exec {
namespace {

std::string escape_json_string(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const unsigned char ch : input) {
        switch (ch) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\b':
                out.append("\\b");
                break;
            case '\f':
                out.append("\\f");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (ch < 0x20U) {
                    char buf[7]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(ch));
                    out.append(buf, 6);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return out;
}

std::string make_error_envelope(std::uint64_t callback_id, std::string_view message) noexcept {
    try {
        std::string out;
        out.reserve(96 + message.size());
        out.append("{\"id\":");
        out.append(std::to_string(callback_id));
        out.append(",\"status\":\"error\",\"error\":{\"code\":-32000,\"message\":\"");
        out.append(escape_json_string(message));
        out.append("\"}}");
        return out;
    } catch (...) {
        return "{\"id\":0,\"status\":\"error\",\"error\":{\"code\":-32000,\"message\":\"Internal error\"}}";
    }
}

std::string invoke_safely(
    std::uint64_t callback_id,
    const std::function<std::string()>& work) noexcept {
    if (!work) {
        return make_error_envelope(callback_id, "Empty IPC task");
    }

    try {
        return work();
    } catch (const std::exception& e) {
        return make_error_envelope(callback_id, e.what());
    } catch (...) {
        return make_error_envelope(callback_id, "Unknown native exception");
    }
}

}  // namespace

TaskRunner::TaskRunner(Scheduler& scheduler, UiMarshal& marshal)
    : m_scheduler(scheduler),
      m_marshal(marshal) {}

TaskRunner::~TaskRunner() {
    m_scope.request_stop();
    stdexec::sync_wait(m_scope.on_empty());
}

void TaskRunner::dispatch(std::uint64_t callback_id, std::function<std::string()> work) {
    auto sender =
        stdexec::schedule(m_scheduler.get_scheduler()) |
        stdexec::then([callback_id, task = std::move(work)]() noexcept -> std::string {
            return invoke_safely(callback_id, task);
        }) |
        stdexec::then([&marshal = m_marshal](std::string payload) noexcept {
            try {
                marshal.enqueue(std::move(payload));
            } catch (...) {
                // Never allow exceptions to escape background worker into stdexec::async_scope.
            }
        });

    m_scope.spawn(std::move(sender));
}

}  // namespace boson::exec
