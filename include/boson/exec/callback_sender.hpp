#pragma once

#include <windows.h>

#include <cstdio>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

namespace boson::exec {

/// @brief Exception carried through set_error when an asynchronous COM operation fails.
class HResultError : public std::runtime_error {
public:
    HResultError(HRESULT hr, std::string_view context)
        : std::runtime_error(format(hr, context)), m_hr(hr) {}

    [[nodiscard]] HRESULT code() const noexcept { return m_hr; }

private:
    static std::string format(HRESULT hr, std::string_view context) {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
        std::string out = "HRESULT ";
        out += buffer;
        out += " at ";
        out += context;
        return out;
    }

    HRESULT m_hr;
};

/// @brief Completion handle handed to a CallbackSender initiator; call exactly once.
template <typename T>
using Completion = std::function<void(HRESULT hr, T value)>;

/// @brief A P2300 sender adapting a callback-style asynchronous COM operation.
///
/// The initiator runs on the thread that calls start() and receives a Completion. WebView2 completes on the
/// UI thread, so never stdexec::sync_wait() one of these senders on the UI thread: it would deadlock the
/// message loop. Chain with stdexec::then / continues_on and start it with an async_scope or start_detached.
template <typename T>
class CallbackSender {
public:
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(T),
        stdexec::set_error_t(std::exception_ptr)>;

    using Initiator = std::function<void(Completion<T>)>;

    CallbackSender(Initiator initiator, std::string_view context)
        : m_initiator(std::move(initiator)), m_context(context) {}

    template <typename Receiver>
    struct Operation {
        using operation_state_concept = stdexec::operation_state_tag;

        Receiver m_receiver;
        Initiator m_initiator;
        std::string_view m_context;

        void start() & noexcept {
            if (!m_initiator) {
                stdexec::set_error(
                    std::move(m_receiver),
                    std::make_exception_ptr(HResultError(E_NOT_VALID_STATE, m_context)));
                return;
            }
            try {
                m_initiator([this](HRESULT hr, T value) noexcept {
                    if (SUCCEEDED(hr)) {
                        stdexec::set_value(std::move(m_receiver), std::move(value));
                    } else {
                        stdexec::set_error(
                            std::move(m_receiver),
                            std::make_exception_ptr(HResultError(hr, m_context)));
                    }
                });
            } catch (...) {
                stdexec::set_error(std::move(m_receiver), std::current_exception());
            }
        }
    };

    template <typename Receiver>
    auto connect(Receiver receiver) && -> Operation<Receiver> {
        return Operation<Receiver>{std::move(receiver), std::move(m_initiator), m_context};
    }

    template <typename Receiver>
    auto connect(Receiver receiver) const& -> Operation<Receiver> {
        return Operation<Receiver>{std::move(receiver), m_initiator, m_context};
    }

private:
    Initiator m_initiator;
    std::string_view m_context;
};

}  // namespace boson::exec
