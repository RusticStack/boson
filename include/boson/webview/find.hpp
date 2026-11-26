#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace boson::webview {

struct FindOptions {
    std::string term;                   ///< UTF-8; must not be empty.
    bool case_sensitive = false;
    bool whole_word = false;
    bool highlight_all = true;
    /// true: no Edge find bar; the app draws its own UI from the match events.
    bool suppress_default_dialog = true;
};

struct FindState {
    std::int32_t match_count = 0;
    /// 1-based index of the highlighted match; -1 when there is none.
    std::int32_t active_match_index = -1;
};

/// @brief Find-in-page for one WebView (ICoreWebView2_28 / ICoreWebView2Find, runtime 1.0.3240+).
///
/// UI thread only. Construct after the WebView is ready; the session binds to the WebView's current
/// ICoreWebView2 and must be re-created if the controller is rebuilt (Environment::recreate). Destroying
/// the session removes its event handlers but does not stop an active find; call stop() for that.
class FindSession {
public:
    using StateCallback = std::function<void(const FindState&)>;

    explicit FindSession(core::WebView& webview);
    ~FindSession();

    FindSession(const FindSession&) = delete;
    FindSession& operator=(const FindSession&) = delete;
    FindSession(FindSession&&) = delete;
    FindSession& operator=(FindSession&&) = delete;

    /// @brief false when the runtime has no Find API (older than 1.0.3240) or the WebView was not ready.
    [[nodiscard]] bool is_supported() const noexcept { return m_find != nullptr; }

    /// @brief Starts (or restarts) a search. done runs on the UI thread once the first pass has completed;
    /// match counts may keep updating afterwards through the state callback.
    bool start(const FindOptions& options, std::function<void(HRESULT, FindState)> done = {});
    bool next() noexcept;
    bool previous() noexcept;
    bool stop() noexcept;

    [[nodiscard]] FindState state() const noexcept;
    [[nodiscard]] bool is_active() const noexcept { return m_active; }

    /// @brief Fires on MatchCountChanged and ActiveMatchIndexChanged.
    void set_state_callback(StateCallback cb) { m_on_state = std::move(cb); }

private:
    void notify() const;

    core::WebView& m_webview;
    Microsoft::WRL::ComPtr<ICoreWebView2Find> m_find;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment15> m_env15;
    EventRegistrationToken m_count_token{};
    EventRegistrationToken m_index_token{};
    StateCallback m_on_state;
    bool m_active = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace boson::webview
