#include <boson/native/web_notifications.hpp>

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/native/notification.hpp>
#include <boson/security/origin.hpp>
#include <boson/shell/toast.hpp>

#include <algorithm>
#include <string>

namespace boson::native {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr std::string_view kToastGroup = "boson-web";

enum ToastKind : int { kActivated = 0, kDismissed = 1, kFailed = 2 };
enum DismissReason : int { kUserCanceled = 0, kApplicationHidden = 1, kTimedOut = 2 };

[[nodiscard]] std::string read_string(HRESULT hr, wchar_t* raw) {
    std::string out;
    if (SUCCEEDED(hr) && raw != nullptr) {
        out = core::wide_to_utf8(raw);
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);
    }
    return out;
}

template <typename Getter>
[[nodiscard]] std::string get_string(Getter getter) {
    wchar_t* raw = nullptr;
    const HRESULT hr = getter(&raw);
    return read_string(hr, raw);
}

[[nodiscard]] bool get_bool(HRESULT hr, BOOL value) noexcept {
    return SUCCEEDED(hr) && value != FALSE;
}

void focus_window(HWND hwnd) noexcept {
    if (hwnd == nullptr || IsWindow(hwnd) == FALSE) {
        return;
    }
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root == nullptr) {
        root = hwnd;
    }
    if (IsIconic(root) != FALSE) {
        ShowWindow(root, SW_RESTORE);
    } else if (IsWindowVisible(root) == FALSE) {
        ShowWindow(root, SW_SHOW);
    }
    SetForegroundWindow(root);
}

} // namespace

std::string_view to_string(WebNotificationEvent event) noexcept {
    switch (event) {
    case WebNotificationEvent::Shown: return "shown";
    case WebNotificationEvent::Clicked: return "clicked";
    case WebNotificationEvent::Closed: return "closed";
    }
    return "closed";
}

WebNotificationHost::WebNotificationHost(exec::UiMarshal& marshal, std::wstring app_id)
    : m_marshal(marshal), m_app_id(std::move(app_id)) {}

WebNotificationHost::~WebNotificationHost() {
    m_alive->store(false, std::memory_order_release);
    if (m_toasts != nullptr) {
        m_toasts->set_event_callback(nullptr);
    }
    while (!m_active.empty()) {
        finish(m_active.back().info.id, true, true);
    }
    for (const Attachment& attachment : m_attachments) {
        if (attachment.webview != nullptr) {
            attachment.webview->events().on_notification_received = nullptr;
        }
    }
}

bool WebNotificationHost::is_supported(const core::WebView& webview) noexcept {
    ICoreWebView2* core = webview.core_webview();
    if (core == nullptr) {
        return false;
    }
    ComPtr<ICoreWebView2_24> webview24;
    return SUCCEEDED(core->QueryInterface(IID_PPV_ARGS(&webview24))) && webview24;
}

bool WebNotificationHost::attach(core::WebView& webview, HWND focus_window) {
    const HWND focus = focus_window != nullptr ? focus_window : webview.parent_hwnd();
    const auto it = std::ranges::find_if(m_attachments, [&webview](const Attachment& a) { return a.webview == &webview; });
    if (it != m_attachments.end()) {
        it->focus = focus;
    } else {
        m_attachments.push_back(Attachment{.webview = &webview, .focus = focus});
    }
    const std::uint64_t id = webview.id();
    webview.events().on_notification_received = [this, id, focus](ICoreWebView2NotificationReceivedEventArgs* args) {
        on_received(id, focus, args);
    };
    return is_supported(webview);
}

void WebNotificationHost::detach(core::WebView& webview) noexcept {
    const auto it = std::ranges::find_if(m_attachments, [&webview](const Attachment& a) { return a.webview == &webview; });
    if (it == m_attachments.end()) {
        return;
    }
    webview.events().on_notification_received = nullptr;
    m_attachments.erase(it);
    const std::uint64_t id = webview.id();
    std::vector<std::uint64_t> ids;
    for (const Entry& entry : m_active) {
        if (entry.info.webview_id == id) {
            ids.push_back(entry.info.id);
        }
    }
    for (const std::uint64_t nid : ids) {
        finish(nid, true, true);
    }
}

shell::ToastNotifier& WebNotificationHost::toasts() {
    if (m_toasts == nullptr) {
        m_toasts = std::make_unique<shell::ToastNotifier>(m_app_id);
        m_toasts->set_event_callback([this, alive = m_alive](const shell::ToastEvent& event) {
            if (!alive->load(std::memory_order_acquire) || event.group != kToastGroup) {
                return;
            }
            const int kind = event.kind == shell::ToastEventKind::Activated   ? kActivated
                             : event.kind == shell::ToastEventKind::Dismissed ? kDismissed
                                                                              : kFailed;
            const int reason = event.reason == shell::ToastDismissReason::TimedOut            ? kTimedOut
                               : event.reason == shell::ToastDismissReason::ApplicationHidden ? kApplicationHidden
                                                                                              : kUserCanceled;
            m_marshal.post_task([this, alive, tag = event.tag, kind, reason]() {
                if (alive->load(std::memory_order_acquire)) {
                    on_toast_event(tag, kind, reason);
                }
            });
        });
    }
    return *m_toasts;
}

void WebNotificationHost::on_received(std::uint64_t webview_id, HWND focus, ICoreWebView2NotificationReceivedEventArgs* args) {
    if (args == nullptr) {
        return;
    }
    ComPtr<ICoreWebView2Notification> notification;
    if (!BOSON_HR_CHECK(args->get_Notification(&notification)) || !notification) {
        return;
    }
    BOSON_HR_CHECK(args->put_Handled(TRUE));

    Entry entry;
    entry.native = notification;
    entry.focus = focus;
    WebNotification& info = entry.info;
    info.id = m_next_id++;
    info.webview_id = webview_id;
    info.origin = security::normalized_origin(get_string([&args](LPWSTR* v) { return args->get_SenderOrigin(v); }));
    info.title = get_string([&notification](LPWSTR* v) { return notification->get_Title(v); });
    info.body = get_string([&notification](LPWSTR* v) { return notification->get_Body(v); });
    info.tag = get_string([&notification](LPWSTR* v) { return notification->get_Tag(v); });
    info.icon_uri = get_string([&notification](LPWSTR* v) { return notification->get_IconUri(v); });
    info.badge_uri = get_string([&notification](LPWSTR* v) { return notification->get_BadgeUri(v); });
    info.image_uri = get_string([&notification](LPWSTR* v) { return notification->get_BodyImageUri(v); });
    info.language = get_string([&notification](LPWSTR* v) { return notification->get_Language(v); });
    BOOL flag = FALSE;
    HRESULT hr = notification->get_IsSilent(&flag);
    info.silent = get_bool(hr, flag);
    flag = FALSE;
    hr = notification->get_RequiresInteraction(&flag);
    info.requires_interaction = get_bool(hr, flag);
    flag = FALSE;
    hr = notification->get_ShouldRenotify(&flag);
    info.renotify = get_bool(hr, flag);
    double timestamp = 0.0;
    if (SUCCEEDED(notification->get_Timestamp(&timestamp))) {
        info.timestamp = timestamp;
    }
    entry.toast_tag = "bw-" + std::to_string(info.id);

    // Same origin + tag replaces the previous notification (Notifications API "tag" semantics).
    if (!info.tag.empty()) {
        std::vector<std::uint64_t> replaced;
        for (const Entry& other : m_active) {
            if (other.info.origin == info.origin && other.info.tag == info.tag) {
                replaced.push_back(other.info.id);
            }
        }
        for (const std::uint64_t old_id : replaced) {
            finish(old_id, false, true); // the runtime closes the replaced notification itself
        }
    }

    const std::uint64_t id = info.id;
    BOSON_HR_CHECK(notification->add_CloseRequested(
        Callback<ICoreWebView2NotificationCloseRequestedEventHandler>(
            [this, alive = m_alive, id](ICoreWebView2Notification*, IUnknown*) -> HRESULT {
                if (alive->load(std::memory_order_acquire)) {
                    finish(id, false, true); // the page closed it; the runtime already knows
                }
                return S_OK;
            })
            .Get(),
        &entry.close_token));

    m_active.push_back(std::move(entry));
    const bool shown = present(m_active.back());
    Entry* current = find(id);
    if (current == nullptr) {
        return;
    }
    if (!shown) {
        finish(id, true, false);
        return;
    }
    BOSON_HR_CHECK(current->native->ReportShown());
    emit(current->info, WebNotificationEvent::Shown);
}

bool WebNotificationHost::present(const Entry& entry) {
    if (m_presenter) {
        try {
            return m_presenter(entry.info);
        } catch (...) {
            return false;
        }
    }
    shell::ToastContent content;
    content.title = entry.info.title;
    content.body = entry.info.body;
    content.attribution = entry.info.origin;
    if (entry.info.icon_uri.starts_with("file:")) {
        content.app_logo = entry.info.icon_uri;
    }
    if (entry.info.image_uri.starts_with("file:")) {
        content.hero_image = entry.info.image_uri;
    }
    content.silent = entry.info.silent;
    content.duration = entry.info.requires_interaction ? shell::ToastDuration::Long : shell::ToastDuration::Default;
    content.tag = entry.toast_tag;
    content.group = std::string(kToastGroup);
    content.launch_arguments = "boson-web-notification=" + std::to_string(entry.info.id);
    if (shell::ToastNotifier::is_supported() && toasts().show(content).ok) {
        return true;
    }
    if (m_fallback == nullptr) {
        m_fallback = std::make_unique<Notifier>(entry.focus);
    }
    m_fallback->set_owner(entry.focus);
    return m_fallback->show(NotificationOptions{
        .title = entry.info.title,
        .body = entry.info.body,
        .level = NotificationLevel::Info,
        .silent = entry.info.silent,
    });
}

void WebNotificationHost::on_toast_event(std::string_view tag, int kind, int reason) {
    Entry* entry = find_by_toast(tag);
    if (entry == nullptr) {
        return;
    }
    const std::uint64_t id = entry->info.id;
    switch (kind) {
    case kActivated:
        click(id);
        break;
    case kDismissed:
        if (reason != kTimedOut) {
            finish(id, true, false);
        }
        break;
    default:
        finish(id, true, false);
        break;
    }
}

bool WebNotificationHost::click(std::uint64_t id) {
    Entry* entry = find(id);
    if (entry == nullptr) {
        return false;
    }
    BOSON_HR_CHECK(entry->native->ReportClicked());
    if (m_focus_on_click) {
        focus_window(entry->focus);
    }
    const WebNotification info = entry->info;
    emit(info, WebNotificationEvent::Clicked);
    // A clicked toast leaves the screen. The page only sees "click" (the runtime rejects ReportClosed after
    // ReportClicked); it calls notification.close() itself if it wants to.
    finish(id, false, true);
    return true;
}

bool WebNotificationHost::close(std::uint64_t id) {
    if (find(id) == nullptr) {
        return false;
    }
    finish(id, true, true);
    return true;
}

void WebNotificationHost::finish(std::uint64_t id, bool report_closed, bool remove_toast) {
    const auto it = std::ranges::find_if(m_active, [id](const Entry& e) { return e.info.id == id; });
    if (it == m_active.end()) {
        return;
    }
    Entry entry = std::move(*it);
    m_active.erase(it);
    if (entry.close_token.value != 0) {
        entry.native->remove_CloseRequested(entry.close_token);
    }
    if (remove_toast && !m_presenter && m_toasts != nullptr) {
        m_toasts->remove(entry.toast_tag, kToastGroup);
    }
    if (report_closed) {
        BOSON_HR_CHECK(entry.native->ReportClosed());
    }
    emit(entry.info, WebNotificationEvent::Closed);
}

void WebNotificationHost::emit(const WebNotification& notification, WebNotificationEvent event) {
    if (!m_on_event) {
        return;
    }
    try {
        m_on_event(notification, event);
    } catch (...) {
        // Callbacks run inside WebView2 / UI dispatch; never let them unwind further.
    }
}

WebNotificationHost::Entry* WebNotificationHost::find(std::uint64_t id) noexcept {
    const auto it = std::ranges::find_if(m_active, [id](const Entry& e) { return e.info.id == id; });
    return it == m_active.end() ? nullptr : &*it;
}

WebNotificationHost::Entry* WebNotificationHost::find_by_toast(std::string_view tag) noexcept {
    const auto it = std::ranges::find_if(m_active, [tag](const Entry& e) { return e.toast_tag == tag; });
    return it == m_active.end() ? nullptr : &*it;
}

std::vector<WebNotification> WebNotificationHost::active() const {
    std::vector<WebNotification> out;
    out.reserve(m_active.size());
    for (const Entry& entry : m_active) {
        out.push_back(entry.info);
    }
    return out;
}

} // namespace boson::native
