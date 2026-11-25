#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace boson::exec {

/// @brief Thread-safe MPSC queue marshaling completed JSON payloads to the Win32 UI thread.
class UiMarshal {
public:
    /// @brief Constructs a UiMarshal with a UI wakeup callback and optional delivery callback.
    explicit UiMarshal(
        std::function<void()> wake_ui_fn,
        std::function<void(std::string_view)> deliver_fn = {})
        : m_wake_ui_fn(std::move(wake_ui_fn)),
          m_deliver_fn(std::move(deliver_fn)) {}

    UiMarshal(const UiMarshal&) = delete;
    UiMarshal& operator=(const UiMarshal&) = delete;
    UiMarshal(UiMarshal&&) = delete;
    UiMarshal& operator=(UiMarshal&&) = delete;
    ~UiMarshal() = default;

    /// @brief Sets or replaces the callback invoked on the UI thread for each drained payload.
    void set_deliver_callback(std::function<void(std::string_view)> deliver_fn) {
        std::scoped_lock lock(m_mutex);
        m_deliver_fn = std::move(deliver_fn);
    }

    /// @brief Enqueues a JSON payload from any background thread and wakes the UI thread.
    void enqueue(std::string json_payload) {
        {
            std::scoped_lock lock(m_mutex);
            m_queue.push_back(std::move(json_payload));
        }

        // Invoke wake callback outside the lock to prevent lock-order inversion with Win32 PostMessage.
        if (m_wake_ui_fn) {
            m_wake_ui_fn();
        }
    }

    /// @brief Drains all queued payloads and delivers them on the UI thread.
    void drain_on_ui_thread() {
        std::deque<std::string> local_queue;
        std::function<void(std::string_view)> deliver_fn;
        {
            std::scoped_lock lock(m_mutex);
            local_queue.swap(m_queue);
            deliver_fn = m_deliver_fn;
        }

        if (!deliver_fn) {
            return;
        }

        for (const auto& payload : local_queue) {
            deliver_fn(payload);
        }
    }

    /// @brief Returns the number of payloads currently queued for UI delivery.
    [[nodiscard]] std::size_t pending_count() const {
        std::scoped_lock lock(m_mutex);
        return m_queue.size();
    }

private:
    mutable std::mutex m_mutex;
    std::deque<std::string> m_queue;
    std::function<void()> m_wake_ui_fn;
    std::function<void(std::string_view)> m_deliver_fn;
};

}  // namespace boson::exec
