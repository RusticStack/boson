#pragma once

#include <windows.h>
#include <objidl.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>
#include <boson/exec/task_runner.hpp>
#include <boson/exec/ui_marshal.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::transfer {

/// @brief Parsed "Range: bytes=..." header for a resource of a known size.
struct ByteRange {
    std::uint64_t first = 0; ///< inclusive
    std::uint64_t last = 0;  ///< inclusive
    [[nodiscard]] std::uint64_t length() const noexcept { return last - first + 1; }
    bool operator==(const ByteRange&) const = default;
};

enum class RangeKind {
    None,          ///< no Range header, or one we serve as a full 200 (multi-range, other units, malformed)
    Satisfiable,   ///< serve 206 with range
    Unsatisfiable, ///< serve 416
};

struct RangeResult {
    RangeKind kind = RangeKind::None;
    ByteRange range{};
};

/// @brief A generated (non-file) resource. read(offset, out) fills out from offset and returns the bytes
/// written; 0 means end of stream. It runs on a WebView2 network thread, so it must be thread-safe.
struct GeneratedResource {
    std::string mime_type = "application/octet-stream";
    /// Total size when known: enables Range / 206 and Content-Length.
    std::optional<std::uint64_t> size;
    std::function<std::size_t(std::uint64_t offset, std::span<std::byte> out)> read;
    std::vector<std::pair<std::string, std::string>> extra_headers;
};

struct StreamRequest {
    std::string method = "GET";
    std::string uri;
    /// Path after the mount prefix, still percent-encoded, without query or fragment ("/a/b.mp4").
    std::string path;
    std::string query;
    std::optional<std::string> range_header;
};

/// @brief Returns std::nullopt for 404.
using GeneratorFn = std::function<std::optional<GeneratedResource>(const StreamRequest& request)>;

struct StreamServerOptions {
    /// URL host served: boson://<host>/<mount>/...  ("app" is reserved for the embedded assets).
    std::string host = "media";
    /// Caps the answer to an open-ended range ("bytes=N-") at this many bytes; 0 = no cap (default). Leave it at
    /// 0 for <video>/<audio>: Chromium's media loader does not honour Content-Range for non-HTTP schemes, so a
    /// capped first response makes it treat the resource as that short (measured: a 60 s WAV reported 11.9 s).
    /// Useful for fetch()-based readers that page through a resource themselves.
    std::uint64_t max_open_range_bytes = 0;
    /// Value for Access-Control-Allow-Origin; empty omits the header.
    std::string allow_origin = "*";
    std::string cache_control = "no-cache";
};

/// @brief One HTTP-like response produced by StreamServer::resolve (headless-testable).
struct StreamResponse {
    int status = 404;
    std::string reason = "Not Found";
    std::vector<std::pair<std::string, std::string>> headers;
    /// Body (nullptr for HEAD, errors and empty bodies). Agile (free-threaded) IStream.
    Microsoft::WRL::ComPtr<IStream> body;

    [[nodiscard]] std::optional<std::string> header(std::string_view name) const;
    [[nodiscard]] std::string header_block() const; ///< "Name: value\r\n..." without the trailing CRLF
};

/// @brief Serves files from mapped directories and generated streams over boson://<host>/..., with HTTP
/// Range support (206 / 416, Content-Range, Accept-Ranges) for <video>/<audio> seeking. Files are streamed from
/// an open handle by an IStream that reads on demand; nothing is loaded whole into memory. Requests are
/// resolved on a TaskRunner pool thread (WebResourceRequested deferral), so the UI thread never touches disk.
class StreamServer {
public:
    StreamServer(exec::TaskRunner& runner, exec::UiMarshal& marshal, StreamServerOptions options = {});
    ~StreamServer();

    StreamServer(const StreamServer&) = delete;
    StreamServer& operator=(const StreamServer&) = delete;
    StreamServer(StreamServer&&) = delete;
    StreamServer& operator=(StreamServer&&) = delete;

    /// @brief Maps boson://<host><prefix>/... onto root. Returns false when root is not an existing directory.
    /// Any thread.
    bool mount_directory(std::string_view prefix, const std::filesystem::path& root);
    /// @brief Serves boson://<host><prefix>/... from generator. Any thread.
    void mount_generator(std::string_view prefix, GeneratorFn generator);
    bool unmount(std::string_view prefix);

    /// @brief Adds the boson://<host>/* filter and handler to webview (UI thread).
    void attach(core::WebView& webview);
    void detach(core::WebView& webview) noexcept;

    /// @brief "boson://<host>"; append a mount prefix and path.
    [[nodiscard]] std::string base_url() const;
    [[nodiscard]] const StreamServerOptions& options() const noexcept { return m_options; }

    /// @brief Routing, path checks, Range handling and stream creation, without WebView2. Any thread.
    [[nodiscard]] StreamResponse resolve(const StreamRequest& request) const;

    // ---- pure helpers (unit-tested) -----------------------------------------------------------------------

    [[nodiscard]] static RangeResult parse_range(std::string_view header, std::uint64_t size, std::uint64_t max_open_range = 0);
    /// @brief Decodes %XX; std::nullopt for malformed escapes or an encoded NUL.
    [[nodiscard]] static std::optional<std::string> percent_decode(std::string_view text);
    /// @brief Maps an encoded URL path under root to a file path, rejecting "..", drive letters, ':' (ADS),
    /// backslashes, reserved device names, trailing dots/spaces and control characters. Lexical only; the
    /// opened handle is checked again with GetFinalPathNameByHandleW (symlink / junction escapes).
    [[nodiscard]] static std::optional<std::filesystem::path> safe_join(const std::filesystem::path& root, std::string_view encoded_path);
    /// @brief MIME type for media and web types (falls back to ProtocolHandler::mime_type_for).
    [[nodiscard]] static std::string_view mime_type_for(std::string_view path) noexcept;
    /// @brief true when candidate is root or inside it (case-insensitive, both final paths).
    [[nodiscard]] static bool is_within(std::wstring_view root_final, std::wstring_view candidate_final) noexcept;

    /// @brief IStream reading [offset, offset + length) of an open file on demand; takes ownership of file.
    [[nodiscard]] static Microsoft::WRL::ComPtr<IStream> make_file_stream(HANDLE file, std::uint64_t offset, std::uint64_t length);
    /// @brief IStream pulling from read starting at offset; length = std::nullopt reads until read returns 0.
    [[nodiscard]] static Microsoft::WRL::ComPtr<IStream> make_generator_stream(
        std::function<std::size_t(std::uint64_t, std::span<std::byte>)> read,
        std::uint64_t offset,
        std::optional<std::uint64_t> length);

    struct Mount;

private:
    struct Attachment {
        core::WebView* webview = nullptr;
        std::uint64_t id = 0;
        core::WebView::HookId hook = 0;
    };

    bool handle(core::WebView& webview, std::string_view uri, ICoreWebView2WebResourceRequestedEventArgs* args);

    exec::TaskRunner& m_runner;
    exec::UiMarshal& m_marshal;
    StreamServerOptions m_options;
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<const Mount>> m_mounts; // longest prefix first
    std::vector<Attachment> m_attachments;              // UI thread
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace boson::transfer
