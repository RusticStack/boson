// WSAAsyncSelect delivers socket readiness as window messages to the UI thread: no polling, no I/O thread.
// It is marked deprecated in favour of WSAEventSelect, which would need a waiting thread or MsgWait loop.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>

#include <boson/webview/cdp_server.hpp>

#include <boson/core/result.hpp>
#include <boson/transfer/base64.hpp>

#include <bcrypt.h>
#include <WebView2.h>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <utility>

namespace boson::webview {

namespace cdpjson {

/// Page.getFrameTree, only the main frame id (Chromium uses it as the page's target id).
struct FrameId {
    std::string id;
};
struct FrameTreeNode {
    FrameId frame;
};
struct FrameTreeResult {
    FrameTreeNode frameTree;
};

struct Inbound {
    std::int64_t id = 0;
    std::string method;
    std::optional<glz::raw_json> params;
    std::optional<std::string> sessionId;
};

struct TargetInfo {
    std::string targetId;
    std::string type;
    std::string title;
    std::string url;
    bool attached = false;
    bool canAccessOpener = false;
    std::string browserContextId;
    std::optional<std::string> openerId;
};

struct AttachedToTarget {
    std::string sessionId;
    TargetInfo targetInfo;
    bool waitingForDebugger = false;
};

struct DetachedFromTarget {
    std::string sessionId;
    std::string targetId;
};

struct TargetInfoEnvelope {
    TargetInfo targetInfo;
};

struct TargetIdOnly {
    std::string targetId;
};

struct TargetInfos {
    std::vector<TargetInfo> targetInfos;
};

struct AutoAttachParams {
    bool autoAttach = false;
    bool waitForDebuggerOnStart = false;
    bool flatten = false;
};

struct DiscoverParams {
    bool discover = false;
};

struct TargetParams {
    std::string targetId;
    bool flatten = true;
};

struct SessionParams {
    std::string sessionId;
    std::string targetId;
};

struct CreateTargetParams {
    std::string url;
    std::string browserContextId;
    bool newWindow = false;
    bool background = false;
};

struct ContextParams {
    std::string browserContextId;
};

struct ContextIds {
    std::vector<std::string> browserContextIds;
};

struct SessionIdResult {
    std::string sessionId;
};

struct SuccessResult {
    bool success = true;
};

struct VersionResult {
    std::string protocolVersion = "1.3";
    std::string product;
    std::string revision = "@boson";
    std::string userAgent;
    std::string jsVersion;
};

struct HttpVersion {
    std::string Browser;
    std::string protocol_version = "1.3";
    std::string user_agent;
    std::string v8_version;
    std::string webkit_version;
    std::string webSocketDebuggerUrl;
};

struct HttpTarget {
    std::string description;
    std::string devtoolsFrontendUrl;
    std::string id;
    std::string title;
    std::string type;
    std::string url;
    std::string webSocketDebuggerUrl;
};

struct WindowBounds {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::string windowState = "normal";
};

struct WindowForTarget {
    std::int32_t windowId = 1;
    WindowBounds bounds;
};

struct CookiesParams {
    std::optional<glz::raw_json> cookies;
    std::string browserContextId;
};

struct ChildSessionParams {
    std::string sessionId;
};

} // namespace cdpjson

} // namespace boson::webview

template <>
struct glz::meta<boson::webview::cdpjson::HttpVersion> {
    using T = boson::webview::cdpjson::HttpVersion;
    static constexpr auto value = glz::object(
        "Browser", &T::Browser,
        "Protocol-Version", &T::protocol_version,
        "User-Agent", &T::user_agent,
        "V8-Version", &T::v8_version,
        "WebKit-Version", &T::webkit_version,
        "webSocketDebuggerUrl", &T::webSocketDebuggerUrl);
};

namespace boson::webview {

namespace {

constexpr glz::opts kLenient{.error_on_unknown_keys = false};
constexpr UINT kSocketMessage = WM_APP + 0x42;
constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t kMaxHeaderBytes = 16 * 1024;

std::string json_string(std::string_view text) {
    std::string out;
    if (glz::write_json(text, out)) {
        return "\"\"";
    }
    return out;
}

template <typename T>
std::string to_json(const T& value) {
    std::string out;
    if (glz::write_json(value, out)) {
        return "{}";
    }
    return out;
}

std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        // Never fall back to a predictable token.
        std::fill(buf.begin(), buf.end(), static_cast<unsigned char>(0));
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        std::memcpy(buf.data(), &counter, std::min(buf.size(), sizeof(counter)));
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes * 2);
    for (const unsigned char b : buf) {
        out.push_back(kHex[b >> 4U]);
        out.push_back(kHex[b & 0xFU]);
    }
    return out;
}

std::string webview2_version() {
    LPWSTR version = nullptr;
    std::string out;
    if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) && version != nullptr) {
        out = core::wide_to_utf8(version);
        // "131.0.2903.86 canary" -> "131.0.2903.86"
        if (const auto space = out.find(' '); space != std::string::npos) {
            out.resize(space);
        }
    }
    if (version != nullptr) {
        CoTaskMemFree(version);
    }
    return out.empty() ? std::string("0.0.0.0") : out;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool contains_token_ci(std::string_view header_value, std::string_view token) noexcept {
    std::size_t pos = 0;
    while (pos <= header_value.size()) {
        const std::size_t comma = header_value.find(',', pos);
        std::string_view part = header_value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) {
            part.remove_prefix(1);
        }
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) {
            part.remove_suffix(1);
        }
        if (iequals(part, token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

std::string url_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

/// Domains whose state a disconnecting client leaves behind in the WebView's shared DevTools session.
constexpr std::string_view kDisableOnRelease[] = {
    "Fetch", "Network", "Debugger", "DOM", "CSS", "Overlay", "Performance", "Profiler", "HeapProfiler",
    "Log", "Animation", "Media", "Audits", "Accessibility", "Security", "ServiceWorker", "DOMStorage",
};

/// Page-session commands that may legitimately be unsupported by WebView2; an error becomes {}.
constexpr std::string_view kTolerant[] = {
    "Target.setAutoAttach",
    "Target.setDiscoverTargets",
    "Runtime.runIfWaitingForDebugger",
    "Emulation.setFocusEmulationEnabled",
    "Page.setInterceptFileChooserDialog",
    "Emulation.setEmulatedMedia",
    "Page.setFontFamilies",
    "Network.setBypassServiceWorker",
};

} // namespace

// ---- WebSocket --------------------------------------------------------------------------------------------

namespace ws {

ParseResult parse_frame(std::string_view data, Frame& out, std::size_t max_payload, bool require_mask) {
    if (data.size() < 2) {
        return {ParseStatus::NeedMore, 0};
    }
    const auto b0 = static_cast<std::uint8_t>(data[0]);
    const auto b1 = static_cast<std::uint8_t>(data[1]);
    if ((b0 & 0x70U) != 0) {
        return {ParseStatus::Error, 0}; // RSV bits: no extension was negotiated
    }
    const bool masked = (b1 & 0x80U) != 0;
    if (require_mask && !masked) {
        return {ParseStatus::Error, 0};
    }
    std::uint64_t length = b1 & 0x7FU;
    std::size_t offset = 2;
    if (length == 126) {
        if (data.size() < 4) {
            return {ParseStatus::NeedMore, 0};
        }
        length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[2])) << 8U) | static_cast<std::uint8_t>(data[3]);
        offset = 4;
    } else if (length == 127) {
        if (data.size() < 10) {
            return {ParseStatus::NeedMore, 0};
        }
        length = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            length = (length << 8U) | static_cast<std::uint8_t>(data[2 + i]);
        }
        offset = 10;
    }
    const auto opcode = static_cast<Opcode>(b0 & 0x0FU);
    const bool control = (b0 & 0x08U) != 0;
    if (control && (length > 125 || (b0 & 0x80U) == 0)) {
        return {ParseStatus::Error, 0};
    }
    if (length > max_payload) {
        return {ParseStatus::Error, 0};
    }
    std::array<std::uint8_t, 4> mask{};
    if (masked) {
        if (data.size() < offset + 4) {
            return {ParseStatus::NeedMore, 0};
        }
        for (std::size_t i = 0; i < 4; ++i) {
            mask[i] = static_cast<std::uint8_t>(data[offset + i]);
        }
        offset += 4;
    }
    if (data.size() - offset < length) {
        return {ParseStatus::NeedMore, 0};
    }
    out.fin = (b0 & 0x80U) != 0;
    out.opcode = opcode;
    out.payload.assign(data.substr(offset, static_cast<std::size_t>(length)));
    if (masked) {
        for (std::size_t i = 0; i < out.payload.size(); ++i) {
            out.payload[i] = static_cast<char>(static_cast<std::uint8_t>(out.payload[i]) ^ mask[i & 3U]);
        }
    }
    return {ParseStatus::Complete, offset + static_cast<std::size_t>(length)};
}

void append_frame(std::string& out, Opcode opcode, std::string_view payload, bool fin, std::uint32_t mask_key) {
    const std::uint8_t b0 = static_cast<std::uint8_t>((fin ? 0x80U : 0U) | static_cast<std::uint8_t>(opcode));
    const std::uint8_t mask_bit = mask_key != 0 ? 0x80U : 0U;
    out.push_back(static_cast<char>(b0));
    const std::uint64_t size = payload.size();
    if (size < 126) {
        out.push_back(static_cast<char>(mask_bit | static_cast<std::uint8_t>(size)));
    } else if (size <= 0xFFFF) {
        out.push_back(static_cast<char>(mask_bit | 126U));
        out.push_back(static_cast<char>((size >> 8U) & 0xFFU));
        out.push_back(static_cast<char>(size & 0xFFU));
    } else {
        out.push_back(static_cast<char>(mask_bit | 127U));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((size >> static_cast<unsigned>(shift)) & 0xFFU));
        }
    }
    if (mask_key == 0) {
        out.append(payload);
        return;
    }
    const std::array<std::uint8_t, 4> mask{
        static_cast<std::uint8_t>(mask_key >> 24U), static_cast<std::uint8_t>(mask_key >> 16U),
        static_cast<std::uint8_t>(mask_key >> 8U), static_cast<std::uint8_t>(mask_key)};
    for (const std::uint8_t m : mask) {
        out.push_back(static_cast<char>(m));
    }
    const std::size_t start = out.size();
    out.append(payload);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        out[start + i] = static_cast<char>(static_cast<std::uint8_t>(out[start + i]) ^ mask[i & 3U]);
    }
}

std::string accept_key(std::string_view client_key) {
    std::string input(client_key);
    input += kWebSocketGuid;
    std::array<std::byte, 20> digest{};
    if (!BCRYPT_SUCCESS(BCryptHash(
            BCRYPT_SHA1_ALG_HANDLE,
            nullptr,
            0,
            reinterpret_cast<PUCHAR>(input.data()),
            static_cast<ULONG>(input.size()),
            reinterpret_cast<PUCHAR>(digest.data()),
            static_cast<ULONG>(digest.size())))) {
        return {};
    }
    return transfer::base64_encode(digest);
}

} // namespace ws

// ---- Server ------------------------------------------------------------------------------------------------

struct CdpServer::Impl {
    struct Target {
        std::uint64_t id = 0;
        std::string target_id;
        core::WebView* webview = nullptr;
        CdpTargetOptions options;
        std::unique_ptr<Cdp> cdp;
        std::vector<std::string> domains;
        std::vector<Cdp::SubscriptionId> subs;
        std::vector<std::string> child_sessions;
        /// Until the real target id (the main frame id) is known the target is not announced to clients.
        bool pending = true;
        std::vector<std::function<void(Target&)>> on_ready;
    };
    struct Session {
        std::string id;
        std::uint64_t target = 0;
    };
    struct Connection {
        std::uint64_t serial = 0;
        SOCKET socket = INVALID_SOCKET;
        std::string in;
        std::string out;
        std::size_t out_offset = 0;
        bool websocket = false;
        bool close_after_flush = false;
        bool closed = false;
        std::uint64_t page_target = 0;
        bool auto_attach = false;
        bool discover = false;
        std::vector<Session> sessions;
        std::string fragment;
        bool fragmenting = false;
    };

    explicit Impl(CdpServerOptions o) : options(std::move(o)) {}

    CdpServerOptions options;
    std::string token;
    std::string browser_id;
    std::string version;
    std::string product;
    std::string user_agent;
    HWND hwnd = nullptr;
    SOCKET listener = INVALID_SOCKET;
    std::uint16_t port = 0;
    bool wsa_started = false;
    CreateTargetFn create_fn;
    CloseTargetFn close_fn;
    CdpServerStats stats;
    std::vector<std::unique_ptr<Target>> targets;
    std::vector<std::unique_ptr<Connection>> conns;
    std::vector<std::string> contexts;
    std::uint64_t next_serial = 1;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    // ---- lookup ----
    Target* find_target(std::uint64_t id) const noexcept {
        for (const auto& t : targets) {
            if (t->id == id) {
                return t.get();
            }
        }
        return nullptr;
    }
    Target* find_target(std::string_view target_id) const noexcept {
        for (const auto& t : targets) {
            if (!t->pending && t->target_id == target_id) {
                return t.get();
            }
        }
        return nullptr;
    }
    Target* find_by_child_session(std::string_view session) const noexcept {
        for (const auto& t : targets) {
            if (std::ranges::find(t->child_sessions, session) != t->child_sessions.end()) {
                return t.get();
            }
        }
        return nullptr;
    }
    Connection* find_conn(std::uint64_t serial) const noexcept {
        for (const auto& c : conns) {
            if (c->serial == serial && !c->closed) {
                return c.get();
            }
        }
        return nullptr;
    }
    static Session* find_session(Connection& c, std::string_view id) noexcept {
        for (auto& s : c.sessions) {
            if (s.id == id) {
                return &s;
            }
        }
        return nullptr;
    }
    bool target_in_use(std::uint64_t id) const noexcept {
        for (const auto& c : conns) {
            if (c->closed) {
                continue;
            }
            if (c->page_target == id) {
                return true;
            }
            for (const auto& s : c->sessions) {
                if (s.target == id) {
                    return true;
                }
            }
        }
        return false;
    }

    /// The CDP target id of a WebView (its main frame id once known).
    std::string target_id_for(std::uint64_t id) const {
        const Target* t = find_target(id);
        return t != nullptr ? t->target_id : target_id_of(id);
    }

    void when_target_ready(Target& t, std::function<void(Target&)> fn) {
        if (!t.pending) {
            fn(t);
        } else {
            t.on_ready.push_back(std::move(fn));
        }
    }

    // ---- JSON pieces ----
    cdpjson::TargetInfo target_info(const Target& t) const {
        cdpjson::TargetInfo info;
        info.targetId = t.target_id;
        info.type = t.options.type;
        info.title = t.webview->document_title().value_or(std::string{});
        info.url = t.webview->source().value_or(std::string{});
        info.attached = target_in_use(t.id);
        info.canAccessOpener = t.options.opener_id != 0;
        info.browserContextId = t.options.browser_context_id.empty() ? ("BOSONDEFAULT" + browser_id.substr(0, 20)) : t.options.browser_context_id;
        if (t.options.opener_id != 0) {
            info.openerId = target_id_for(t.options.opener_id);
        }
        if (info.title.empty()) {
            info.title = info.url;
        }
        return info;
    }

    std::string ws_base() const {
        return std::format("ws://127.0.0.1:{}/{}", port, token);
    }

    // ---- output ----
    void send_text(Connection& c, std::string_view text) {
        if (c.closed) {
            return;
        }
        ws::append_frame(c.out, ws::Opcode::Text, text);
        ++stats.messages_out;
        flush(c);
    }

    void send_result(Connection& c, std::int64_t id, std::string_view result, std::string_view session) {
        std::string msg;
        msg.reserve(result.size() + session.size() + 48);
        msg += "{\"id\":";
        msg += std::to_string(id);
        msg += ",\"result\":";
        msg += result.empty() ? std::string_view("{}") : result;
        if (!session.empty()) {
            msg += ",\"sessionId\":";
            msg += json_string(session);
        }
        msg += '}';
        send_text(c, msg);
    }

    void send_error_json(Connection& c, std::int64_t id, std::string_view error_json, std::string_view session) {
        std::string msg = "{\"id\":" + std::to_string(id) + ",\"error\":";
        msg += error_json;
        if (!session.empty()) {
            msg += ",\"sessionId\":";
            msg += json_string(session);
        }
        msg += '}';
        send_text(c, msg);
    }

    void send_error(Connection& c, std::int64_t id, int code, std::string_view message, std::string_view session) {
        send_error_json(c, id, std::format("{{\"code\":{},\"message\":{}}}", code, json_string(message)), session);
    }

    void send_event(Connection& c, std::string_view method, std::string_view params, std::string_view session) {
        std::string msg;
        msg.reserve(params.size() + method.size() + session.size() + 48);
        msg += "{\"method\":";
        msg += json_string(method);
        msg += ",\"params\":";
        msg += params.empty() ? std::string_view("{}") : params;
        if (!session.empty()) {
            msg += ",\"sessionId\":";
            msg += json_string(session);
        }
        msg += '}';
        send_text(c, msg);
    }

    void flush(Connection& c) {
        while (!c.closed && c.out_offset < c.out.size()) {
            const std::size_t chunk = std::min<std::size_t>(c.out.size() - c.out_offset, 1U << 20U);
            const int n = ::send(c.socket, c.out.data() + c.out_offset, static_cast<int>(chunk), 0);
            if (n == SOCKET_ERROR) {
                if (WSAGetLastError() != WSAEWOULDBLOCK) {
                    c.closed = true;
                }
                break; // FD_WRITE resumes
            }
            c.out_offset += static_cast<std::size_t>(n);
            stats.bytes_out += static_cast<std::uint64_t>(n);
        }
        if (c.out_offset == c.out.size()) {
            c.out.clear();
            c.out_offset = 0;
            if (c.close_after_flush && !c.closed) {
                ::shutdown(c.socket, SD_SEND);
                c.closed = true;
            }
        } else if (c.out_offset > (1U << 20U) && c.out_offset * 2 > c.out.size()) {
            c.out.erase(0, c.out_offset);
            c.out_offset = 0;
        }
    }

    void http_respond(Connection& c, std::string_view status, std::string_view content_type, std::string_view body) {
        c.out += std::format(
            "HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
            status,
            content_type,
            body.size());
        c.out += body;
        c.close_after_flush = true;
        flush(c);
    }

    // ---- sessions and targets ----
    void ensure_domain(Target& t, std::string_view domain) {
        if (domain.empty() || std::ranges::find(t.domains, domain) != t.domains.end()) {
            return;
        }
        t.domains.emplace_back(domain);
        const std::uint64_t tid = t.id;
        const Cdp::SubscriptionId sub = t.cdp->subscribe_domain(domain, [this, tid](std::string_view method, std::string_view params, std::string_view session) {
            on_target_event(tid, method, params, session);
        });
        if (sub != 0) {
            t.subs.push_back(sub);
        }
    }

    std::string attach(Connection& c, Target& t) {
        Session session{.id = random_hex(16), .target = t.id};
        c.sessions.push_back(session);
        ensure_domain(t, "Target");
        ensure_domain(t, "Inspector");
        cdpjson::AttachedToTarget event{.sessionId = session.id, .targetInfo = target_info(t), .waitingForDebugger = false};
        event.targetInfo.attached = true;
        send_event(c, "Target.attachedToTarget", to_json(event), {});
        return session.id;
    }

    void release_if_unused(std::uint64_t target_id) {
        Target* t = find_target(target_id);
        if (t == nullptr || target_in_use(target_id)) {
            return;
        }
        for (const auto& domain : t->domains) {
            if (std::ranges::find(kDisableOnRelease, std::string_view(domain)) != std::ranges::end(kDisableOnRelease)) {
                t->cdp->call(domain + ".disable", "{}", Cdp::ResultFn{});
            }
        }
        if (std::ranges::find(t->domains, std::string_view("Target")) != t->domains.end()) {
            t->cdp->call("Target.setAutoAttach", R"({"autoAttach":false,"waitForDebuggerOnStart":false})", Cdp::ResultFn{});
        }
        t->cdp->unsubscribe_all();
        t->subs.clear();
        t->domains.clear();
        t->child_sessions.clear();
    }

    void on_target_event(std::uint64_t tid, std::string_view method, std::string_view params, std::string_view session) {
        Target* t = find_target(tid);
        if (t == nullptr) {
            return;
        }
        if (method == "Target.attachedToTarget" || method == "Target.detachedFromTarget") {
            cdpjson::ChildSessionParams p;
            if (!glz::read<kLenient>(p, params) && !p.sessionId.empty()) {
                if (method == "Target.attachedToTarget") {
                    t->child_sessions.push_back(p.sessionId);
                } else {
                    std::erase(t->child_sessions, p.sessionId);
                }
            }
        }
        for (const auto& cp : conns) {
            Connection& c = *cp;
            if (c.closed || !c.websocket) {
                continue;
            }
            if (c.page_target == tid) {
                send_event(c, method, params, session);
                ++stats.events_forwarded;
                continue;
            }
            if (!session.empty()) {
                if (std::ranges::any_of(c.sessions, [tid](const Session& s) { return s.target == tid; })) {
                    send_event(c, method, params, session);
                    ++stats.events_forwarded;
                }
                continue;
            }
            for (const auto& s : c.sessions) {
                if (s.target == tid) {
                    send_event(c, method, params, s.id);
                    ++stats.events_forwarded;
                }
            }
        }
        reap();
    }

    void forward(Connection& c, Target& t, std::int64_t id, std::string_view method, std::string_view params, std::string reply_session, std::string child) {
        ensure_domain(t, cdp_domain_of(method));
        const bool tolerant = std::ranges::find(kTolerant, method) != std::ranges::end(kTolerant);
        const std::weak_ptr<bool> alive_w = alive;
        const std::uint64_t serial = c.serial;
        const bool started = t.cdp->call(
            method,
            params,
            [this, alive_w, serial, id, reply_session, tolerant](bool ok, std::string json) {
                const auto a = alive_w.lock();
                if (!a || !*a) {
                    return;
                }
                if (Connection* conn = find_conn(serial); conn != nullptr) {
                    if (ok || tolerant) {
                        send_result(*conn, id, ok ? std::string_view(json) : std::string_view("{}"), reply_session);
                    } else {
                        send_error_json(*conn, id, json, reply_session);
                    }
                }
                reap();
            },
            child);
        if (!started) {
            send_error(c, id, -32000, "Target is not ready", reply_session);
            return;
        }
        ++stats.commands_forwarded;
    }

    // ---- inbound messages ----
    void on_message(Connection& c, std::string_view text) {
        ++stats.messages_in;
        cdpjson::Inbound m;
        if (glz::read<kLenient>(m, text) || m.method.empty()) {
            send_error(c, m.id, -32700, "Message must be a valid JSON object with id and method", {});
            return;
        }
        const std::string params = m.params.has_value() && !m.params->str.empty() ? m.params->str : std::string("{}");
        const std::string session = m.sessionId.value_or(std::string{});

        if (!session.empty()) {
            if (Session* s = find_session(c, session); s != nullptr) {
                if (Target* t = find_target(s->target); t != nullptr) {
                    handle_page(c, *t, m.id, m.method, params, session, {});
                } else {
                    send_error(c, m.id, -32001, "Session with given id not found.", session);
                }
                return;
            }
            if (Target* t = find_by_child_session(session); t != nullptr &&
                std::ranges::any_of(c.sessions, [t](const Session& s) { return s.target == t->id; })) {
                forward(c, *t, m.id, m.method, params, session, session);
                return;
            }
            send_error(c, m.id, -32001, "Session with given id not found.", session);
            return;
        }
        if (c.page_target != 0) {
            if (Target* t = find_target(c.page_target); t != nullptr) {
                handle_page(c, *t, m.id, m.method, params, {}, {});
            } else {
                send_error(c, m.id, -32000, "Target closed", {});
            }
            return;
        }
        handle_browser(c, m.id, m.method, params);
    }

    void handle_page(Connection& c, Target& t, std::int64_t id, std::string_view method, std::string_view params, const std::string& session, const std::string& child) {
        if (method == "Page.close") {
            const bool ok = close_fn && close_fn(t.id);
            if (ok) {
                send_result(c, id, "{}", session);
            } else {
                send_error(c, id, -32000, "Target cannot be closed", session);
            }
            return;
        }
        if (method == "Runtime.enable") {
            // The WebView has one DevTools session shared by every client: when Runtime is already enabled
            // (console-message, another client) a second enable would not replay executionContextCreated.
            t.cdp->call("Runtime.disable", "{}", Cdp::ResultFn{});
        }
        forward(c, t, id, method, params, session, child);
    }

    void handle_browser(Connection& c, std::int64_t id, std::string_view method, std::string_view params) {
        if (method == "Browser.getVersion") {
            send_result(c, id, to_json(cdpjson::VersionResult{.product = product, .userAgent = user_agent}), {});
            return;
        }
        if (method == "Target.setAutoAttach") {
            cdpjson::AutoAttachParams p;
            (void)glz::read<kLenient>(p, params);
            c.auto_attach = p.autoAttach;
            if (p.autoAttach) {
                for (const auto& t : targets) {
                    if (!t->pending && !std::ranges::any_of(c.sessions, [&t](const Session& s) { return s.target == t->id; })) {
                        attach(c, *t);
                    }
                }
            }
            send_result(c, id, "{}", {});
            return;
        }
        if (method == "Target.setDiscoverTargets") {
            cdpjson::DiscoverParams p;
            (void)glz::read<kLenient>(p, params);
            c.discover = p.discover;
            if (p.discover) {
                for (const auto& t : targets) {
                    if (!t->pending) {
                        send_event(c, "Target.targetCreated", to_json(cdpjson::TargetInfoEnvelope{.targetInfo = target_info(*t)}), {});
                    }
                }
            }
            send_result(c, id, "{}", {});
            return;
        }
        if (method == "Target.getTargets") {
            cdpjson::TargetInfos out;
            for (const auto& t : targets) {
                if (!t->pending) {
                    out.targetInfos.push_back(target_info(*t));
                }
            }
            send_result(c, id, to_json(out), {});
            return;
        }
        if (method == "Target.getTargetInfo") {
            cdpjson::TargetParams p;
            (void)glz::read<kLenient>(p, params);
            if (p.targetId.empty() || p.targetId == browser_id) {
                cdpjson::TargetInfo info{.targetId = browser_id, .type = "browser", .title = "", .url = "", .attached = true,
                                         .canAccessOpener = false, .browserContextId = "", .openerId = std::nullopt};
                send_result(c, id, to_json(cdpjson::TargetInfoEnvelope{.targetInfo = info}), {});
            } else if (Target* t = find_target(p.targetId); t != nullptr) {
                send_result(c, id, to_json(cdpjson::TargetInfoEnvelope{.targetInfo = target_info(*t)}), {});
            } else {
                send_error(c, id, -32602, "No target with given id found", {});
            }
            return;
        }
        if (method == "Target.attachToTarget") {
            cdpjson::TargetParams p;
            (void)glz::read<kLenient>(p, params);
            Target* t = find_target(p.targetId);
            if (t == nullptr) {
                send_error(c, id, -32602, "No target with given id found", {});
                return;
            }
            if (!p.flatten) {
                send_error(c, id, -32000, "Only flattened sessions (flatten: true) are supported", {});
                return;
            }
            const std::string session = attach(c, *t);
            send_result(c, id, to_json(cdpjson::SessionIdResult{.sessionId = session}), {});
            return;
        }
        if (method == "Target.detachFromTarget") {
            cdpjson::SessionParams p;
            (void)glz::read<kLenient>(p, params);
            const auto it = std::ranges::find_if(c.sessions, [this, &p](const Session& s) {
                return s.id == p.sessionId || (p.sessionId.empty() && !p.targetId.empty() && target_id_for(s.target) == p.targetId);
            });
            if (it == c.sessions.end()) {
                send_error(c, id, -32602, "No session with given id", {});
                return;
            }
            const Session s = *it;
            c.sessions.erase(it);
            send_event(c, "Target.detachedFromTarget", to_json(cdpjson::DetachedFromTarget{.sessionId = s.id, .targetId = target_id_for(s.target)}), {});
            send_result(c, id, "{}", {});
            release_if_unused(s.target);
            return;
        }
        if (method == "Target.createTarget") {
            create_target(c, id, params);
            return;
        }
        if (method == "Target.closeTarget") {
            cdpjson::TargetParams p;
            (void)glz::read<kLenient>(p, params);
            Target* t = find_target(p.targetId);
            if (t == nullptr) {
                send_error(c, id, -32602, "No target with given id found", {});
                return;
            }
            const bool ok = close_fn && close_fn(t->id);
            send_result(c, id, to_json(cdpjson::SuccessResult{.success = ok}), {});
            return;
        }
        if (method == "Target.createBrowserContext") {
            std::string context = "BOSONCTX" + random_hex(12);
            contexts.push_back(context);
            send_result(c, id, std::format("{{\"browserContextId\":{}}}", json_string(context)), {});
            return;
        }
        if (method == "Target.getBrowserContexts") {
            send_result(c, id, to_json(cdpjson::ContextIds{.browserContextIds = contexts}), {});
            return;
        }
        if (method == "Target.disposeBrowserContext") {
            cdpjson::ContextParams p;
            (void)glz::read<kLenient>(p, params);
            if (std::erase(contexts, p.browserContextId) == 0) {
                send_error(c, id, -32602, "Failed to find context with id " + p.browserContextId, {});
                return;
            }
            std::vector<std::uint64_t> doomed;
            for (const auto& t : targets) {
                if (t->options.browser_context_id == p.browserContextId) {
                    doomed.push_back(t->id);
                }
            }
            for (const std::uint64_t target : doomed) {
                if (close_fn) {
                    close_fn(target);
                }
            }
            send_result(c, id, "{}", {});
            return;
        }
        if (method == "Browser.getWindowForTarget") {
            cdpjson::WindowForTarget out;
            Target* t = targets.empty() ? nullptr : targets.front().get();
            cdpjson::TargetParams p;
            if (!glz::read<kLenient>(p, params) && !p.targetId.empty()) {
                t = find_target(p.targetId);
            }
            if (t != nullptr && t->webview->parent_hwnd() != nullptr) {
                RECT r{};
                GetWindowRect(GetAncestor(t->webview->parent_hwnd(), GA_ROOT), &r);
                out.bounds = cdpjson::WindowBounds{.left = r.left, .top = r.top, .width = r.right - r.left, .height = r.bottom - r.top,
                                                   .windowState = "normal"};
            }
            send_result(c, id, to_json(out), {});
            return;
        }
        if (method == "Browser.getBrowserCommandLine") {
            send_result(c, id, R"({"arguments":[]})", {});
            return;
        }
        if (method == "Storage.getCookies" || method == "Storage.setCookies" || method == "Storage.clearCookies") {
            cdpjson::CookiesParams p;
            (void)glz::read<kLenient>(p, params);
            Target* t = nullptr;
            for (const auto& candidate : targets) {
                if (candidate->options.browser_context_id == p.browserContextId) {
                    t = candidate.get();
                    break;
                }
            }
            if (t == nullptr) {
                send_result(c, id, method == "Storage.getCookies" ? std::string_view(R"({"cookies":[]})") : std::string_view("{}"), {});
                return;
            }
            if (method == "Storage.getCookies") {
                forward(c, *t, id, "Network.getAllCookies", "{}", {}, {});
            } else if (method == "Storage.setCookies") {
                forward(c, *t, id, "Network.setCookies", "{\"cookies\":" + (p.cookies ? p.cookies->str : std::string("[]")) + "}", {}, {});
            } else {
                forward(c, *t, id, "Network.clearBrowserCookies", "{}", {}, {});
            }
            return;
        }
        if (method.starts_with("Browser.") || method.starts_with("Target.")) {
            // Browser.close, setDownloadBehavior, grantPermissions, setWindowBounds, Target.activateTarget, ...:
            // acknowledged without effect (the app owns its windows and lifetime).
            send_result(c, id, "{}", {});
            return;
        }
        send_error(c, id, -32601, std::format("'{}' wasn't found", method), {});
    }

    void create_target(Connection& c, std::int64_t id, std::string_view params) {
        cdpjson::CreateTargetParams p;
        (void)glz::read<kLenient>(p, params);
        if (!create_fn) {
            send_error(c, id, -32000, "Target.createTarget is not supported by this application", {});
            return;
        }
        if (!p.browserContextId.empty() && std::ranges::find(contexts, p.browserContextId) == contexts.end()) {
            send_error(c, id, -32602, "Failed to find browser context with id " + p.browserContextId, {});
            return;
        }
        const CdpCreateTargetRequest request{
            .url = p.url.empty() ? std::string("about:blank") : p.url,
            .browser_context_id = p.browserContextId,
            .new_window = p.newWindow,
            .background = p.background,
        };
        const std::weak_ptr<bool> alive_w = alive;
        const std::uint64_t serial = c.serial;
        const std::string context = p.browserContextId;
        create_fn(request, [this, alive_w, serial, id, context](core::WebView* webview) {
            const auto a = alive_w.lock();
            if (!a || !*a) {
                return;
            }
            if (webview != nullptr && find_target(webview->id()) == nullptr) {
                add_target_impl(*webview, CdpTargetOptions{.type = "page", .browser_context_id = context, .opener_id = 0});
            }
            Target* t = webview != nullptr ? find_target(webview->id()) : nullptr;
            if (t == nullptr) {
                if (Connection* conn = find_conn(serial); conn != nullptr) {
                    send_error(*conn, id, -32000, "Failed to create target", {});
                }
                reap();
                return;
            }
            // Reply once the target is announced: clients attach (Target.attachedToTarget) before the reply.
            when_target_ready(*t, [this, serial, id](Target& ready) {
                if (Connection* conn = find_conn(serial); conn != nullptr) {
                    send_result(*conn, id, to_json(cdpjson::TargetIdOnly{.targetId = ready.target_id}), {});
                }
            });
            reap();
        });
    }

    bool add_target_impl(core::WebView& webview, CdpTargetOptions target_options) {
        if (!webview.is_ready() || find_target(webview.id()) != nullptr) {
            return false;
        }
        auto t = std::make_unique<Target>();
        t->id = webview.id();
        t->target_id = target_id_of(t->id);
        t->webview = &webview;
        t->options = std::move(target_options);
        t->cdp = std::make_unique<Cdp>(webview);
        Target& ref = *t;
        targets.push_back(std::move(t));
        // Clients (Playwright, Puppeteer) assume a page's target id equals its main frame id, so the target is
        // announced once Page.getFrameTree has told us that id (the synthetic id stays as a fallback).
        const std::weak_ptr<bool> alive_w = alive;
        const std::uint64_t wid = ref.id;
        const bool started = ref.cdp->call("Page.getFrameTree", "{}", [this, alive_w, wid](bool ok, std::string json) {
            const auto a = alive_w.lock();
            if (!a || !*a) {
                return;
            }
            Target* target = find_target(wid);
            if (target == nullptr || !target->pending) {
                return;
            }
            cdpjson::FrameTreeResult tree;
            if (ok && !glz::read<kLenient>(tree, json) && !tree.frameTree.frame.id.empty() && find_target(tree.frameTree.frame.id) == nullptr) {
                target->target_id = tree.frameTree.frame.id;
            }
            announce(*target);
            reap();
        });
        if (!started) {
            announce(ref);
        }
        return true;
    }

    void announce(Target& ref) {
        ref.pending = false;
        for (const auto& cp : conns) {
            Connection& c = *cp;
            if (c.closed || !c.websocket || c.page_target != 0) {
                continue;
            }
            if (c.discover) {
                send_event(c, "Target.targetCreated", to_json(cdpjson::TargetInfoEnvelope{.targetInfo = target_info(ref)}), {});
            }
            if (c.auto_attach) {
                attach(c, ref);
            }
        }
        const std::uint64_t wid = ref.id;
        auto callbacks = std::move(ref.on_ready);
        ref.on_ready.clear();
        for (auto& fn : callbacks) {
            Target* t = find_target(wid);
            if (t == nullptr) {
                break;
            }
            fn(*t);
        }
    }

    bool remove_target_impl(std::uint64_t id) {
        const auto it = std::ranges::find_if(targets, [id](const auto& t) { return t->id == id; });
        if (it == targets.end()) {
            return false;
        }
        const std::string tid = (*it)->target_id;
        for (const auto& cp : conns) {
            Connection& c = *cp;
            if (c.closed || !c.websocket) {
                continue;
            }
            if (c.page_target == id) {
                send_event(c, "Inspector.detached", R"({"reason":"target_closed"})", {});
                std::string close;
                ws::append_frame(close, ws::Opcode::Close, std::string_view("\x03\xE8", 2));
                c.out += close;
                c.close_after_flush = true;
                flush(c);
                continue;
            }
            for (auto s = c.sessions.begin(); s != c.sessions.end();) {
                if (s->target == id) {
                    send_event(c, "Target.detachedFromTarget", to_json(cdpjson::DetachedFromTarget{.sessionId = s->id, .targetId = tid}), {});
                    s = c.sessions.erase(s);
                } else {
                    ++s;
                }
            }
            if (c.discover) {
                send_event(c, "Target.targetDestroyed", to_json(cdpjson::TargetIdOnly{.targetId = tid}), {});
            }
        }
        targets.erase(it);
        return true;
    }

    // ---- HTTP / WebSocket handshake ----
    void handle_http(Connection& c, std::string_view request) {
        ++stats.http_requests;
        const std::size_t line_end = request.find("\r\n");
        const std::string_view line = request.substr(0, line_end);
        const std::size_t sp1 = line.find(' ');
        const std::size_t sp2 = sp1 == std::string_view::npos ? std::string_view::npos : line.find(' ', sp1 + 1);
        if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
            http_respond(c, "400 Bad Request", "text/plain", "Bad request");
            return;
        }
        const std::string_view verb = line.substr(0, sp1);
        const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);

        std::string_view host;
        std::string_view origin;
        std::string_view upgrade;
        std::string_view connection;
        std::string_view key;
        std::string_view ws_version;
        std::size_t pos = line_end == std::string_view::npos ? request.size() : line_end + 2;
        while (pos < request.size()) {
            const std::size_t end = request.find("\r\n", pos);
            const std::string_view header = request.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
            pos = end == std::string_view::npos ? request.size() : end + 2;
            const std::size_t colon = header.find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            const std::string_view name = header.substr(0, colon);
            std::string_view value = header.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            if (iequals(name, "Host")) {
                host = value;
            } else if (iequals(name, "Origin")) {
                origin = value;
            } else if (iequals(name, "Upgrade")) {
                upgrade = value;
            } else if (iequals(name, "Connection")) {
                connection = value;
            } else if (iequals(name, "Sec-WebSocket-Key")) {
                key = value;
            } else if (iequals(name, "Sec-WebSocket-Version")) {
                ws_version = value;
            }
        }

        // DNS rebinding: only literal loopback host names are accepted.
        const std::string port_suffix = ":" + std::to_string(port);
        if (!(iequals(host, "127.0.0.1" + port_suffix) || iequals(host, "localhost" + port_suffix) || iequals(host, "127.0.0.1") ||
              iequals(host, "localhost"))) {
            http_respond(c, "403 Forbidden", "text/plain", "Host header is not a loopback address");
            return;
        }
        if (!origin.empty() && std::ranges::none_of(options.allowed_origins, [origin](const std::string& o) { return iequals(o, origin); })) {
            http_respond(c, "403 Forbidden", "text/plain", "Rejected an incoming WebSocket/HTTP request from a browser origin");
            return;
        }

        std::string_view path = target;
        std::string_view query;
        if (const std::size_t q = target.find('?'); q != std::string_view::npos) {
            path = target.substr(0, q);
            query = target.substr(q + 1);
        }
        const std::string prefix = "/" + token;
        if (!path.starts_with(prefix) || (path.size() > prefix.size() && path[prefix.size()] != '/')) {
            http_respond(c, "404 Not Found", "text/plain", "Not found");
            return;
        }
        std::string_view rest = path.substr(prefix.size());
        while (rest.size() > 1 && rest.back() == '/') {
            rest.remove_suffix(1);
        }

        if (!upgrade.empty() && iequals(upgrade, "websocket") && contains_token_ci(connection, "upgrade")) {
            std::uint64_t page_target = 0;
            if (rest == "/devtools/browser/" + browser_id) {
                page_target = 0;
            } else if (rest.starts_with("/devtools/page/")) {
                Target* t = find_target(rest.substr(std::string_view("/devtools/page/").size()));
                if (t == nullptr) {
                    http_respond(c, "404 Not Found", "text/plain", "No such target id");
                    return;
                }
                page_target = t->id;
            } else {
                http_respond(c, "404 Not Found", "text/plain", "Not found");
                return;
            }
            if (key.empty() || ws_version != "13") {
                http_respond(c, "426 Upgrade Required", "text/plain", "WebSocket version 13 required");
                return;
            }
            c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
            c.out += ws::accept_key(key);
            c.out += "\r\n\r\n";
            c.websocket = true;
            c.page_target = page_target;
            ++stats.connections;
            if (Target* t = find_target(page_target); t != nullptr) {
                ensure_domain(*t, "Target");
                ensure_domain(*t, "Inspector");
            }
            flush(c);
            return;
        }

        if (verb != "GET" && verb != "PUT") {
            http_respond(c, "405 Method Not Allowed", "text/plain", "Method not allowed");
            return;
        }
        if (rest == "/json/version") {
            const cdpjson::HttpVersion v{
                .Browser = product,
                .protocol_version = "1.3",
                .user_agent = user_agent,
                .v8_version = "",
                .webkit_version = "",
                .webSocketDebuggerUrl = ws_base() + "/devtools/browser/" + browser_id,
            };
            http_respond(c, "200 OK", "application/json; charset=UTF-8", to_json(v));
            return;
        }
        if (rest == "/json" || rest == "/json/list") {
            std::vector<cdpjson::HttpTarget> list;
            for (const auto& t : targets) {
                if (t->pending) {
                    continue;
                }
                const auto info = target_info(*t);
                list.push_back(cdpjson::HttpTarget{
                    .description = "",
                    .devtoolsFrontendUrl = "",
                    .id = info.targetId,
                    .title = info.title,
                    .type = info.type,
                    .url = info.url,
                    .webSocketDebuggerUrl = ws_base() + "/devtools/page/" + info.targetId,
                });
            }
            http_respond(c, "200 OK", "application/json; charset=UTF-8", to_json(list));
            return;
        }
        if (rest == "/json/new") {
            if (!create_fn) {
                http_respond(c, "501 Not Implemented", "text/plain", "Target creation is not supported");
                return;
            }
            const std::weak_ptr<bool> alive_w = alive;
            const std::uint64_t serial = c.serial;
            create_fn(CdpCreateTargetRequest{.url = query.empty() ? std::string("about:blank") : url_decode(query), .browser_context_id = {},
                                             .new_window = false, .background = false},
                      [this, alive_w, serial](core::WebView* webview) {
                          const auto a = alive_w.lock();
                          if (!a || !*a) {
                              return;
                          }
                          if (webview != nullptr && find_target(webview->id()) == nullptr) {
                              add_target_impl(*webview, {});
                          }
                          Target* t = webview != nullptr ? find_target(webview->id()) : nullptr;
                          if (t == nullptr) {
                              if (Connection* conn = find_conn(serial); conn != nullptr) {
                                  http_respond(*conn, "500 Internal Server Error", "text/plain", "Failed to create target");
                              }
                              reap();
                              return;
                          }
                          when_target_ready(*t, [this, serial](Target& ready) {
                              if (Connection* conn = find_conn(serial); conn != nullptr) {
                                  const auto info = target_info(ready);
                                  http_respond(*conn, "200 OK", "application/json; charset=UTF-8",
                                               to_json(cdpjson::HttpTarget{.description = "", .devtoolsFrontendUrl = "", .id = info.targetId,
                                                                           .title = info.title, .type = info.type, .url = info.url,
                                                                           .webSocketDebuggerUrl = ws_base() + "/devtools/page/" + info.targetId}));
                              }
                          });
                          reap();
                      });
            return;
        }
        if (rest.starts_with("/json/close/")) {
            Target* t = find_target(rest.substr(std::string_view("/json/close/").size()));
            const bool ok = t != nullptr && close_fn && close_fn(t->id);
            http_respond(c, ok ? "200 OK" : "404 Not Found", "text/plain", ok ? "Target is closing" : "No such target id");
            return;
        }
        if (rest.starts_with("/json/activate/")) {
            Target* t = find_target(rest.substr(std::string_view("/json/activate/").size()));
            http_respond(c, t != nullptr ? "200 OK" : "404 Not Found", "text/plain", t != nullptr ? "Target activated" : "No such target id");
            return;
        }
        http_respond(c, "404 Not Found", "text/plain", "Not found");
    }

    void process(Connection& c) {
        if (!c.websocket) {
            const std::size_t end = c.in.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (c.in.size() > kMaxHeaderBytes) {
                    http_respond(c, "431 Request Header Fields Too Large", "text/plain", "Headers too large");
                }
                return;
            }
            const std::string request = c.in.substr(0, end + 4);
            c.in.erase(0, end + 4);
            handle_http(c, request);
            if (!c.websocket) {
                c.in.clear();
                return;
            }
        }
        std::size_t offset = 0;
        ws::Frame frame;
        while (!c.closed && !c.close_after_flush) {
            const auto r = ws::parse_frame(std::string_view(c.in).substr(offset), frame, options.max_message_bytes, true);
            if (r.status == ws::ParseStatus::NeedMore) {
                break;
            }
            if (r.status == ws::ParseStatus::Error) {
                close_websocket(c, 1002);
                break;
            }
            offset += r.consumed;
            handle_frame(c, frame);
        }
        c.in.erase(0, offset);
    }

    void close_websocket(Connection& c, std::uint16_t code) {
        const char payload[2] = {static_cast<char>(code >> 8U), static_cast<char>(code & 0xFFU)};
        ws::append_frame(c.out, ws::Opcode::Close, std::string_view(payload, 2));
        c.close_after_flush = true;
        flush(c);
    }

    void handle_frame(Connection& c, ws::Frame& frame) {
        switch (frame.opcode) {
        case ws::Opcode::Text:
        case ws::Opcode::Binary:
            if (c.fragmenting) {
                close_websocket(c, 1002);
                return;
            }
            if (frame.fin) {
                on_message(c, frame.payload);
            } else {
                c.fragment = std::move(frame.payload);
                c.fragmenting = true;
            }
            return;
        case ws::Opcode::Continuation:
            if (!c.fragmenting || c.fragment.size() + frame.payload.size() > options.max_message_bytes) {
                close_websocket(c, 1009);
                return;
            }
            c.fragment += frame.payload;
            if (frame.fin) {
                c.fragmenting = false;
                const std::string message = std::move(c.fragment);
                c.fragment.clear();
                on_message(c, message);
            }
            return;
        case ws::Opcode::Ping:
            ws::append_frame(c.out, ws::Opcode::Pong, frame.payload);
            flush(c);
            return;
        case ws::Opcode::Pong:
            return;
        case ws::Opcode::Close:
            ws::append_frame(c.out, ws::Opcode::Close, frame.payload.size() >= 2 ? std::string_view(frame.payload).substr(0, 2) : std::string_view{});
            c.close_after_flush = true;
            flush(c);
            return;
        }
        close_websocket(c, 1002);
    }

    // ---- sockets ----
    void read_all(Connection& c) {
        char buffer[64 * 1024];
        for (;;) {
            const int n = ::recv(c.socket, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (n > 0) {
                c.in.append(buffer, static_cast<std::size_t>(n));
                stats.bytes_in += static_cast<std::uint64_t>(n);
                if (c.in.size() > options.max_message_bytes + kMaxHeaderBytes) {
                    c.closed = true;
                    return;
                }
                continue;
            }
            if (n == 0) {
                c.closed = true; // orderly shutdown by the client; process what already arrived first
            } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                c.closed = true;
            }
            break;
        }
    }

    void on_socket(SOCKET s, WORD event, WORD error) {
        if (s == listener) {
            if (event == FD_ACCEPT) {
                for (;;) {
                    SOCKET accepted = ::accept(listener, nullptr, nullptr);
                    if (accepted == INVALID_SOCKET) {
                        break;
                    }
                    BOOL no_delay = TRUE;
                    ::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
                    if (WSAAsyncSelect(accepted, hwnd, kSocketMessage, FD_READ | FD_WRITE | FD_CLOSE) != 0) {
                        ::closesocket(accepted);
                        continue;
                    }
                    auto c = std::make_unique<Connection>();
                    c->serial = next_serial++;
                    c->socket = accepted;
                    conns.push_back(std::move(c));
                }
            }
            return;
        }
        Connection* c = nullptr;
        for (const auto& cp : conns) {
            if (cp->socket == s) {
                c = cp.get();
                break;
            }
        }
        if (c == nullptr) {
            return;
        }
        if (error != 0 && event != FD_CLOSE) {
            c->closed = true;
        } else if (event == FD_READ || event == FD_CLOSE) {
            const bool was_closed = c->closed;
            read_all(*c);
            const bool peer_closed = c->closed && !was_closed;
            c->closed = false;
            process(*c);
            if (peer_closed || event == FD_CLOSE) {
                c->closed = true;
            }
        } else if (event == FD_WRITE) {
            flush(*c);
        }
        reap();
    }

    void reap() {
        std::vector<std::uint64_t> touched;
        for (auto it = conns.begin(); it != conns.end();) {
            Connection& c = **it;
            if (!c.closed) {
                ++it;
                continue;
            }
            for (const auto& s : c.sessions) {
                touched.push_back(s.target);
            }
            if (c.page_target != 0) {
                touched.push_back(c.page_target);
            }
            ::closesocket(c.socket);
            it = conns.erase(it);
        }
        for (const std::uint64_t t : touched) {
            release_if_unused(t);
        }
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
        if (msg == kSocketMessage) {
            if (auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); impl != nullptr) {
                impl->on_socket(static_cast<SOCKET>(w_param), WSAGETSELECTEVENT(l_param), WSAGETSELECTERROR(l_param));
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, w_param, l_param);
    }

    bool start() {
        if (hwnd != nullptr) {
            return true;
        }
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
        wsa_started = true;
        static const ATOM atom = []() {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &Impl::wnd_proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"BosonCdpServerWindow";
            return RegisterClassExW(&wc);
        }();
        if (atom == 0) {
            stop();
            return false;
        }
        hwnd = CreateWindowExW(0, L"BosonCdpServerWindow", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (hwnd == nullptr) {
            stop();
            return false;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            stop();
            return false;
        }
        BOOL exclusive = TRUE;
        ::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(options.port);
        if (::bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listener, SOMAXCONN) != 0) {
            stop();
            return false;
        }
        int len = sizeof(addr);
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        if (WSAAsyncSelect(listener, hwnd, kSocketMessage, FD_ACCEPT) != 0) {
            stop();
            return false;
        }
        return true;
    }

    void stop() noexcept {
        for (const auto& c : conns) {
            ::closesocket(c->socket);
        }
        conns.clear();
        if (listener != INVALID_SOCKET) {
            ::closesocket(listener);
            listener = INVALID_SOCKET;
        }
        if (hwnd != nullptr) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        for (const auto& t : targets) {
            t->cdp->unsubscribe_all();
            t->domains.clear();
            t->subs.clear();
        }
        if (wsa_started) {
            WSACleanup();
            wsa_started = false;
        }
        port = 0;
    }
};

CdpServer::CdpServer(CdpServerOptions options) : m_impl(std::make_unique<Impl>(std::move(options))) {
    Impl& s = *m_impl;
    s.token = s.options.token.empty() ? random_hex(16) : s.options.token;
    s.browser_id = random_hex(16);
    s.version = webview2_version();
    s.product = s.options.product.empty() ? "Chrome/" + s.version : s.options.product;
    if (s.options.user_agent.empty()) {
        const std::string major = s.version.substr(0, s.version.find('.'));
        s.user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" + major +
                       ".0.0.0 Safari/537.36 Edg/" + s.version;
    } else {
        s.user_agent = s.options.user_agent;
    }
}

CdpServer::~CdpServer() {
    *m_impl->alive = false;
    m_impl->stop();
}

bool CdpServer::start() {
    return m_impl->start();
}

void CdpServer::stop() noexcept {
    m_impl->stop();
}

bool CdpServer::running() const noexcept {
    return m_impl->hwnd != nullptr && m_impl->listener != INVALID_SOCKET;
}

std::uint16_t CdpServer::port() const noexcept {
    return m_impl->port;
}

const std::string& CdpServer::token() const noexcept {
    return m_impl->token;
}

std::string CdpServer::http_endpoint() const {
    return std::format("http://127.0.0.1:{}/{}", m_impl->port, m_impl->token);
}

std::string CdpServer::browser_websocket_url() const {
    return m_impl->ws_base() + "/devtools/browser/" + m_impl->browser_id;
}

bool CdpServer::add_target(core::WebView& webview, CdpTargetOptions options) {
    const bool ok = m_impl->add_target_impl(webview, std::move(options));
    m_impl->reap();
    return ok;
}

bool CdpServer::remove_target(std::uint64_t webview_id) {
    const bool ok = m_impl->remove_target_impl(webview_id);
    m_impl->reap();
    return ok;
}

void CdpServer::notify_target_changed(std::uint64_t webview_id) {
    Impl& s = *m_impl;
    Impl::Target* t = s.find_target(webview_id);
    if (t == nullptr || t->pending) {
        return;
    }
    std::string params;
    for (const auto& cp : s.conns) {
        if (cp->closed || !cp->websocket || !cp->discover) {
            continue;
        }
        if (params.empty()) {
            params = to_json(cdpjson::TargetInfoEnvelope{.targetInfo = s.target_info(*t)});
        }
        s.send_event(*cp, "Target.targetInfoChanged", params, {});
    }
    s.reap();
}

bool CdpServer::has_target(std::uint64_t webview_id) const noexcept {
    return m_impl->find_target(webview_id) != nullptr;
}

void CdpServer::set_create_target_handler(CreateTargetFn fn) {
    m_impl->create_fn = std::move(fn);
}

void CdpServer::set_close_target_handler(CloseTargetFn fn) {
    m_impl->close_fn = std::move(fn);
}

CdpServerStats CdpServer::stats() const noexcept {
    CdpServerStats out = m_impl->stats;
    out.open_connections = 0;
    for (const auto& c : m_impl->conns) {
        if (c->websocket && !c->closed) {
            ++out.open_connections;
        }
    }
    out.targets = m_impl->targets.size();
    return out;
}

std::string CdpServer::target_id(std::uint64_t webview_id) const {
    const Impl::Target* t = m_impl->find_target(webview_id);
    return t != nullptr && !t->pending ? t->target_id : std::string{};
}

std::string CdpServer::target_id_of(std::uint64_t webview_id) {
    return std::format("B050{:028X}", webview_id);
}

// ---- CdpGuestBridge -----------------------------------------------------------------------------------------

namespace {

/// A CDP browser context id as an in-memory partition name (profile names allow letters and digits).
std::string context_partition(std::string_view context) {
    std::string out;
    for (const char ch : context) {
        if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
            out.push_back(ch);
        }
    }
    return out.substr(0, 60);
}

} // namespace

CdpGuestBridge::CdpGuestBridge(CdpServer& server, GuestManager& manager, CdpGuestBridgeOptions options)
    : m_server(server), m_manager(manager), m_options(std::move(options)) {
    m_listener = m_manager.add_listener([this](Guest& guest, const GuestEvent& event) {
        if (std::holds_alternative<GuestCreated>(event)) {
            expose(guest);
        } else if (std::holds_alternative<GuestDestroyed>(event)) {
            const std::uint64_t id = guest.id();
            for (auto it = m_pending.begin(); it != m_pending.end();) {
                if (it->guest_id == id) {
                    auto done = std::move(it->done);
                    it = m_pending.erase(it);
                    if (done) {
                        done(nullptr);
                    }
                } else {
                    ++it;
                }
            }
            std::erase_if(m_contexts, [id](const auto& p) { return p.first == id; });
            m_server.remove_target(id);
        } else if (std::holds_alternative<GuestTitleUpdated>(event) || std::holds_alternative<GuestDidNavigate>(event) ||
                   std::holds_alternative<GuestDidNavigateInPage>(event)) {
            m_server.notify_target_changed(guest.id());
        }
    });
    m_server.set_create_target_handler([this](const CdpCreateTargetRequest& request, CdpServer::DoneFn done) {
        GuestOptions opts = m_options.defaults;
        opts.src = request.url;
        if (!request.browser_context_id.empty()) {
            opts.partition = context_partition(request.browser_context_id);
        }
        Guest* guest = m_manager.create(m_options.owner_id, std::move(opts), "cdp");
        if (guest == nullptr) {
            done(nullptr);
            return;
        }
        const std::uint64_t id = guest->id();
        if (!request.browser_context_id.empty()) {
            m_contexts.emplace_back(id, request.browser_context_id);
        }
        m_pending.push_back(PendingCreate{.guest_id = id, .context = request.browser_context_id, .done = std::move(done)});
        const std::weak_ptr<bool> alive = m_alive;
        guest->when_ready([this, alive, id]() {
            const auto a = alive.lock();
            if (!a || !*a) {
                return;
            }
            const auto it = std::ranges::find(m_pending, id, &PendingCreate::guest_id);
            if (it == m_pending.end()) {
                return;
            }
            auto done_fn = std::move(it->done);
            const std::string context = it->context;
            m_pending.erase(it);
            Guest* g = m_manager.find(id);
            if (g != nullptr && !m_server.has_target(id)) {
                m_server.add_target(g->webview(), CdpTargetOptions{.type = "page", .browser_context_id = context, .opener_id = g->opener_id()});
            }
            if (done_fn) {
                done_fn(g != nullptr ? &g->webview() : nullptr);
            }
        });
    });
    m_server.set_close_target_handler([this](std::uint64_t id) { return m_manager.destroy(id); });
    if (m_options.expose_existing) {
        for (Guest* g : m_manager.guests()) {
            expose(*g);
        }
    }
}

CdpGuestBridge::~CdpGuestBridge() {
    *m_alive = false;
    m_manager.remove_listener(m_listener);
    m_server.set_create_target_handler({});
    m_server.set_close_target_handler({});
    for (auto& p : m_pending) {
        if (p.done) {
            p.done(nullptr);
        }
    }
}

void CdpGuestBridge::expose(Guest& guest) {
    const std::uint64_t id = guest.id();
    const std::weak_ptr<bool> alive = m_alive;
    guest.when_ready([this, alive, id]() {
        const auto a = alive.lock();
        if (!a || !*a) {
            return;
        }
        if (std::ranges::find(m_pending, id, &PendingCreate::guest_id) != m_pending.end()) {
            return; // Target.createTarget registers it with its browser context
        }
        Guest* g = m_manager.find(id);
        if (g == nullptr || m_server.has_target(id)) {
            return;
        }
        std::string context;
        if (const auto it = std::ranges::find(m_contexts, id, &std::pair<std::uint64_t, std::string>::first); it != m_contexts.end()) {
            context = it->second;
        } else if (g->opener_id() != 0) {
            // Popups inherit their opener's CDP browser context.
            const auto opener = std::ranges::find(m_contexts, g->opener_id(), &std::pair<std::uint64_t, std::string>::first);
            if (opener != m_contexts.end()) {
                context = opener->second;
                m_contexts.emplace_back(id, context);
            }
        }
        m_server.add_target(g->webview(), CdpTargetOptions{.type = "page", .browser_context_id = context, .opener_id = g->opener_id()});
    });
}

} // namespace boson::webview
