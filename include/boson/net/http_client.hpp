#pragma once

#include <boson/crypto/hash.hpp>
#include <boson/net/http_cache.hpp>
#include <boson/net/proxy.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

namespace boson::net {

enum class HttpMethod : std::uint8_t {
    Get,
    Head,
    Post,
    Put,
    Patch,
    Delete,
    Options,
};

[[nodiscard]] std::string_view to_string(HttpMethod method) noexcept;
/// @brief Case-insensitive "GET", "post", ... std::nullopt for anything else.
[[nodiscard]] std::optional<HttpMethod> parse_http_method(std::string_view text) noexcept;

struct HttpHeader {
    std::string name;
    std::string value;
};
using HttpHeaders = std::vector<HttpHeader>;

/// @brief First header named name (ASCII case-insensitive).
[[nodiscard]] std::optional<std::string_view> find_header(const HttpHeaders& headers, std::string_view name) noexcept;

/// @brief Retries with exponential backoff. A retry happens only before any body byte reached a
/// streaming sink (send() and download() can always restart; stream() cannot once data was delivered).
struct RetryPolicy {
    std::uint32_t max_attempts = 3; ///< total attempts, 1 = no retry
    std::chrono::milliseconds initial_backoff{250};
    std::chrono::milliseconds max_backoff{8000};
    double multiplier = 2.0;
    bool retry_server_errors = true;    ///< 5xx and 429
    bool retry_transport_errors = true; ///< connect / reset / per-phase timeouts
    bool retry_non_idempotent = false;  ///< POST / PATCH as well
    bool honor_retry_after = true;      ///< Retry-After (seconds) on 429 / 503, capped by max_retry_after
    std::chrono::milliseconds max_retry_after{30'000};

    [[nodiscard]] static RetryPolicy none() noexcept {
        RetryPolicy p;
        p.max_attempts = 1;
        return p;
    }
};

struct HttpClientOptions {
    std::wstring user_agent = L"Boson/1.0 (Windows)";
    std::chrono::milliseconds resolve_timeout{std::chrono::seconds{15}};
    std::chrono::milliseconds connect_timeout{std::chrono::seconds{15}};
    std::chrono::milliseconds send_timeout{std::chrono::seconds{30}};
    std::chrono::milliseconds receive_timeout{std::chrono::seconds{60}}; ///< per read, not per body
    bool enable_http2 = true;  ///< negotiated over TLS (ALPN); plain http:// stays HTTP/1.1
    bool enable_http3 = false; ///< Windows 11 22H2+ only; ignored where unsupported
    bool follow_redirects = true;
    std::uint32_t max_redirects = 10;
    /// Accept-Encoding gzip, deflate with transparent WinHTTP decoding. Size limits apply to decoded bytes.
    bool decompress = true;
    /// Refuse http:// (initial URL and redirects) ...
    bool require_https = false;
    /// ... except for 127.0.0.0/8, ::1 and localhost.
    bool allow_insecure_loopback = true;
    /// https -> http redirects are refused unless this is set.
    bool allow_https_downgrade = false;
    ProxySettings proxy;
    RetryPolicy retry;
    /// LRU response cache for GET (std::nullopt = no cache).
    std::optional<HttpCacheOptions> cache;
    /// send(): bodies larger than this fail with TooLarge. stream() / download(): 0 = unlimited.
    std::size_t max_body_bytes = 64u * 1024u * 1024u;
    std::uint64_t max_download_bytes = 0;
    /// WINHTTP_OPTION_MAX_CONNS_PER_SERVER (0 = WinHTTP default).
    std::uint32_t max_connections_per_server = 0;
    HttpHeaders default_headers;
    /// Checked for the initial URL and every redirect hop; return false to refuse it (e.g. a host allow-list).
    std::function<bool(std::string_view url)> url_filter;
};

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string url;
    HttpHeaders headers;
    std::string body;
    /// Deadline for the whole call (redirects, retries and the body). Enforced with a thread-pool timer
    /// armed only for this request.
    std::optional<std::chrono::milliseconds> timeout;
    std::optional<std::uint64_t> max_body_bytes; ///< overrides the client limit
    std::optional<RetryPolicy> retry;            ///< overrides the client policy
    bool use_cache = true;
    std::stop_token stop;
};

struct HttpResponse {
    int status = 0;
    HttpHeaders headers;
    std::string body;
    std::string final_url;
    std::string protocol;      ///< "HTTP/1.1", "HTTP/2", "HTTP/3"
    std::optional<std::uint64_t> content_length; ///< as sent (compressed size when decoding)
    std::uint32_t redirects = 0;
    std::uint32_t attempts = 0;
    bool from_cache = false;

    [[nodiscard]] bool ok() const noexcept { return status >= 200 && status < 300; }
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept { return find_header(headers, name); }
};

enum class HttpErrorKind : std::uint8_t {
    InvalidUrl,
    Refused,          ///< scheme / https policy / url_filter rejected the URL or a redirect
    Connect,          ///< name resolution or TCP connect failed
    Tls,              ///< certificate / handshake failure
    Transport,        ///< connection reset, protocol error
    Timeout,
    Cancelled,
    TooLarge,
    TooManyRedirects,
    Status,           ///< stream() / download() got a non-2xx final status (response in HttpError::status)
    Io,               ///< destination file could not be written
    HashMismatch,
    Aborted,          ///< the body sink returned false
};

[[nodiscard]] std::string_view to_string(HttpErrorKind kind) noexcept;

struct HttpError {
    HttpErrorKind kind = HttpErrorKind::Transport;
    std::string message;
    std::uint32_t win32_error = 0;
    int status = 0;
    std::uint32_t attempts = 0;
    std::string body; ///< first bytes (<= 64 KiB) of a non-2xx body for Status errors
};

template <typename T>
using HttpResult = std::expected<T, HttpError>;

struct TransferProgress {
    std::uint64_t received = 0;
    std::optional<std::uint64_t> total;
    double bytes_per_second = 0.0;
    [[nodiscard]] double fraction() const noexcept {
        return total && *total > 0 ? static_cast<double>(received) / static_cast<double>(*total) : 0.0;
    }
};

using ProgressFn = std::function<void(const TransferProgress&)>;
/// Receives the body in order; return false to abort (HttpErrorKind::Aborted). Called on the request thread.
using BodySink = std::function<bool(std::span<const std::byte>)>;

struct StreamOptions {
    ProgressFn on_progress;
    std::chrono::milliseconds progress_interval{100};
};

struct DownloadOptions {
    ProgressFn on_progress;
    std::chrono::milliseconds progress_interval{100};
    /// Hash the bytes while they are written (no second pass over the file).
    std::optional<crypto::HashAlgorithm> hash;
    /// When set (hex), a mismatch deletes the file and fails with HashMismatch. Implies hash (algorithm
    /// guessed from the length when hash is unset: 32 = MD5, 40 = SHA-1, 64 = SHA-256).
    std::string expected_hash;
    bool overwrite = true;
};

struct DownloadResult {
    HttpResponse response; ///< body empty
    std::uint64_t bytes = 0;
    std::filesystem::path path;
    std::optional<crypto::Digest> digest;
};

struct HttpClientStats {
    std::uint64_t requests = 0;   ///< WinHTTP requests sent (attempts and redirect hops)
    std::uint64_t retries = 0;
    std::uint64_t redirects = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t sessions_opened = 0;
};

namespace detail {

struct StopForwarder {
    std::stop_source* source;
    void operator()() const noexcept { source->request_stop(); }
};

/// Runs fn(std::stop_token) with a token that fires when either the sender environment's stop token or the
/// request's own std::stop_token is triggered.
template <typename EnvToken, typename Fn>
decltype(auto) with_combined_stop(const EnvToken& env_token, const std::stop_token& own, Fn&& fn) {
    std::stop_source source;
    const std::stop_callback own_cb(own, StopForwarder{&source});
    if constexpr (stdexec::unstoppable_token<EnvToken>) {
        return fn(source.get_token());
    } else {
        const stdexec::stop_callback_for_t<EnvToken, StopForwarder> env_cb(env_token, StopForwarder{&source});
        return fn(source.get_token());
    }
}

} // namespace detail

/// @brief WinHTTP client. One lazily opened WinHTTP session per client (opened on the first request, so an
/// idle client costs nothing) gives keep-alive connection reuse across requests and threads. Requests are
/// blocking calls that wait in the kernel (no polling); run them on a pool thread, or use the senders
/// (send_on / download_on), which honour the receiver's stop token. Thread-safe.
class HttpClient {
public:
    explicit HttpClient(HttpClientOptions options = {});
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    /// @brief Whole response in memory (bounded by max_body_bytes). Non-2xx statuses are not errors here.
    [[nodiscard]] HttpResult<HttpResponse> send(HttpRequest request);
    /// @brief 2xx body to sink as it arrives; response.body stays empty. Non-2xx -> HttpErrorKind::Status.
    [[nodiscard]] HttpResult<HttpResponse> stream(HttpRequest request, const BodySink& sink, const StreamOptions& options = {});
    /// @brief Streams the body to "<destination>.partial", verifies the optional hash, then renames it to
    /// destination (MoveFileExW, replace when overwrite). Failure or cancellation deletes the partial file.
    [[nodiscard]] HttpResult<DownloadResult> download(
        HttpRequest request, const std::filesystem::path& destination, const DownloadOptions& options = {});

    [[nodiscard]] HttpResult<HttpResponse> get(std::string url, HttpHeaders headers = {});
    [[nodiscard]] HttpResult<HttpResponse> head(std::string url, HttpHeaders headers = {});
    [[nodiscard]] HttpResult<HttpResponse> post(std::string url, std::string body, std::string content_type = "application/json");

    /// @brief Sender: send(request) on sched. Completes with HttpResult<HttpResponse>; a stop request from
    /// the receiver's environment cancels the transfer (closes the WinHTTP handle).
    template <stdexec::scheduler Sched>
    [[nodiscard]] auto send_on(Sched sched, HttpRequest request) {
        return stdexec::starts_on(
            std::move(sched),
            stdexec::read_env(stdexec::get_stop_token) |
                stdexec::then([this, request = std::move(request)](auto token) mutable {
                    return detail::with_combined_stop(token, request.stop, [&](std::stop_token combined) {
                        request.stop = std::move(combined);
                        return send(std::move(request));
                    });
                }));
    }

    template <stdexec::scheduler Sched>
    [[nodiscard]] auto download_on(Sched sched, HttpRequest request, std::filesystem::path destination, DownloadOptions options = {}) {
        return stdexec::starts_on(
            std::move(sched),
            stdexec::read_env(stdexec::get_stop_token) |
                stdexec::then([this, request = std::move(request), destination = std::move(destination),
                               options = std::move(options)](auto token) mutable {
                    return detail::with_combined_stop(token, request.stop, [&](std::stop_token combined) {
                        request.stop = std::move(combined);
                        return download(std::move(request), destination, options);
                    });
                }));
    }

    /// @brief The response cache (nullptr when HttpClientOptions::cache is unset).
    [[nodiscard]] HttpCache* cache() noexcept;
    [[nodiscard]] const HttpClientOptions& options() const noexcept;
    [[nodiscard]] HttpClientStats stats() const noexcept;
    /// @brief Whether the WinHTTP session has been opened yet (false until the first request).
    [[nodiscard]] bool session_open() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// @brief Validates the scheme / https policy for url ("" = allowed, else the reason).
[[nodiscard]] std::string check_url(std::string_view url, const HttpClientOptions& options);
/// @brief Lower-cased host of an http(s) URL ("" when malformed).
[[nodiscard]] std::string url_host(std::string_view url);
/// @brief true for 127.0.0.0/8, ::1 and localhost.
[[nodiscard]] bool is_loopback_host(std::string_view host) noexcept;

} // namespace boson::net
