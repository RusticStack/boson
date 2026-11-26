#pragma once

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace boson::exec {

/// @brief Thread-safe MPSC queue marshaling completed JSON payloads and UI-thread callbacks to the Win32 UI thread.
///
/// The UI thread is woken at most once per batch: the wake callback runs only when no earlier wake is still
/// pending, and drain_on_ui_thread() re-arms it. N completions posted while the UI thread is busy therefore cost
/// one PostMessage, not N. Items run in the order they were posted.
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
        Item item;
        item.payload = std::move(json_payload);
        push(std::move(item));
    }

    /// @brief Enqueues an arbitrary UI-thread callback and wakes the UI thread.
    void post_task(std::function<void()> task) {
        if (!task) {
            return;
        }
        Item item;
        item.task = std::move(task);
        push(std::move(item));
    }

    /// @brief Enqueues done(payload) for the UI thread. Unlike post_task() the payload travels in the queue item,
    /// so a small done (the usual capture-by-pointer closure) makes the whole post allocation-free.
    void post_reply(std::function<void(std::string)> done, std::string payload) {
        if (!done) {
            return;
        }
        Item item;
        item.reply = std::move(done);
        item.payload = std::move(payload);
        push(std::move(item));
    }

    /// @brief Registers fn to run on the UI thread at the end of every drain_on_ui_thread() (after the queued items),
    /// keyed by owner for remove_drain_hook(). Lets a producer coalesce the items of one drain (Bridge event batching).
    void add_drain_hook(const void* owner, std::function<void()> fn) {
        std::scoped_lock lock(m_mutex);
        auto hooks = m_hooks ? std::make_shared<std::vector<Hook>>(*m_hooks) : std::make_shared<std::vector<Hook>>();
        hooks->push_back(Hook{.owner = owner, .fn = std::move(fn)});
        m_hooks = std::move(hooks);
    }

    /// @brief Removes the hooks registered by owner (UI thread; must not be called from inside a hook).
    void remove_drain_hook(const void* owner) noexcept {
        std::scoped_lock lock(m_mutex);
        if (!m_hooks) {
            return;
        }
        auto hooks = std::make_shared<std::vector<Hook>>(*m_hooks);
        std::erase_if(*hooks, [owner](const Hook& hook) { return hook.owner == owner; });
        m_hooks = hooks->empty() ? nullptr : std::move(hooks);
    }

    /// @brief Executes a callable on the UI thread and blocks the caller until it completes.
    template <typename Fn>
    auto invoke_sync(Fn&& fn) -> std::invoke_result_t<Fn> {
        using Result = std::invoke_result_t<Fn>;
        std::promise<Result> promise;
        std::future<Result> future = promise.get_future();

        post_task([&promise, callable = std::forward<Fn>(fn)]() mutable {
            try {
                if constexpr (std::is_void_v<Result>) {
                    callable();
                    promise.set_value();
                } else {
                    promise.set_value(callable());
                }
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        });

        return future.get();
    }

    /// @brief Drains all queued UI tasks and JSON payloads on the UI thread and re-arms the wake-up.
    void drain_on_ui_thread() {
        std::vector<Item> local;
        std::function<void(std::string_view)> deliver_fn;
        std::shared_ptr<const std::vector<Hook>> hooks;
        {
            std::scoped_lock lock(m_mutex);
            local.swap(m_items);
            m_items.swap(m_spare); // keep the grown capacity: steady-state posting does not allocate
            m_wake_pending = false;
            deliver_fn = m_deliver_fn;
            hooks = m_hooks;
        }

        for (Item& item : local) {
            if (item.task) {
                item.task();
            } else if (item.reply) {
                item.reply(std::move(item.payload));
            } else if (deliver_fn) {
                deliver_fn(item.payload);
            }
        }

        if (hooks) {
            for (const Hook& hook : *hooks) {
                hook.fn();
            }
        }

        local.clear();
        if (local.capacity() <= kMaxRetainedItems) {
            std::scoped_lock lock(m_mutex);
            if (m_spare.capacity() < local.capacity()) {
                m_spare.swap(local);
            }
        }
    }

    /// @brief Returns the number of items currently queued for UI delivery.
    [[nodiscard]] std::size_t pending_count() const {
        std::scoped_lock lock(m_mutex);
        return m_items.size();
    }

private:
    struct Hook {
        const void* owner = nullptr;
        std::function<void()> fn;
    };

    struct Item {
        std::function<void()> task;
        std::function<void(std::string)> reply;
        std::string payload;
    };

    static constexpr std::size_t kMaxRetainedItems = 4096;

    void push(Item&& item) {
        bool wake = false;
        {
            std::scoped_lock lock(m_mutex);
            m_items.push_back(std::move(item));
            wake = !m_wake_pending;
            m_wake_pending = true;
        }
        // Invoke wake callback outside the lock to prevent lock-order inversion with Win32 PostMessage.
        if (wake && m_wake_ui_fn) {
            m_wake_ui_fn();
        }
    }

    mutable std::mutex m_mutex;
    std::vector<Item> m_items;
    std::vector<Item> m_spare;
    std::shared_ptr<const std::vector<Hook>> m_hooks;
    bool m_wake_pending = false;
    std::function<void()> m_wake_ui_fn;
    std::function<void(std::string_view)> m_deliver_fn;
};

}  // namespace boson::exec
