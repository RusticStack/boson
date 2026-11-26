#include "boson/ipc/bridge.hpp"

#include <boson/core/result.hpp>
#include <boson/ipc/request_context.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace boson::ipc {

namespace {

std::string to_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
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
      m_dispatch_fn(std::move(dispatch_fn)) {}

Bridge::~Bridge() {
    *m_alive = false;
    for (const auto& attachment : m_attachments) {
        attachment.webview->set_message_callback({});
    }
    m_attachments.clear();
}

std::string Bridge::origin_of(std::string_view uri) {
    const auto scheme_end = uri.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return {};
    }
    const auto authority_start = scheme_end + 3;
    auto authority_end = uri.find_first_of("/?#", authority_start);
    if (authority_end == std::string_view::npos) {
        authority_end = uri.size();
    }
    std::string_view authority = uri.substr(authority_start, authority_end - authority_start);
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        authority.remove_prefix(at + 1); // strip userinfo
    }
    if (authority.empty()) {
        return {};
    }
    std::string out = to_lower(uri.substr(0, scheme_end));
    out += "://";
    out += to_lower(authority);
    return out;
}

bool Bridge::is_origin_allowed(std::string_view source_uri, std::span<const std::string> allowed) {
    const std::string origin = origin_of(source_uri);
    for (const auto& entry : allowed) {
        if (entry == "*") {
            return true;
        }
        if (!origin.empty() && origin == to_lower(trim_trailing_slash(entry))) {
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

void Bridge::deliver(std::uint64_t webview_id, std::string_view json) const {
    if (webview_id == kAllWebViews) {
        for (const auto& attachment : m_attachments) {
            attachment.webview->post_web_message_as_json(json);
        }
        return;
    }
    if (const Attachment* attachment = find(webview_id); attachment != nullptr) {
        attachment->webview->post_web_message_as_json(json);
    }
}

bool Bridge::emit_json(std::uint64_t webview_id, std::string_view event, std::string_view data_json) {
    try {
        std::string envelope = make_event_envelope(event, data_json);
        m_marshal.post_task([this, alive = m_alive, webview_id, envelope = std::move(envelope)]() {
            if (*alive) {
                deliver(webview_id, envelope);
            }
        });
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace boson::ipc
