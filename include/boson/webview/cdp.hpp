#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>
#include <boson/exec/callback_sender.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::webview {

/// @brief Event names of one Chrome DevTools Protocol domain ("Page", "Runtime", ...) as known to Boson.
/// WebView2 has no wildcard subscription, so Cdp::subscribe_domain and CdpServer use this table. Empty for an
/// unknown domain.
[[nodiscard]] std::span<const std::string_view> cdp_domain_events(std::string_view domain) noexcept;
/// @brief Every domain cdp_domain_events knows.
[[nodiscard]] std::span<const std::string_view> cdp_known_domains() noexcept;
/// @brief "Page" for "Page.navigate"; empty when method has no dot.
[[nodiscard]] std::string_view cdp_domain_of(std::string_view method) noexcept;

/// @brief Chrome DevTools Protocol access to one WebView: ICoreWebView2::CallDevToolsProtocolMethod,
/// ICoreWebView2_11::CallDevToolsProtocolMethodForSession (flattened child sessions: OOPIFs, workers) and
/// GetDevToolsProtocolEventReceiver for any event.
///
/// One COM event receiver is registered per event name, on first subscription, and removed with the last
/// one, so an idle Cdp costs nothing. Results and events are delivered on the UI thread as the raw JSON
/// text the runtime produced (converted to UTF-8 once, never re-parsed). UI thread only. The WebView must
/// outlive the Cdp.
class Cdp {
public:
    /// ok: json is the CDP result object. !ok: json is the CDP error object {"code":..,"message":..}.
    using ResultFn = std::function<void(bool ok, std::string json)>;
    /// session_id is empty for the WebView's own (top page) session.
    using EventFn = std::function<void(std::string_view method, std::string_view params_json, std::string_view session_id)>;
    using SubscriptionId = std::uint64_t;

    explicit Cdp(core::WebView& webview);
    ~Cdp();

    Cdp(const Cdp&) = delete;
    Cdp& operator=(const Cdp&) = delete;
    Cdp(Cdp&&) = delete;
    Cdp& operator=(Cdp&&) = delete;

    /// @brief Calls method with params_json (a JSON object; empty means {}). A non-empty session_id targets
    /// a flattened child session (requires ICoreWebView2_11). Returns false (done is not called) when the
    /// WebView is not ready or the call could not be started.
    bool call(std::string_view method, std::string_view params_json, ResultFn done, std::string_view session_id = {});
    /// @brief Sender form: completes with the result JSON, or set_error(HResultError) carrying the CDP error.
    [[nodiscard]] exec::CallbackSender<std::string> call(std::string method, std::string params_json, std::string session_id = {});

    /// @brief Subscribes to one event ("Network.requestWillBeSent"). Remember to call "<Domain>.enable".
    SubscriptionId subscribe(std::string_view event, EventFn fn);
    /// @brief Subscribes to every event of a domain listed in cdp_domain_events (0 for an unknown domain).
    SubscriptionId subscribe_domain(std::string_view domain, EventFn fn);
    bool unsubscribe(SubscriptionId id) noexcept;
    void unsubscribe_all() noexcept;

    /// @brief Re-registers every receiver on the WebView's current ICoreWebView2 (after Environment::recreate).
    void rebind();

    [[nodiscard]] bool supports_sessions() const noexcept;
    [[nodiscard]] std::size_t receiver_count() const noexcept { return m_receivers.size(); }
    [[nodiscard]] core::WebView& webview() const noexcept { return m_webview; }

private:
    struct Subscription {
        SubscriptionId id = 0;
        std::vector<std::string> events;
        std::shared_ptr<EventFn> fn;
    };
    struct Receiver {
        std::string event;
        Microsoft::WRL::ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
        EventRegistrationToken token{};
        std::vector<std::shared_ptr<EventFn>> handlers;
    };

    Receiver* receiver_for(std::string_view event);
    bool attach_receiver(Receiver& receiver);
    static void detach_receiver(Receiver& receiver) noexcept;
    void drop_handler(const std::string& event, const std::shared_ptr<EventFn>& fn) noexcept;

    core::WebView& m_webview;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_bound;
    std::vector<std::unique_ptr<Receiver>> m_receivers;
    std::vector<Subscription> m_subscriptions;
    SubscriptionId m_next_id = 1;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace boson::webview
