#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boson::shipping {

/// @brief WinHTTP client options shared by manifest fetches, update downloads and the WebView2 bootstrapper.
struct HttpOptions {
    std::wstring user_agent = L"Boson/1.0 (Windows)";
    std::chrono::milliseconds resolve_timeout{std::chrono::seconds{15}};
    std::chrono::milliseconds connect_timeout{std::chrono::seconds{15}};
    std::chrono::milliseconds send_timeout{std::chrono::seconds{30}};
    std::chrono::milliseconds receive_timeout{std::chrono::seconds{60}};
    /// Reject plain http:// URLs (after redirects too). Production update channels must be HTTPS.
    bool require_https = true;
    /// Allow http:// for 127.0.0.1 / ::1 / localhost even when require_https is set (local test servers).
    bool allow_insecure_loopback = false;
    /// Allow file:// URLs (local or UNC update shares). Off by default.
    bool allow_file_urls = false;
    /// Extra request headers (name, value).
    std::vector<std::pair<std::wstring, std::wstring>> headers;
    /// http_get() aborts bodies larger than this.
    std::size_t max_body_bytes = 8u * 1024u * 1024u;
};

struct HttpResponse {
    bool ok = false;      ///< Transport succeeded and status is 2xx.
    int status = 0;       ///< HTTP status (200 for a successful file:// read).
    std::string body;
    std::string error;    ///< Human-readable reason when !ok.
    std::uint32_t win32_error = 0;
    std::string etag;
    std::string last_modified;
    std::string content_type;
    std::string final_url; ///< After redirects.
};

/// @brief Blocking GET. Call from a background thread (TaskRunner), never from the UI thread.
/// A stop request aborts the transfer promptly (the WinHTTP request handle is closed).
[[nodiscard]] HttpResponse http_get(std::string_view url, const HttpOptions& options = {}, std::stop_token stop = {});

struct DownloadProgress {
    std::uint64_t received = 0;               ///< Bytes now in the file, including resumed bytes.
    std::optional<std::uint64_t> total;       ///< When the server reported it.
    std::uint64_t resumed_from = 0;           ///< Bytes that were already present when this transfer began.
    double bytes_per_second = 0.0;            ///< Average over this transfer.
    [[nodiscard]] double fraction() const noexcept {
        return total.has_value() && *total > 0 ? static_cast<double>(received) / static_cast<double>(*total) : 0.0;
    }
};

/// Called on the downloading thread, at most every progress_interval plus once at the end.
using DownloadProgressFn = std::function<void(const DownloadProgress&)>;

struct DownloadOptions {
    /// Continue an interrupted download from "<destination>.partial" with Range + If-Range.
    bool resume = true;
    std::chrono::milliseconds progress_interval{100};
    /// When set, a server response of a different total size is rejected.
    std::optional<std::uint64_t> expected_size;
};

struct DownloadResult {
    bool ok = false;
    bool cancelled = false;
    bool resumed = false;      ///< The server honoured the Range request.
    int status = 0;
    std::uint64_t bytes = 0;   ///< Final file size.
    std::string error;
    std::uint32_t win32_error = 0;
};

/// @brief Downloads url to destination (atomically renamed from "<destination>.partial" when complete).
/// An interrupted or cancelled transfer leaves the partial file and "<destination>.partial.json" behind so
/// the next call resumes it. Blocking: call from a background thread.
[[nodiscard]] DownloadResult download_file(
    std::string_view url,
    const std::filesystem::path& destination,
    const HttpOptions& options = {},
    const DownloadProgressFn& on_progress = {},
    std::stop_token stop = {},
    const DownloadOptions& download_options = {});

enum class UrlScheme {
    Invalid,
    Http,
    Https,
    File,
};

struct UrlInfo {
    UrlScheme scheme = UrlScheme::Invalid;
    std::wstring host;
    bool loopback = false;
};

[[nodiscard]] UrlInfo inspect_url(std::string_view url);
/// @brief Empty when options allow url; otherwise the reason it is refused.
[[nodiscard]] std::string check_url_allowed(std::string_view url, const HttpOptions& options);
/// @brief Resolves reference against base (RFC 3986 via UrlCombineW); absolute references are returned as is.
[[nodiscard]] std::string resolve_url(std::string_view base, std::string_view reference);

} // namespace boson::shipping
