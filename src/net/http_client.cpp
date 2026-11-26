#include <boson/net/http_client.hpp>

#include <boson/core/result.hpp>

#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <mutex>

#ifndef WINHTTP_OPTION_HTTP_PROTOCOL_USED
#define WINHTTP_OPTION_HTTP_PROTOCOL_USED 134
#endif
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif

namespace boson::net {

namespace {

struct InternetCloser {
    void operator()(HINTERNET handle) const noexcept {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
};
using UniqueInternet = std::unique_ptr<void, InternetCloser>;

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

DWORD clamp_ms(std::chrono::milliseconds ms) noexcept {
    return ms.count() <= 0 ? 0 : static_cast<DWORD>(std::min<long long>(ms.count(), 0x7FFFFFFF));
}

std::string win32_text(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_HMODULE,
        GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string out = "error " + std::to_string(code);
    if (len > 0 && buffer != nullptr) {
        std::wstring_view text{buffer, len};
        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ' || text.back() == L'.')) {
            text.remove_suffix(1);
        }
        out += ": " + core::wide_to_utf8(text);
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return out;
}

HttpErrorKind classify(DWORD code) noexcept {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT:
        return HttpErrorKind::Timeout;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return HttpErrorKind::Connect;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        return HttpErrorKind::Tls;
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
    case ERROR_WINHTTP_INVALID_URL:
        return HttpErrorKind::InvalidUrl;
    default:
        return HttpErrorKind::Transport;
    }
}

bool retryable(HttpErrorKind kind) noexcept {
    return kind == HttpErrorKind::Connect || kind == HttpErrorKind::Transport || kind == HttpErrorKind::Timeout;
}

bool idempotent(HttpMethod m) noexcept {
    return m == HttpMethod::Get || m == HttpMethod::Head || m == HttpMethod::Put || m == HttpMethod::Delete || m == HttpMethod::Options;
}

struct Cracked {
    bool https = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path;
};

std::optional<Cracked> crack(std::string_view url) {
    const std::wstring wide = core::utf8_to_wide(url);
    if (wide.empty()) {
        return std::nullopt;
    }
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts)) {
        return std::nullopt;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) {
        return std::nullopt;
    }
    Cracked out;
    out.https = parts.nScheme == INTERNET_SCHEME_HTTPS;
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.port = parts.nPort;
    out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) {
        out.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (out.path.empty()) {
        out.path = L"/";
    }
    if (out.host.empty()) {
        return std::nullopt;
    }
    return out;
}

std::string resolve_location(std::string_view base, std::string_view location) {
    const std::wstring b = core::utf8_to_wide(base);
    const std::wstring r = core::utf8_to_wide(location);
    std::wstring out(4096, L'\0');
    DWORD len = static_cast<DWORD>(out.size());
    if (FAILED(UrlCombineW(b.c_str(), r.c_str(), out.data(), &len, 0))) {
        return std::string(location);
    }
    out.resize(len);
    return core::wide_to_utf8(out);
}

std::optional<std::wstring> query_raw_headers(HINTERNET request) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    value.resize(size / sizeof(wchar_t));
    return value;
}

void parse_headers(std::wstring_view raw, HttpHeaders& out) {
    bool first = true;
    while (!raw.empty()) {
        const auto end = raw.find(L"\r\n");
        const std::wstring_view line = raw.substr(0, end);
        raw = end == std::wstring_view::npos ? std::wstring_view{} : raw.substr(end + 2);
        if (first) {
            first = false;
            continue; // status line
        }
        const auto colon = line.find(L':');
        if (colon == std::wstring_view::npos || colon == 0) {
            continue;
        }
        std::wstring_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) {
            value.remove_prefix(1);
        }
        out.push_back(HttpHeader{.name = core::wide_to_utf8(line.substr(0, colon)), .value = core::wide_to_utf8(value)});
    }
}

std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    if (text.empty() || text.front() < '0' || text.front() > '9') {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            break;
        }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

/// Bridges a stop_token and an optional deadline (thread-pool timer) to one internal stop source.
class CancelScope {
public:
    CancelScope(std::stop_token user, std::optional<std::chrono::milliseconds> timeout)
        : m_user_cb(user, Forward{this, false}) {
        if (timeout && timeout->count() > 0) {
            m_timer = CreateThreadpoolTimer(&CancelScope::on_timer, this, nullptr);
            if (m_timer != nullptr) {
                ULARGE_INTEGER due{};
                due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(timeout->count()) * 10'000LL);
                FILETIME ft{due.LowPart, due.HighPart};
                SetThreadpoolTimer(m_timer, &ft, 0, 0);
            }
        }
    }
    ~CancelScope() {
        if (m_timer != nullptr) {
            SetThreadpoolTimer(m_timer, nullptr, 0, 0);
            WaitForThreadpoolTimerCallbacks(m_timer, TRUE);
            CloseThreadpoolTimer(m_timer);
        }
    }
    CancelScope(const CancelScope&) = delete;
    CancelScope& operator=(const CancelScope&) = delete;

    [[nodiscard]] std::stop_token token() const noexcept { return m_source.get_token(); }
    [[nodiscard]] bool timed_out() const noexcept { return m_timed_out.load(std::memory_order_acquire); }
    [[nodiscard]] bool stopped() const noexcept { return m_source.stop_requested(); }

private:
    struct Forward {
        CancelScope* self;
        bool timer;
        void operator()() const noexcept { self->m_source.request_stop(); }
    };
    static void CALLBACK on_timer(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
        auto* self = static_cast<CancelScope*>(context);
        self->m_timed_out.store(true, std::memory_order_release);
        self->m_source.request_stop();
    }

    std::stop_source m_source;
    std::atomic<bool> m_timed_out{false};
    std::stop_callback<Forward> m_user_cb;
    PTP_TIMER m_timer = nullptr;
};

/// Per-phase inactivity timer: armed around each blocking WinHTTP call (headers, every body read), it
/// closes the request handle when the peer stays silent too long. WinHTTP's own receive timeouts are
/// set as well, but they did not fire while a synchronous WinHttpSendRequest waited for a silent server
/// (measured on Windows 11 26300).
class PhaseWatchdog {
public:
    explicit PhaseWatchdog(std::function<void()> on_fire) : m_on_fire(std::move(on_fire)) {
        m_timer = CreateThreadpoolTimer(&PhaseWatchdog::on_timer, this, nullptr);
    }
    ~PhaseWatchdog() {
        if (m_timer != nullptr) {
            SetThreadpoolTimer(m_timer, nullptr, 0, 0);
            WaitForThreadpoolTimerCallbacks(m_timer, TRUE);
            CloseThreadpoolTimer(m_timer);
        }
    }
    PhaseWatchdog(const PhaseWatchdog&) = delete;
    PhaseWatchdog& operator=(const PhaseWatchdog&) = delete;

    void arm(std::chrono::milliseconds d) noexcept {
        if (m_timer == nullptr || d.count() <= 0) {
            return;
        }
        ULARGE_INTEGER due{};
        due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(d.count()) * 10'000LL);
        FILETIME ft{due.LowPart, due.HighPart};
        SetThreadpoolTimer(m_timer, &ft, 0, 0);
    }
    void disarm() noexcept {
        if (m_timer != nullptr) {
            SetThreadpoolTimer(m_timer, nullptr, 0, 0);
        }
    }
    [[nodiscard]] bool fired() const noexcept { return m_fired.load(std::memory_order_acquire); }

private:
    static void CALLBACK on_timer(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
        auto* self = static_cast<PhaseWatchdog*>(context);
        self->m_fired.store(true, std::memory_order_release);
        self->m_on_fire();
    }
    std::function<void()> m_on_fire;
    std::atomic<bool> m_fired{false};
    PTP_TIMER m_timer = nullptr;
};

/// Waits up to d or until stop: an event-driven sleep for backoff.
bool interruptible_wait(std::stop_token stop, std::chrono::milliseconds d) {
    std::mutex m;
    std::condition_variable_any cv;
    std::unique_lock lock(m);
    return !cv.wait_for(lock, stop, d, [] { return false; }) && !stop.stop_requested();
}

/// What a request does with its body.
struct BodyHandler {
    /// Called once the final (non-redirect) head is known. Return false to skip the body.
    std::function<bool(const HttpResponse& head)> on_head;
    std::function<bool(std::span<const std::byte>)> on_data;
    /// Undo delivered data before a retry; empty = not restartable once data was delivered.
    std::function<void()> restart;
};

} // namespace

std::string_view to_string(HttpMethod method) noexcept {
    switch (method) {
    case HttpMethod::Get: return "GET";
    case HttpMethod::Head: return "HEAD";
    case HttpMethod::Post: return "POST";
    case HttpMethod::Put: return "PUT";
    case HttpMethod::Patch: return "PATCH";
    case HttpMethod::Delete: return "DELETE";
    case HttpMethod::Options: return "OPTIONS";
    }
    return "GET";
}

std::optional<HttpMethod> parse_http_method(std::string_view text) noexcept {
    for (const HttpMethod m : {HttpMethod::Get, HttpMethod::Head, HttpMethod::Post, HttpMethod::Put, HttpMethod::Patch, HttpMethod::Delete, HttpMethod::Options}) {
        if (iequals(text, to_string(m))) {
            return m;
        }
    }
    return std::nullopt;
}

std::string_view to_string(HttpErrorKind kind) noexcept {
    switch (kind) {
    case HttpErrorKind::InvalidUrl: return "invalid-url";
    case HttpErrorKind::Refused: return "refused";
    case HttpErrorKind::Connect: return "connect";
    case HttpErrorKind::Tls: return "tls";
    case HttpErrorKind::Transport: return "transport";
    case HttpErrorKind::Timeout: return "timeout";
    case HttpErrorKind::Cancelled: return "cancelled";
    case HttpErrorKind::TooLarge: return "too-large";
    case HttpErrorKind::TooManyRedirects: return "too-many-redirects";
    case HttpErrorKind::Status: return "status";
    case HttpErrorKind::Io: return "io";
    case HttpErrorKind::HashMismatch: return "hash-mismatch";
    case HttpErrorKind::Aborted: return "aborted";
    }
    return "unknown";
}

std::optional<std::string_view> find_header(const HttpHeaders& headers, std::string_view name) noexcept {
    for (const auto& h : headers) {
        if (iequals(h.name, name)) {
            return std::string_view(h.value);
        }
    }
    return std::nullopt;
}

bool is_loopback_host(std::string_view host_in) noexcept {
    std::string_view host = host_in;
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    if (iequals(host, "localhost") || host == "::1") {
        return true;
    }
    return host.starts_with("127.");
}

std::string url_host(std::string_view url) {
    const auto c = crack(url);
    return c ? lower(core::wide_to_utf8(c->host)) : std::string{};
}

std::string check_url(std::string_view url, const HttpClientOptions& options) {
    const auto c = crack(url);
    if (!c) {
        return "not an http(s) URL";
    }
    if (!c->https && options.require_https &&
        !(options.allow_insecure_loopback && is_loopback_host(core::wide_to_utf8(c->host)))) {
        return "plain http:// is not allowed";
    }
    if (options.url_filter && !options.url_filter(url)) {
        return "URL not allowed: " + std::string(url);
    }
    return {};
}

// ---- Impl ---------------------------------------------------------------------------------------------

struct HttpClient::Impl {
    HttpClientOptions options;
    std::unique_ptr<HttpCache> cache;

    std::mutex session_mutex;
    UniqueInternet session;        // proxy as configured
    UniqueInternet direct_session; // loopback in System mode
    std::atomic<bool> opened{false};

    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> retries{0};
    std::atomic<std::uint64_t> redirects{0};
    std::atomic<std::uint64_t> cache_hits{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<std::uint64_t> sessions_opened{0};

    HINTERNET get_session(bool loopback, DWORD& error);
    HttpResult<HttpResponse> run(HttpRequest& request, BodyHandler& body, std::uint64_t max_bytes);
};

HINTERNET HttpClient::Impl::get_session(bool loopback, DWORD& error) {
    const bool direct = loopback && options.proxy.mode == ProxyMode::System;
    std::scoped_lock lock(session_mutex);
    UniqueInternet& slot = direct ? direct_session : session;
    if (slot) {
        return slot.get();
    }
    DWORD access = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    std::wstring proxy;
    std::wstring bypass;
    if (direct || options.proxy.mode == ProxyMode::Direct) {
        access = WINHTTP_ACCESS_TYPE_NO_PROXY;
    } else if (options.proxy.mode == ProxyMode::Explicit) {
        access = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        proxy = core::utf8_to_wide(options.proxy.server);
        bypass = core::utf8_to_wide(options.proxy.bypass);
    }
    HINTERNET h = WinHttpOpen(
        options.user_agent.c_str(), access,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(),
        bypass.empty() ? WINHTTP_NO_PROXY_BYPASS : bypass.c_str(), 0);
    if (h == nullptr && access == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY) {
        // Windows 8.0 and older do not know AUTOMATIC_PROXY.
        h = WinHttpOpen(options.user_agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (h == nullptr) {
        error = GetLastError();
        return nullptr;
    }
    WinHttpSetTimeouts(h, static_cast<int>(clamp_ms(options.resolve_timeout)), static_cast<int>(clamp_ms(options.connect_timeout)),
                       static_cast<int>(clamp_ms(options.send_timeout)), static_cast<int>(clamp_ms(options.receive_timeout)));
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    if (options.max_connections_per_server > 0) {
        DWORD conns = options.max_connections_per_server;
        WinHttpSetOption(h, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &conns, sizeof(conns));
        WinHttpSetOption(h, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &conns, sizeof(conns));
    }
    slot.reset(h);
    opened.store(true, std::memory_order_release);
    sessions_opened.fetch_add(1, std::memory_order_relaxed);
    return h;
}

HttpResult<HttpResponse> HttpClient::Impl::run(HttpRequest& request, BodyHandler& body, std::uint64_t max_bytes) {
    const RetryPolicy retry = request.retry.value_or(options.retry);
    const std::uint32_t max_attempts = std::max<std::uint32_t>(1, retry.max_attempts);
    const bool may_retry_method = idempotent(request.method) || retry.retry_non_idempotent;
    CancelScope cancel(request.stop, request.timeout);
    const std::stop_token stop = cancel.token();
    auto backoff = retry.initial_backoff;

    const auto stopped_error = [&](std::uint32_t attempt) {
        return HttpError{
            .kind = cancel.timed_out() ? HttpErrorKind::Timeout : HttpErrorKind::Cancelled,
            .message = cancel.timed_out() ? "request deadline exceeded" : "cancelled",
            .win32_error = cancel.timed_out() ? static_cast<std::uint32_t>(ERROR_WINHTTP_TIMEOUT) : static_cast<std::uint32_t>(ERROR_CANCELLED),
            .status = 0,
            .attempts = attempt,
            .body = {},
        };
    };

    for (std::uint32_t attempt = 1;; ++attempt) {
        if (stop.stop_requested()) {
            return std::unexpected(stopped_error(attempt - 1));
        }
        bool delivered = false;
        std::optional<std::chrono::milliseconds> retry_after;
        HttpResult<HttpResponse> outcome = [&]() -> HttpResult<HttpResponse> {
            HttpResponse resp;
            resp.attempts = attempt;
            std::string url = request.url;
            HttpMethod method = request.method;
            const std::string* send_body = &request.body;
            const std::string empty_body;
            HttpHeaders headers = options.default_headers;
            headers.insert(headers.end(), request.headers.begin(), request.headers.end());
            std::string first_host = url_host(url);
            for (std::uint32_t hop = 0;; ++hop) {
                if (const std::string refused = check_url(url, options); !refused.empty()) {
                    return std::unexpected(HttpError{.kind = hop == 0 && refused == "not an http(s) URL" ? HttpErrorKind::InvalidUrl : HttpErrorKind::Refused,
                                                     .message = hop == 0 ? refused : "redirect refused: " + refused});
                }
                const auto cracked = crack(url);
                const bool loopback = is_loopback_host(core::wide_to_utf8(cracked->host));
                DWORD error = 0;
                HINTERNET session_handle = get_session(loopback, error);
                if (session_handle == nullptr) {
                    return std::unexpected(HttpError{.kind = HttpErrorKind::Transport, .message = "WinHttpOpen failed: " + win32_text(error), .win32_error = error});
                }
                const UniqueInternet connection(WinHttpConnect(session_handle, cracked->host.c_str(), cracked->port, 0));
                if (!connection) {
                    error = GetLastError();
                    return std::unexpected(HttpError{.kind = classify(error), .message = "WinHttpConnect failed: " + win32_text(error), .win32_error = error});
                }
                const std::wstring verb = core::utf8_to_wide(to_string(method));
                HINTERNET raw = WinHttpOpenRequest(connection.get(), verb.c_str(), cracked->path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, cracked->https ? WINHTTP_FLAG_SECURE : 0);
                if (raw == nullptr) {
                    error = GetLastError();
                    return std::unexpected(HttpError{.kind = classify(error), .message = "WinHttpOpenRequest failed: " + win32_text(error), .win32_error = error});
                }
                // A stop request closes the request handle, which makes a blocked WinHTTP call return at once.
                std::mutex handle_mutex;
                HINTERNET live = raw;
                const auto close_request = [&]() noexcept {
                    std::scoped_lock lock(handle_mutex);
                    if (live != nullptr) {
                        WinHttpCloseHandle(live);
                        live = nullptr;
                    }
                };
                struct CloseOnExit {
                    decltype(close_request)& fn;
                    ~CloseOnExit() { fn(); }
                } closer{close_request};
                const std::stop_callback on_stop(stop, close_request);
                PhaseWatchdog watchdog(close_request);
                const auto fail = [&](std::string what) {
                    const DWORD code = GetLastError();
                    if (stop.stop_requested()) {
                        return stopped_error(attempt);
                    }
                    if (watchdog.fired()) {
                        return HttpError{.kind = HttpErrorKind::Timeout,
                                         .message = "no data from the server for " + std::to_string(options.receive_timeout.count()) + " ms",
                                         .win32_error = static_cast<std::uint32_t>(ERROR_WINHTTP_TIMEOUT)};
                    }
                    return HttpError{.kind = classify(code), .message = std::move(what) + " (" + win32_text(code) + ")", .win32_error = code};
                };

                // WinHttpReceiveResponse has its own timeout (default 90 s) separate from the session's receive timeout.
                DWORD response_timeout = clamp_ms(options.receive_timeout);
                if (response_timeout > 0) {
                    WinHttpSetTimeouts(raw, static_cast<int>(clamp_ms(options.resolve_timeout)), static_cast<int>(clamp_ms(options.connect_timeout)),
                                       static_cast<int>(clamp_ms(options.send_timeout)), static_cast<int>(response_timeout));
                    WinHttpSetOption(raw, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &response_timeout, sizeof(response_timeout));
                }
                DWORD disable = WINHTTP_DISABLE_REDIRECTS;
                WinHttpSetOption(raw, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
                if (options.decompress) {
                    DWORD flags = WINHTTP_DECOMPRESSION_FLAG_ALL;
                    WinHttpSetOption(raw, WINHTTP_OPTION_DECOMPRESSION, &flags, sizeof(flags));
                }
                if (options.enable_http2 || options.enable_http3) {
                    DWORD proto = (options.enable_http2 ? WINHTTP_PROTOCOL_FLAG_HTTP2 : 0u) | (options.enable_http3 ? WINHTTP_PROTOCOL_FLAG_HTTP3 : 0u);
                    if (!WinHttpSetOption(raw, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &proto, sizeof(proto)) && options.enable_http3) {
                        proto = WINHTTP_PROTOCOL_FLAG_HTTP2;
                        WinHttpSetOption(raw, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &proto, sizeof(proto));
                    }
                }
                std::wstring header_text;
                const bool cross_host = url_host(url) != first_host;
                for (const auto& h : headers) {
                    if (cross_host && (iequals(h.name, "Authorization") || iequals(h.name, "Cookie"))) {
                        continue; // credentials never follow a redirect to another host
                    }
                    header_text += core::utf8_to_wide(h.name) + L": " + core::utf8_to_wide(h.value) + L"\r\n";
                }
                if (!options.decompress) {
                    header_text += L"Accept-Encoding: identity\r\n";
                }
                requests.fetch_add(1, std::memory_order_relaxed);
                const DWORD body_len = static_cast<DWORD>(send_body->size());
                // Synchronous WinHttpSendRequest may itself wait for the response headers, so the watchdog
                // covers send + headers; large uploads also get the send timeout.
                watchdog.arm(body_len > (1u << 20) ? options.send_timeout + options.receive_timeout : options.receive_timeout);
                if (!WinHttpSendRequest(raw, header_text.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_text.c_str(),
                                        header_text.empty() ? 0 : static_cast<DWORD>(-1L),
                                        body_len == 0 ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(send_body->data()), body_len, body_len, 0)) {
                    return std::unexpected(fail("WinHttpSendRequest failed"));
                }
                if (!WinHttpReceiveResponse(raw, nullptr)) {
                    return std::unexpected(fail("WinHttpReceiveResponse failed"));
                }
                watchdog.disarm();
                DWORD status = 0;
                DWORD size = sizeof(status);
                WinHttpQueryHeaders(raw, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                resp.status = static_cast<int>(status);
                resp.headers.clear();
                if (const auto raw_headers = query_raw_headers(raw)) {
                    parse_headers(*raw_headers, resp.headers);
                }
                resp.final_url = url;
                DWORD used = 0;
                DWORD used_size = sizeof(used);
                resp.protocol = "HTTP/1.1";
                if (WinHttpQueryOption(raw, WINHTTP_OPTION_HTTP_PROTOCOL_USED, &used, &used_size)) {
                    if ((used & WINHTTP_PROTOCOL_FLAG_HTTP3) != 0) {
                        resp.protocol = "HTTP/3";
                    } else if ((used & WINHTTP_PROTOCOL_FLAG_HTTP2) != 0) {
                        resp.protocol = "HTTP/2";
                    }
                }
                resp.content_length.reset();
                if (const auto cl = resp.header("Content-Length")) {
                    resp.content_length = parse_u64(*cl);
                }

                const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
                if (redirect && options.follow_redirects) {
                    const auto location = resp.header("Location");
                    if (location && !location->empty()) {
                        if (hop + 1 > options.max_redirects) {
                            return std::unexpected(HttpError{.kind = HttpErrorKind::TooManyRedirects,
                                                             .message = "more than " + std::to_string(options.max_redirects) + " redirects",
                                                             .status = resp.status});
                        }
                        const std::string next = resolve_location(url, *location);
                        if (crack(url)->https && crack(next) && !crack(next)->https && !options.allow_https_downgrade) {
                            return std::unexpected(HttpError{.kind = HttpErrorKind::Refused, .message = "redirect from https to http refused", .status = resp.status});
                        }
                        if (status == 303 || ((status == 301 || status == 302) && method == HttpMethod::Post)) {
                            if (method != HttpMethod::Head) {
                                method = HttpMethod::Get;
                            }
                            send_body = &empty_body;
                        }
                        url = next;
                        ++resp.redirects;
                        redirects.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                }

                // Final response.
                const bool server_error = status >= 500 || status == 429;
                if (server_error && retry.retry_server_errors && may_retry_method && attempt < max_attempts) {
                    if (retry.honor_retry_after) {
                        if (const auto ra = resp.header("Retry-After")) {
                            if (const auto secs = parse_u64(*ra)) {
                                retry_after = std::min(std::chrono::milliseconds{static_cast<long long>(*secs) * 1000}, retry.max_retry_after);
                            }
                        }
                    }
                    return std::unexpected(HttpError{.kind = HttpErrorKind::Status, .message = "server error " + std::to_string(status) + " (retrying)",
                                                     .status = resp.status, .attempts = attempt});
                }
                if (resp.content_length && max_bytes > 0 && *resp.content_length > max_bytes && method != HttpMethod::Head) {
                    return std::unexpected(HttpError{.kind = HttpErrorKind::TooLarge,
                                                     .message = "response of " + std::to_string(*resp.content_length) + " bytes exceeds the limit",
                                                     .status = resp.status, .attempts = attempt});
                }
                if (method == HttpMethod::Head || !body.on_head(resp)) {
                    return resp;
                }
                std::vector<std::byte> buffer(128u * 1024u);
                std::uint64_t total = 0;
                while (true) {
                    if (stop.stop_requested()) {
                        return std::unexpected(stopped_error(attempt));
                    }
                    DWORD read = 0;
                    watchdog.arm(options.receive_timeout);
                    if (!WinHttpReadData(raw, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
                        return std::unexpected(fail("WinHttpReadData failed"));
                    }
                    watchdog.disarm();
                    if (read == 0) {
                        break;
                    }
                    total += read;
                    bytes_received.fetch_add(read, std::memory_order_relaxed);
                    if (max_bytes > 0 && total > max_bytes) {
                        return std::unexpected(HttpError{.kind = HttpErrorKind::TooLarge, .message = "response body exceeds the limit of " + std::to_string(max_bytes) + " bytes",
                                                         .status = resp.status, .attempts = attempt});
                    }
                    delivered = true;
                    if (!body.on_data(std::span<const std::byte>(buffer.data(), read))) {
                        return std::unexpected(HttpError{.kind = HttpErrorKind::Aborted, .message = "aborted by the body sink", .status = resp.status, .attempts = attempt});
                    }
                }
                return resp;
            }
        }();

        if (outcome) {
            return outcome;
        }
        HttpError& err = outcome.error();
        err.attempts = attempt;
        if (stop.stop_requested()) {
            return std::unexpected(stopped_error(attempt));
        }
        const bool retry_status = err.kind == HttpErrorKind::Status && err.message.ends_with("(retrying)");
        const bool can_retry = attempt < max_attempts && may_retry_method &&
                               (retry_status || (retry.retry_transport_errors && retryable(err.kind))) &&
                               (!delivered || static_cast<bool>(body.restart));
        if (!can_retry) {
            if (retry_status) {
                err.message = "server error " + std::to_string(err.status);
            }
            return outcome;
        }
        if (delivered) {
            body.restart();
        }
        retries.fetch_add(1, std::memory_order_relaxed);
        const auto wait = retry_after.value_or(backoff);
        if (!interruptible_wait(stop, wait)) {
            return std::unexpected(stopped_error(attempt));
        }
        backoff = std::min(std::chrono::milliseconds{static_cast<long long>(static_cast<double>(backoff.count()) * retry.multiplier)}, retry.max_backoff);
    }
}

// ---- HttpClient ---------------------------------------------------------------------------------------

HttpClient::HttpClient(HttpClientOptions options) : m_impl(std::make_unique<Impl>()) {
    m_impl->options = std::move(options);
    if (m_impl->options.cache) {
        m_impl->cache = std::make_unique<HttpCache>(*m_impl->options.cache);
    }
}

HttpClient::~HttpClient() = default;

HttpCache* HttpClient::cache() noexcept { return m_impl->cache.get(); }
const HttpClientOptions& HttpClient::options() const noexcept { return m_impl->options; }
bool HttpClient::session_open() const noexcept { return m_impl->opened.load(std::memory_order_acquire); }

HttpClientStats HttpClient::stats() const noexcept {
    return HttpClientStats{
        .requests = m_impl->requests.load(),
        .retries = m_impl->retries.load(),
        .redirects = m_impl->redirects.load(),
        .cache_hits = m_impl->cache_hits.load(),
        .bytes_received = m_impl->bytes_received.load(),
        .sessions_opened = m_impl->sessions_opened.load(),
    };
}

HttpResult<HttpResponse> HttpClient::send(HttpRequest request) {
    const bool cacheable = m_impl->cache && request.use_cache && request.method == HttpMethod::Get &&
                           !find_header(request.headers, "Authorization") && !find_header(request.headers, "Range");
    std::string cache_key;
    if (cacheable) {
        cache_key = "GET " + request.url;
        if (auto hit = m_impl->cache->lookup(cache_key)) {
            m_impl->cache_hits.fetch_add(1, std::memory_order_relaxed);
            HttpResponse copy = *hit;
            copy.from_cache = true;
            copy.attempts = 0;
            return copy;
        }
    }
    std::string body_text;
    BodyHandler handler{
        .on_head = [](const HttpResponse&) { return true; },
        .on_data = [&](std::span<const std::byte> part) {
            body_text.append(reinterpret_cast<const char*>(part.data()), part.size());
            return true;
        },
        .restart = [&]() { body_text.clear(); },
    };
    const std::uint64_t limit = request.max_body_bytes.value_or(m_impl->options.max_body_bytes);
    auto result = m_impl->run(request, handler, limit);
    if (!result) {
        return result;
    }
    result->body = std::move(body_text);
    if (cacheable && result->status == 200) {
        m_impl->cache->store(std::move(cache_key), std::make_shared<const HttpResponse>(*result));
    }
    return result;
}

HttpResult<HttpResponse> HttpClient::stream(HttpRequest request, const BodySink& sink, const StreamOptions& options) {
    const auto start = std::chrono::steady_clock::now();
    auto last = start - options.progress_interval;
    TransferProgress progress;
    std::string error_body;
    bool ok_status = true;
    const auto report = [&](bool force) {
        if (!options.on_progress) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last < options.progress_interval) {
            return;
        }
        last = now;
        const double secs = std::chrono::duration<double>(now - start).count();
        progress.bytes_per_second = secs > 0 ? static_cast<double>(progress.received) / secs : 0.0;
        options.on_progress(progress);
    };
    BodyHandler handler{
        .on_head = [&](const HttpResponse& head) {
            ok_status = head.ok();
            progress.total = head.content_length;
            if (head.header("Content-Encoding")) {
                progress.total.reset(); // decoded size unknown
            }
            return true;
        },
        .on_data = [&](std::span<const std::byte> part) {
            if (!ok_status) {
                if (error_body.size() < 64u * 1024u) {
                    error_body.append(reinterpret_cast<const char*>(part.data()), std::min<std::size_t>(part.size(), 64u * 1024u - error_body.size()));
                }
                return true;
            }
            progress.received += part.size();
            if (!sink(part)) {
                return false;
            }
            report(false);
            return true;
        },
        .restart = {},
    };
    const std::uint64_t limit = request.max_body_bytes.value_or(m_impl->options.max_download_bytes);
    auto result = m_impl->run(request, handler, limit);
    if (!result) {
        return result;
    }
    if (!ok_status) {
        return std::unexpected(HttpError{.kind = HttpErrorKind::Status, .message = "HTTP status " + std::to_string(result->status),
                                         .status = result->status, .attempts = result->attempts, .body = std::move(error_body)});
    }
    report(true);
    return result;
}

HttpResult<DownloadResult> HttpClient::download(HttpRequest request, const std::filesystem::path& destination, const DownloadOptions& options) {
    std::optional<crypto::HashAlgorithm> algorithm = options.hash;
    if (!options.expected_hash.empty() && !algorithm) {
        switch (options.expected_hash.size()) {
        case 32: algorithm = crypto::HashAlgorithm::Md5; break;
        case 40: algorithm = crypto::HashAlgorithm::Sha1; break;
        case 64: algorithm = crypto::HashAlgorithm::Sha256; break;
        case 128: algorithm = crypto::HashAlgorithm::Sha512; break;
        default:
            return std::unexpected(HttpError{.kind = HttpErrorKind::HashMismatch, .message = "cannot infer the hash algorithm from expected_hash"});
        }
    }
    if (!options.overwrite && GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return std::unexpected(HttpError{.kind = HttpErrorKind::Io, .message = "destination exists: " + destination.string()});
    }
    std::error_code ec;
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), ec);
    }
    std::filesystem::path partial = destination;
    partial += L".partial";
    struct HandleCloser {
        void operator()(HANDLE h) const noexcept {
            if (h != nullptr && h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
        }
    };
    std::unique_ptr<void, HandleCloser> file(CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        return std::unexpected(HttpError{.kind = HttpErrorKind::Io, .message = "cannot create " + partial.string() + ": " + win32_text(code), .win32_error = code});
    }
    std::optional<crypto::Hasher> hasher;
    if (algorithm) {
        hasher.emplace(*algorithm);
    }
    std::uint64_t written = 0;
    DWORD write_error = 0;
    const auto discard = [&]() {
        file.reset();
        DeleteFileW(partial.c_str());
    };

    const auto start = std::chrono::steady_clock::now();
    auto last = start - options.progress_interval;
    TransferProgress progress;
    std::string error_body;
    bool ok_status = true;
    const auto report = [&](bool force) {
        if (!options.on_progress) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last < options.progress_interval) {
            return;
        }
        last = now;
        const double secs = std::chrono::duration<double>(now - start).count();
        progress.bytes_per_second = secs > 0 ? static_cast<double>(progress.received) / secs : 0.0;
        options.on_progress(progress);
    };
    BodyHandler handler{
        .on_head = [&](const HttpResponse& head) {
            ok_status = head.ok();
            progress.total = head.header("Content-Encoding") ? std::nullopt : head.content_length;
            if (ok_status && progress.total && *progress.total > 0) {
                FILE_ALLOCATION_INFO alloc{};
                alloc.AllocationSize.QuadPart = static_cast<LONGLONG>(*progress.total);
                SetFileInformationByHandle(file.get(), FileAllocationInfo, &alloc, sizeof(alloc));
            }
            return true;
        },
        .on_data = [&](std::span<const std::byte> part) {
            if (!ok_status) {
                if (error_body.size() < 64u * 1024u) {
                    error_body.append(reinterpret_cast<const char*>(part.data()), std::min<std::size_t>(part.size(), 64u * 1024u - error_body.size()));
                }
                return true;
            }
            DWORD n = 0;
            if (!WriteFile(file.get(), part.data(), static_cast<DWORD>(part.size()), &n, nullptr) || n != part.size()) {
                write_error = GetLastError();
                return false;
            }
            if (hasher) {
                hasher->update(part);
            }
            written += part.size();
            progress.received = written;
            report(false);
            return true;
        },
        .restart = [&]() {
            LARGE_INTEGER zero{};
            SetFilePointerEx(file.get(), zero, nullptr, FILE_BEGIN);
            SetEndOfFile(file.get());
            written = 0;
            progress.received = 0;
            if (hasher) {
                (void)hasher->finish(); // finish() resets the reusable CNG hash
            }
        },
    };
    const std::uint64_t limit = request.max_body_bytes.value_or(m_impl->options.max_download_bytes);
    auto result = m_impl->run(request, handler, limit);
    if (!result) {
        discard();
        if (result.error().kind == HttpErrorKind::Aborted && write_error != 0) {
            return std::unexpected(HttpError{.kind = HttpErrorKind::Io, .message = "write failed: " + win32_text(write_error), .win32_error = write_error});
        }
        return std::unexpected(std::move(result.error()));
    }
    if (!ok_status) {
        discard();
        return std::unexpected(HttpError{.kind = HttpErrorKind::Status, .message = "HTTP status " + std::to_string(result->status),
                                         .status = result->status, .attempts = result->attempts, .body = std::move(error_body)});
    }
    // Trim a preallocation larger than the body (e.g. decoded size differs).
    FILE_END_OF_FILE_INFO eof{};
    eof.EndOfFile.QuadPart = static_cast<LONGLONG>(written);
    SetFileInformationByHandle(file.get(), FileEndOfFileInfo, &eof, sizeof(eof));
    DownloadResult out;
    if (hasher) {
        out.digest = hasher->finish();
        if (!options.expected_hash.empty() && (!out.digest || !out.digest->matches_hex(options.expected_hash))) {
            discard();
            return std::unexpected(HttpError{.kind = HttpErrorKind::HashMismatch,
                                             .message = "hash mismatch: expected " + options.expected_hash + ", got " + (out.digest ? out.digest->hex() : std::string("?")),
                                             .status = result->status, .attempts = result->attempts});
        }
    }
    file.reset();
    if (!MoveFileExW(partial.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH | (options.overwrite ? MOVEFILE_REPLACE_EXISTING : 0))) {
        const DWORD code = GetLastError();
        DeleteFileW(partial.c_str());
        return std::unexpected(HttpError{.kind = HttpErrorKind::Io, .message = "cannot move the download into place: " + win32_text(code), .win32_error = code});
    }
    report(true);
    out.response = std::move(*result);
    out.bytes = written;
    out.path = destination;
    return out;
}

HttpResult<HttpResponse> HttpClient::get(std::string url, HttpHeaders headers) {
    return send(HttpRequest{.method = HttpMethod::Get, .url = std::move(url), .headers = std::move(headers)});
}

HttpResult<HttpResponse> HttpClient::head(std::string url, HttpHeaders headers) {
    return send(HttpRequest{.method = HttpMethod::Head, .url = std::move(url), .headers = std::move(headers)});
}

HttpResult<HttpResponse> HttpClient::post(std::string url, std::string body, std::string content_type) {
    HttpRequest req{.method = HttpMethod::Post, .url = std::move(url), .body = std::move(body)};
    req.headers.push_back(HttpHeader{.name = "Content-Type", .value = std::move(content_type)});
    return send(std::move(req));
}

} // namespace boson::net
