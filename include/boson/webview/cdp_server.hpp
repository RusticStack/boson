#pragma once

#include <windows.h>

#include <boson/core/webview.hpp>
#include <boson/webview/cdp.hpp>
#include <boson/webview/guest.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace boson::webview {

/// @brief RFC 6455 pieces used by CdpServer (exposed for tests).
namespace ws {

enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct Frame {
    bool fin = true;
    Opcode opcode = Opcode::Text;
    std::string payload; ///< Unmasked.
};

enum class ParseStatus {
    Complete, ///< out holds one frame; consumed bytes were used.
    NeedMore, ///< data holds an incomplete frame.
    Error,    ///< Protocol violation (unmasked client frame, oversized payload, reserved bits).
};

struct ParseResult {
    ParseStatus status = ParseStatus::NeedMore;
    std::size_t consumed = 0;
};

/// @brief Parses one frame from the start of data.
[[nodiscard]] ParseResult parse_frame(std::string_view data, Frame& out, std::size_t max_payload, bool require_mask);
/// @brief Appends one unmasked (server-to-client) frame. mask_key != 0 produces a masked (client) frame.
void append_frame(std::string& out, Opcode opcode, std::string_view payload, bool fin = true, std::uint32_t mask_key = 0);
/// @brief Sec-WebSocket-Accept for a Sec-WebSocket-Key: base64(SHA-1(key + GUID)).
[[nodiscard]] std::string accept_key(std::string_view client_key);

} // namespace ws

struct CdpServerOptions {
    std::uint16_t port = 0;               ///< 0 = a random free port. Always bound to 127.0.0.1 only.
    std::string token;                    ///< Path secret; empty = 32 random hex characters.
    std::string product;                  ///< Browser.getVersion product; empty = "Chrome/<WebView2 version>".
    std::string user_agent;               ///< Empty = a Chrome-style UA with the WebView2 version.
    std::size_t max_message_bytes = 256ULL * 1024 * 1024;
    /// Origins allowed to open the WebSocket (browsers send Origin; Playwright and other CLIs do not).
    /// A request carrying an Origin header that is not listed is refused with 403.
    std::vector<std::string> allowed_origins;
};

struct CdpTargetOptions {
    std::string type = "page";
    std::string browser_context_id;       ///< Empty = the default context.
    std::uint64_t opener_id = 0;          ///< WebView::id() of the opener (popups).
};

struct CdpCreateTargetRequest {
    std::string url;
    std::string browser_context_id;
    bool new_window = false;
    bool background = false;
};

struct CdpServerStats {
    std::uint64_t connections = 0;        ///< Accepted WebSocket connections (total).
    std::uint64_t http_requests = 0;
    std::uint64_t messages_in = 0;
    std::uint64_t messages_out = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t commands_forwarded = 0; ///< Sent to WebView2 (CallDevToolsProtocolMethod[ForSession]).
    std::uint64_t events_forwarded = 0;
    std::uint64_t open_connections = 0;
    std::uint64_t targets = 0;
};

/// @brief Local Chrome DevTools Protocol endpoint for Boson WebViews (chromium.connectOverCDP, Puppeteer,
/// chrome-remote-interface).
///
/// Listens on 127.0.0.1 only, on a random port, with a random token in every path:
///   GET  /<token>/json/version, /<token>/json[/list], /<token>/json/new?<url>, /<token>/json/close/<id>
///   WS   /<token>/devtools/browser/<id>   browser endpoint: Target.* / Browser.* + flattened sessions
///   WS   /<token>/devtools/page/<targetId> one page, no sessions
/// http_endpoint() is what Playwright's connectOverCDP takes. Page sessions are relayed to WebView2's
/// CallDevToolsProtocolMethod (ForSession for child sessions); events are subscribed per domain when a
/// client first uses it and released when the last client of a target disconnects (with Network / Fetch /
/// Debugger / ... .disable), so an idle server holds no CDP receivers.
///
/// Everything runs on the UI thread: sockets use WSAAsyncSelect on a message-only window (no polling, no
/// threads, no blocking calls). All members must be called on the UI thread.
class CdpServer {
public:
    using DoneFn = std::function<void(core::WebView* webview)>;
    /// Target.createTarget / json/new. Call done with the new, ready WebView (or null on failure).
    using CreateTargetFn = std::function<void(const CdpCreateTargetRequest& request, DoneFn done)>;
    /// Target.closeTarget / Page.close / json/close. Return false when the target cannot be closed.
    using CloseTargetFn = std::function<bool(std::uint64_t webview_id)>;

    explicit CdpServer(CdpServerOptions options = {});
    ~CdpServer();

    CdpServer(const CdpServer&) = delete;
    CdpServer& operator=(const CdpServer&) = delete;
    CdpServer(CdpServer&&) = delete;
    CdpServer& operator=(CdpServer&&) = delete;

    /// @brief Binds 127.0.0.1:<port> and starts accepting. false when the socket could not be opened.
    bool start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] const std::string& token() const noexcept;
    /// @brief "http://127.0.0.1:<port>/<token>" (chromium.connectOverCDP endpointURL).
    [[nodiscard]] std::string http_endpoint() const;
    /// @brief "ws://127.0.0.1:<port>/<token>/devtools/browser/<id>".
    [[nodiscard]] std::string browser_websocket_url() const;

    /// @brief Exposes a ready WebView as a page target. false when already registered or not ready.
    bool add_target(core::WebView& webview, CdpTargetOptions options = {});
    /// @brief Detaches every session of the target (Target.detachedFromTarget) and forgets it. Call before
    /// the WebView is destroyed.
    bool remove_target(std::uint64_t webview_id);
    /// @brief Sends Target.targetInfoChanged (title / url) to clients that called Target.setDiscoverTargets.
    void notify_target_changed(std::uint64_t webview_id);
    [[nodiscard]] bool has_target(std::uint64_t webview_id) const noexcept;

    void set_create_target_handler(CreateTargetFn fn);
    void set_close_target_handler(CloseTargetFn fn);

    [[nodiscard]] CdpServerStats stats() const noexcept;

    /// @brief CDP targetId of a WebView (32 upper-case hex digits).
    /// @brief The CDP target id of a registered WebView: its main frame id (as in Chrome), known shortly after
    /// add_target; empty until then or when the WebView is not a target.
    [[nodiscard]] std::string target_id(std::uint64_t webview_id) const;
    /// @brief The fallback id used when the main frame id cannot be read (32 hex digits).
    [[nodiscard]] static std::string target_id_of(std::uint64_t webview_id);

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

struct CdpGuestBridgeOptions {
    /// Owner of guests created through Target.createTarget (usually the host page's WebView::id(); the host
    /// page receives guest:created and can adopt them with <boson-webview guestinstance=id>).
    std::uint64_t owner_id = 0;
    /// Options for guests created through Target.createTarget (src and partition are set from the request).
    GuestOptions defaults;
    /// Register guests that already exist when the bridge is created.
    bool expose_existing = true;
};

/// @brief Connects a GuestManager to a CdpServer: every guest becomes a page target once ready (and is
/// removed when destroyed), popups carry openerId, Target.createTarget creates guests (browser contexts map
/// to in-memory partitions) and Target.closeTarget destroys them. UI thread.
class CdpGuestBridge {
public:
    CdpGuestBridge(CdpServer& server, GuestManager& manager, CdpGuestBridgeOptions options = {});
    ~CdpGuestBridge();

    CdpGuestBridge(const CdpGuestBridge&) = delete;
    CdpGuestBridge& operator=(const CdpGuestBridge&) = delete;
    CdpGuestBridge(CdpGuestBridge&&) = delete;
    CdpGuestBridge& operator=(CdpGuestBridge&&) = delete;

private:
    void expose(Guest& guest);

    CdpServer& m_server;
    GuestManager& m_manager;
    CdpGuestBridgeOptions m_options;
    GuestManager::ListenerId m_listener = 0;
    struct PendingCreate {
        std::uint64_t guest_id = 0;
        std::string context;
        CdpServer::DoneFn done;
    };
    std::vector<PendingCreate> m_pending;
    std::vector<std::pair<std::uint64_t, std::string>> m_contexts; ///< guest id -> CDP browser context
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace boson::webview
