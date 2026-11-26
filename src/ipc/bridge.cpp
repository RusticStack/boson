#include "boson/ipc/bridge.hpp"

#include <boson/core/result.hpp>
#include <boson/core/text_convert.hpp>
#include <boson/ipc/request_context.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace boson::ipc {

namespace {

char ascii_lower(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

std::string to_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(ascii_lower(ch));
    }
    return out;
}

bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

/// scheme and authority (userinfo stripped) of uri as views into it; false for opaque or malformed URIs.
bool split_origin(std::string_view uri, std::string_view& scheme, std::string_view& authority) noexcept {
    const auto scheme_end = uri.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return false;
    }
    const auto authority_start = scheme_end + 3;
    auto authority_end = uri.find_first_of("/?#", authority_start);
    if (authority_end == std::string_view::npos) {
        authority_end = uri.size();
    }
    authority = uri.substr(authority_start, authority_end - authority_start);
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        authority.remove_prefix(at + 1); // strip userinfo
    }
    scheme = uri.substr(0, scheme_end);
    return !authority.empty();
}

std::string_view trim_trailing_slash(std::string_view text) noexcept {
    while (!text.empty() && text.back() == '/') {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

Bridge::Bridge(exec::TaskRunner& runner, exec::UiMarshal& marshal, DispatchCallback dispatch_fn)
    : m_runner(runner),
      m_marshal(marshal),
      m_dispatch_fn(std::move(dispatch_fn)) {
    m_marshal.add_drain_hook(this, [this]() { flush_events(); });
}

Bridge::~Bridge() {
    m_marshal.remove_drain_hook(this);
    *m_alive = false;
    for (const auto& attachment : m_attachments) {
        attachment.webview->set_message_callback({});
    }
    m_attachments.clear();
}

std::string Bridge::origin_of(std::string_view uri) {
    std::string_view scheme;
    std::string_view authority;
    if (!split_origin(uri, scheme, authority)) {
        return {};
    }
    std::string out = to_lower(scheme);
    out += "://";
    out += to_lower(authority);
    return out;
}

bool Bridge::is_origin_allowed(std::string_view source_uri, std::span<const std::string> allowed) {
    // Allocation-free: this runs for every IPC message. The origin is compared as views, case-insensitively.
    std::string_view scheme;
    std::string_view authority;
    const bool has_origin = split_origin(source_uri, scheme, authority);
    for (const auto& entry : allowed) {
        if (entry == "*") {
            return true;
        }
        if (!has_origin) {
            continue;
        }
        const std::string_view pattern = trim_trailing_slash(entry);
        // pattern must read "<scheme>://<authority>" (the origin is never longer than that form).
        if (pattern.size() == scheme.size() + 3 + authority.size() &&
            equals_ignore_case(pattern.substr(0, scheme.size()), scheme) &&
            pattern.substr(scheme.size(), 3) == "://" &&
            equals_ignore_case(pattern.substr(scheme.size() + 3), authority)) {
            return true;
        }
    }
    return false;
}

void Bridge::attach(core::WebView& webview, BridgeAttachOptions options) {
    Attachment attachment{
        .webview = &webview,
        .id = webview.id(),
        .allowed_origins = std::move(options.allowed_origins),
        .capabilities = std::move(options.capabilities),
        .window_label = std::move(options.window_label),
        .batch_events = options.batch_events,
    };
    if (attachment.allowed_origins.empty()) {
        attachment.allowed_origins.emplace_back("boson://app");
        if (const auto& dev = webview.config().dev_url; dev.has_value()) {
            if (std::string dev_origin = origin_of(*dev); !dev_origin.empty()) {
                attachment.allowed_origins.push_back(std::move(dev_origin));
            }
        }
    }

    detach(webview);
    m_attachments.push_back(std::move(attachment));

    const std::uint64_t id = webview.id();
    webview.set_message_callback([this, id](std::string_view raw_json, std::string_view source_uri) {
        on_web_message(id, raw_json, source_uri);
    });
}

void Bridge::detach(core::WebView& webview) noexcept {
    flush_events(); // events queued for this WebView must not be lost (or be sent to a later attachment)
    const auto it = std::ranges::find(m_attachments, &webview, &Attachment::webview);
    if (it != m_attachments.end()) {
        webview.set_message_callback({});
        m_attachments.erase(it);
    }
}

bool Bridge::is_attached(std::uint64_t webview_id) const noexcept {
    return find(webview_id) != nullptr;
}

bool Bridge::set_capabilities(std::uint64_t webview_id, std::shared_ptr<const security::CapabilityPolicy> policy) noexcept {
    const auto it = std::ranges::find(m_attachments, webview_id, &Attachment::id);
    if (it == m_attachments.end()) {
        return false;
    }
    it->capabilities = std::move(policy);
    return true;
}

bool Bridge::set_window_label(std::uint64_t webview_id, std::string window_label) {
    const auto it = std::ranges::find(m_attachments, webview_id, &Attachment::id);
    if (it == m_attachments.end()) {
        return false;
    }
    it->window_label = std::move(window_label);
    return true;
}

std::string_view Bridge::window_label_of(std::uint64_t webview_id) const noexcept {
    const Attachment* attachment = find(webview_id);
    return attachment == nullptr ? std::string_view{} : std::string_view{attachment->window_label};
}

const Bridge::Attachment* Bridge::find(std::uint64_t webview_id) const noexcept {
    const auto it = std::ranges::find(m_attachments, webview_id, &Attachment::id);
    return it == m_attachments.end() ? nullptr : &*it;
}

core::WebView::MessageCallback Bridge::make_message_callback() {
    if (m_attachments.empty()) {
        return {};
    }
    const std::uint64_t id = m_attachments.front().id;
    return [this, id](std::string_view raw_json, std::string_view source_uri) {
        on_web_message(id, raw_json, source_uri);
    };
}

void Bridge::on_web_message(std::uint64_t webview_id, std::string_view raw_json, std::string_view source_uri) {
    const Attachment* attachment = find(webview_id);
    if (attachment == nullptr || !m_dispatch_fn) {
        return;
    }
    if (!is_origin_allowed(source_uri, attachment->allowed_origins)) {
        core::hr_check(E_ACCESSDENIED, "Bridge: dropped IPC message from untrusted origin");
        return;
    }
    if (attachment->capabilities) {
        // Allocation-free header peek + compiled policy lookup; params are never parsed for a denied call.
        const auto header = security::peek_rpc_header(raw_json);
        const security::PermissionDecision decision = header.has_value()
            ? attachment->capabilities->check(header->method, source_uri)
            : security::PermissionDecision::Malformed;
        if (decision != security::PermissionDecision::Allowed) {
            core::hr_check(E_ACCESSDENIED, "Bridge: IPC call rejected by capability policy");
            deliver(webview_id, security::make_permission_denied_envelope(
                header.has_value() ? header->id : 0, header.has_value() ? header->method : std::string_view{}, decision));
            return;
        }
    }
    const SenderScope sender_scope{webview_id}; // Router carries it to the pool thread
    m_dispatch_fn(raw_json, [this, alive = m_alive, webview_id](std::string response) {
        if (*alive) {
            deliver(webview_id, response);
        }
    });
}

void Bridge::deliver(std::uint64_t webview_id, std::string_view json) {
    flush_events();
    deliver_raw(webview_id, json);
}

bool Bridge::batches_events_for(std::uint64_t webview_id) const noexcept {
    if (webview_id == kAllWebViews) {
        return !m_attachments.empty() &&
               std::ranges::all_of(m_attachments, [](const Attachment& attachment) { return attachment.batch_events; });
    }
    const Attachment* attachment = find(webview_id);
    return attachment != nullptr && attachment->batch_events;
}

void Bridge::deliver_event(std::uint64_t webview_id, std::string envelope) {
    constexpr std::size_t kMaxBatchBytes = 256 * 1024;
    constexpr std::uint32_t kMaxBatchEvents = 512;
    if (!batches_events_for(webview_id)) {
        deliver(webview_id, envelope);
        return;
    }
    if (m_batch_count != 0 && m_batch_target != webview_id) {
        flush_events();
    }
    if (m_batch_count == 0) {
        m_batch.assign(1, '[');
        m_batch_target = webview_id;
    } else {
        m_batch.push_back(',');
    }
    m_batch.append(envelope);
    if (++m_batch_count == 1) {
        m_batch_first_end = m_batch.size();
    }
    if (m_batch_count >= kMaxBatchEvents || m_batch.size() >= kMaxBatchBytes) {
        flush_events();
    }
}

void Bridge::flush_events() {
    if (m_batch_count == 0) {
        return;
    }
    const std::uint32_t count = std::exchange(m_batch_count, 0U);
    if (count == 1) {
        deliver_raw(m_batch_target, std::string_view{m_batch}.substr(1, m_batch_first_end - 1)); // a lone event stays unwrapped
    } else {
        m_batch.push_back(']');
        deliver_raw(m_batch_target, m_batch);
    }
    if (m_batch.capacity() > 1024 * 1024) {
        std::string{}.swap(m_batch);
    }
}

void Bridge::deliver_raw(std::uint64_t webview_id, std::string_view json) const {
    if (webview_id == kAllWebViews) {
        if (m_attachments.size() == 1) {
            m_attachments.front().webview->post_web_message_as_json(json);
            return;
        }
        // Convert once for every WebView instead of once per WebView.
        thread_local std::wstring wide;
        core::utf8_to_wide_into(json, wide);
        for (const auto& attachment : m_attachments) {
            attachment.webview->post_web_message_as_json_wide(wide);
        }
        return;
    }
    if (const Attachment* attachment = find(webview_id); attachment != nullptr) {
        attachment->webview->post_web_message_as_json(json);
    }
}

void Bridge::deliver_to_window(std::string_view window_label, std::string_view json) {
    flush_events();
    for (const auto& attachment : m_attachments) {
        if (attachment.window_label == window_label) {
            attachment.webview->post_web_message_as_json(json);
        }
    }
}

bool Bridge::emit_json_to_window(std::string_view window_label, std::string_view event, std::string_view data_json) {
    try {
        m_marshal.post_reply(
            [this, alive = m_alive, label = std::string(window_label)](std::string envelope) {
                if (*alive) {
                    deliver_to_window(label, envelope);
                }
            },
            make_event_envelope(event, data_json));
        return true;
    } catch (...) {
        return false;
    }
}

bool Bridge::emit_json(std::uint64_t webview_id, std::string_view event, std::string_view data_json) {
    try {
        // The envelope rides in the queue item; the closure (this, alive, id) fits std::function's inline buffer.
        m_marshal.post_reply(
            [this, alive = m_alive, webview_id](std::string envelope) {
                if (*alive) {
                    deliver_event(webview_id, std::move(envelope));
                }
            },
            make_event_envelope(event, data_json));
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace boson::ipc
