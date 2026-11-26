#pragma once

#include <cstdint>

namespace boson::ipc {

namespace detail {
inline thread_local std::uint64_t t_sender_webview_id = 0;
} // namespace detail

/// @brief WebView::id() of the WebView whose request is being handled on this thread, 0 when unknown
/// (e.g. dispatch_sync() called directly). Set by Bridge on the UI thread and carried by Router onto the
/// pool thread that runs the handler, so controllers can reply to the sender (Electron's event.sender).
[[nodiscard]] inline std::uint64_t current_sender_id() noexcept {
    return detail::t_sender_webview_id;
}

/// @brief RAII scope that sets current_sender_id() for this thread and restores the previous value.
class SenderScope {
public:
    explicit SenderScope(std::uint64_t webview_id) noexcept
        : m_previous(detail::t_sender_webview_id) {
        detail::t_sender_webview_id = webview_id;
    }
    ~SenderScope() { detail::t_sender_webview_id = m_previous; }

    SenderScope(const SenderScope&) = delete;
    SenderScope& operator=(const SenderScope&) = delete;
    SenderScope(SenderScope&&) = delete;
    SenderScope& operator=(SenderScope&&) = delete;

private:
    std::uint64_t m_previous;
};

} // namespace boson::ipc
