#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace boson::core {
class WebView;
} // namespace boson::core

namespace boson::exec {
class UiMarshal;
} // namespace boson::exec

namespace boson::shell {
class ToastNotifier;
} // namespace boson::shell

namespace boson::native {

class Notifier;

/// @brief A page notification (new Notification(...) / ServiceWorkerRegistration.showNotification).
struct WebNotification {
    std::uint64_t id = 0;
    std::uint64_t webview_id = 0;
    std::string origin;
    std::string title;
    std::string body;
    std::string tag;
    std::string icon_uri;
    std::string badge_uri;
    std::string image_uri;
    std::string language;
    bool silent = false;
    bool requires_interaction = false;
    bool renotify = false;
    double timestamp = 0.0; ///< ms since the UNIX epoch, as reported by the page
};

enum class WebNotificationEvent : std::uint8_t {
    Shown,
    Clicked,
    Closed,
};

[[nodiscard]] std::string_view to_string(WebNotificationEvent event) noexcept;

/// @brief Routes the Web Notification API of attached WebViews to native Windows toasts.
///
/// On ICoreWebView2_24::NotificationReceived the runtime's default UI is suppressed (Handled = TRUE) and the
/// notification is shown as a shell::ToastNotifier toast (lazily created on the first notification). A toast
/// click reports "click" to the page (notification.onclick), restores and focuses the owning window and
/// raises WebNotificationEvent::Clicked; dismissing or failing reports "close". notification.close() from the
/// page removes the toast. A newer notification with the same origin + tag replaces the older one. When the
/// toast cannot be shown (unregistered AppUserModelID, toasts disabled) a Shell_NotifyIcon balloon is used
/// instead (no click feedback).
///
/// Whether a page may create notifications at all is the Notifications permission: combine with
/// native::PermissionManager. UI thread only; destroy it before the WebViews it is attached to.
class WebNotificationHost {
public:
    /// Custom presentation (in-app UI, tests). Return true when the notification is now visible; then call
    /// click(id) / close(id) when the user acts on it.
    using Presenter = std::function<bool(const WebNotification& notification)>;
    using EventFn = std::function<void(const WebNotification& notification, WebNotificationEvent event)>;

    /// @param marshal toast callbacks arrive on WinRT threads and are marshalled to the UI thread through it.
    /// @param app_id AppUserModelID for toasts; empty = the process explicit AppUserModelID.
    explicit WebNotificationHost(exec::UiMarshal& marshal, std::wstring app_id = {});
    ~WebNotificationHost();

    WebNotificationHost(const WebNotificationHost&) = delete;
    WebNotificationHost& operator=(const WebNotificationHost&) = delete;
    WebNotificationHost(WebNotificationHost&&) = delete;
    WebNotificationHost& operator=(WebNotificationHost&&) = delete;

    /// @brief true when the WebView's runtime has ICoreWebView2_24 (NotificationReceived).
    [[nodiscard]] static bool is_supported(const core::WebView& webview) noexcept;

    /// @brief Takes over webview.events().on_notification_received. focus_window is restored and focused on
    /// click (null = the WebView's parent window). Returns is_supported(webview); the handler is installed
    /// either way, so it takes effect once the WebView becomes ready.
    bool attach(core::WebView& webview, HWND focus_window = nullptr);
    void detach(core::WebView& webview) noexcept;

    void set_presenter(Presenter presenter) { m_presenter = std::move(presenter); }
    void set_event_callback(EventFn callback) { m_on_event = std::move(callback); }
    void set_focus_on_click(bool enabled) noexcept { m_focus_on_click = enabled; }

    /// @brief Acts as if the user clicked / closed notification id. false = unknown id.
    bool click(std::uint64_t id);
    bool close(std::uint64_t id);

    [[nodiscard]] std::vector<WebNotification> active() const;
    [[nodiscard]] std::size_t active_count() const noexcept { return m_active.size(); }

private:
    struct Entry {
        WebNotification info;
        Microsoft::WRL::ComPtr<ICoreWebView2Notification> native;
        EventRegistrationToken close_token{};
        HWND focus = nullptr;
        std::string toast_tag;
    };
    struct Attachment {
        core::WebView* webview = nullptr;
        HWND focus = nullptr;
    };

    void on_received(std::uint64_t webview_id, HWND focus, ICoreWebView2NotificationReceivedEventArgs* args);
    bool present(const Entry& entry);
    void on_toast_event(std::string_view tag, int kind, int reason);
    [[nodiscard]] Entry* find(std::uint64_t id) noexcept;
    [[nodiscard]] Entry* find_by_toast(std::string_view tag) noexcept;
    void finish(std::uint64_t id, bool report_closed, bool remove_toast);
    void emit(const WebNotification& notification, WebNotificationEvent event);
    shell::ToastNotifier& toasts();

    exec::UiMarshal& m_marshal;
    std::wstring m_app_id;
    std::unique_ptr<shell::ToastNotifier> m_toasts;
    std::unique_ptr<Notifier> m_fallback;
    Presenter m_presenter;
    EventFn m_on_event;
    bool m_focus_on_click = true;
    std::vector<Attachment> m_attachments;
    std::vector<Entry> m_active;
    std::uint64_t m_next_id = 1;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace boson::native
