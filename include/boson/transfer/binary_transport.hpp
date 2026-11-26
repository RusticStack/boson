#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/ui_timer.hpp>
#include <boson/core/webview.hpp>
#include <boson/exec/task_runner.hpp>
#include <boson/exec/ui_marshal.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::transfer {

enum class TransferMode {
    /// ICoreWebView2Environment12::CreateSharedBuffer + ICoreWebView2_17::PostSharedBufferToScript.
    SharedBuffer,
    /// base64 inside a JSON web message (runtime without shared buffers, or force_base64).
    Base64,
};

struct BinaryTransportOptions {
    /// Idle shared-buffer bytes kept for reuse; buffers released beyond this are closed.
    std::uint64_t max_pool_bytes = 256ULL * 1024 * 1024;
    /// Largest page-to-native upload accepted (shared buffer or base64).
    std::uint64_t max_upload_bytes = 1024ULL * 1024 * 1024;
    /// A buffer posted to script that is not acknowledged within this time is closed instead of reused.
    std::chrono::milliseconds ack_timeout{std::chrono::seconds{60}};
    /// Always use the base64 fallback (benchmarks, diagnostics).
    bool force_base64 = false;
};

struct BinaryAttachOptions {
    /// Origins allowed to use the transport. Empty = boson://app plus the origin of the WebView's dev_url.
    std::vector<std::string> allowed_origins;
    /// Adds page_script() as a document-created script and runs it in the current document (WebView must be
    /// ready). To survive Environment::recreate, also put page_script() in WebViewConfig::scripts_on_document_created.
    bool inject_page_runtime = true;
};

/// @brief A page-to-native binary payload. data is valid only for the duration of the handler call.
struct BinaryUpload {
    std::uint64_t webview_id = 0;
    std::string channel;
    std::string metadata_json = "null";
    std::span<const std::byte> data;
    /// true when data points straight into the shared buffer the page wrote (no copy, no decode).
    bool zero_copy = false;
};

/// @brief A page request for binary data (transfer.ts requestBinary).
struct BinaryRequest {
    std::uint64_t webview_id = 0;
    std::string channel;
    std::string params_json = "null";
};

/// @brief The answer to a BinaryRequest. Either data, or size + fill (written straight into the shared buffer).
struct BinaryReply {
    std::vector<std::byte> data;
    std::string metadata_json = "null";
    std::uint64_t size = 0;
    std::function<void(std::span<std::byte> out)> fill;
};

struct TransferStats {
    std::uint64_t buffers_created = 0;
    std::uint64_t buffers_reused = 0;
    std::uint64_t buffers_closed = 0;
    std::uint64_t buffers_in_flight = 0; ///< posted to script, not yet acknowledged
    std::uint64_t buffers_leased = 0;    ///< lent to the page for an upload, or being read by an upload handler
    std::uint64_t idle_buffers = 0;
    std::uint64_t idle_bytes = 0;
    std::uint64_t shared_sends = 0;
    std::uint64_t base64_sends = 0;
    std::uint64_t uploads = 0;
    std::uint64_t ack_timeouts = 0;
};

/// @brief Binary IPC between C++ and pages, next to the JSON Bridge.
///
/// Native to page: send_binary() copies once into a pooled ICoreWebView2SharedBuffer and posts it read-only;
/// the page (transfer.ts onBinary) gets an ArrayBuffer mapped onto the same memory, releases it and acks, and
/// the buffer goes back to the pool. Page to native: transfer.ts upload() asks for a read-write buffer, writes
/// into it and commits; the upload handler reads the shared memory in place on a pool thread. Pages can also
/// request binary data (requestBinary) from a handler. Without shared-buffer support everything falls back to
/// base64 inside JSON web messages with the same API on both sides.
///
/// Construct, attach and detach on the UI thread. send_binary / post_binary may be called from any thread.
class BinaryTransport {
public:
    using UploadHandler = std::function<std::string(const BinaryUpload& upload)>;
    using RequestHandler = std::function<BinaryReply(const BinaryRequest& request)>;

    BinaryTransport(exec::TaskRunner& runner, exec::UiMarshal& marshal, BinaryTransportOptions options = {});
    ~BinaryTransport();

    BinaryTransport(const BinaryTransport&) = delete;
    BinaryTransport& operator=(const BinaryTransport&) = delete;
    BinaryTransport(BinaryTransport&&) = delete;
    BinaryTransport& operator=(BinaryTransport&&) = delete;

    /// @brief Routes webview's transfer messages here. The WebView must outlive the transport or be detached.
    void attach(core::WebView& webview, BinaryAttachOptions options = {});
    void detach(core::WebView& webview) noexcept;
    [[nodiscard]] bool is_attached(std::uint64_t webview_id) const;
    /// @brief Mode used for webview_id right now (Base64 when not attached or not supported).
    [[nodiscard]] TransferMode mode(std::uint64_t webview_id) const;
    /// @brief true when this WebView's runtime exposes CreateSharedBuffer and PostSharedBufferToScript.
    [[nodiscard]] static bool supports_shared_buffers(const core::WebView& webview);

    /// @brief Handler for transfer.ts upload(channel, ...). Runs on a pool thread; returns the JSON value the
    /// page's Promise resolves with ("null" for nothing). A thrown exception rejects the Promise.
    void on_upload(std::string channel, UploadHandler handler);
    /// @brief Handler for transfer.ts requestBinary(channel, params). Runs on a pool thread.
    void on_request(std::string channel, RequestHandler handler);
    void remove_handler(std::string_view channel);

    /// @brief Copies data once (shared buffer) or encodes it (fallback) on the calling thread and posts it to
    /// the page's onBinary(channel) listeners. From a non-UI thread this may block briefly on the UI thread
    /// when a new shared buffer has to be created. Returns false when not attached or allocation failed.
    bool send_binary(
        std::uint64_t webview_id,
        std::string_view channel,
        std::span<const std::byte> data,
        std::string_view metadata_json = "null");

    /// @brief Like send_binary, but fill writes the payload straight into the shared buffer (true zero-copy).
    bool send_binary_with(
        std::uint64_t webview_id,
        std::string_view channel,
        std::uint64_t size,
        const std::function<void(std::span<std::byte> out)>& fill,
        std::string_view metadata_json = "null");

    /// @brief Takes ownership of data and does the copy on a TaskRunner pool thread; never blocks the caller.
    void post_binary(
        std::uint64_t webview_id,
        std::string channel,
        std::vector<std::byte> data,
        std::string metadata_json = "null");

    /// @brief Closes idle pooled buffers (and in-flight ones past ack_timeout). UI thread.
    void trim();
    [[nodiscard]] TransferStats stats() const;
    [[nodiscard]] const BinaryTransportOptions& options() const noexcept { return m_options; }
    void set_force_base64(bool force) noexcept;

    /// @brief The page runtime (window.__bosonTransfer) that transfer.ts wraps. Idempotent.
    [[nodiscard]] static std::wstring page_script();

    /// @brief Pool size class for a payload: powers of two from 64 KiB to 16 MiB, then multiples of 4 MiB.
    [[nodiscard]] static std::uint64_t bucket_size(std::uint64_t bytes) noexcept;

    struct State;

private:
    BinaryTransportOptions m_options;
    std::shared_ptr<State> m_state;
    core::UiTimer m_timer;
};

} // namespace boson::transfer
