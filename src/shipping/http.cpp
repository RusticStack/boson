#include <boson/shipping/http.hpp>

#include <boson/core/result.hpp>

#include "file_util.hpp"

#include <winhttp.h>
#include <shlwapi.h>

#include <glaze/glaze.hpp>

#include <atomic>
#include <cctype>
#include <cwctype>
#include <memory>
#include <mutex>

namespace boson::shipping {

namespace detail {
/// Sidecar "<destination>.partial.json": what the partial file was downloaded from.
struct PartialMeta {
    std::string url;
    std::string etag;
    std::string last_modified;
};
} // namespace detail

namespace {

using detail::PartialMeta;

using detail::UniqueHandle;
using detail::make_handle;

struct InternetCloser {
    void operator()(HINTERNET handle) const noexcept {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
};
using UniqueInternet = std::unique_ptr<void, InternetCloser>;

std::wstring lower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& ch : out) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return out;
}

DWORD clamp_timeout(std::chrono::milliseconds ms) noexcept {
    if (ms.count() <= 0) {
        return 0;
    }
    return static_cast<DWORD>(std::min<long long>(ms.count(), 0x7FFFFFFF));
}

std::optional<std::wstring> query_header(HINTERNET request, DWORD query) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    value.resize(size / sizeof(wchar_t));
    return value;
}

std::optional<std::uint64_t> parse_u64(std::wstring_view text) noexcept {
    while (!text.empty() && text.front() == L' ') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            break;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - L'0');
    }
    return value;
}

/// "bytes 100-199/1000" -> {100, 1000}; total is nullopt for "*".
struct ContentRange {
    std::uint64_t start = 0;
    std::optional<std::uint64_t> total;
};

std::optional<ContentRange> parse_content_range(std::wstring_view value) {
    const auto bytes = value.find(L"bytes");
    if (bytes == std::wstring_view::npos) {
        return std::nullopt;
    }
    value.remove_prefix(bytes + 5);
    const auto dash = value.find(L'-');
    const auto slash = value.find(L'/');
    if (dash == std::wstring_view::npos || slash == std::wstring_view::npos) {
        return std::nullopt;
    }
    const auto start = parse_u64(value.substr(0, dash));
    if (!start) {
        return std::nullopt;
    }
    ContentRange out{.start = *start};
    const auto total_text = value.substr(slash + 1);
    if (total_text.find(L'*') == std::wstring_view::npos) {
        out.total = parse_u64(total_text);
    }
    return out;
}

bool is_loopback_host(std::wstring_view host) {
    const std::wstring h = lower(host);
    return h == L"localhost" || h == L"127.0.0.1" || h == L"::1" || h == L"[::1]" || h.starts_with(L"127.");
}

struct CrackedUrl {
    INTERNET_SCHEME scheme = INTERNET_SCHEME_HTTPS;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path_and_query;
};

std::optional<CrackedUrl> crack(const std::wstring& url) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        return std::nullopt;
    }
    CrackedUrl out;
    out.scheme = parts.nScheme;
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.port = parts.nPort;
    out.path_and_query.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) {
        out.path_and_query.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (out.path_and_query.empty()) {
        out.path_and_query = L"/";
    }
    return out;
}

std::optional<std::filesystem::path> file_url_to_path(std::string_view url) {
    std::wstring wide = core::utf8_to_wide(url);
    std::wstring path(32768, L'\0');
    DWORD len = static_cast<DWORD>(path.size());
    if (FAILED(PathCreateFromUrlW(wide.c_str(), path.data(), &len, 0))) {
        return std::nullopt;
    }
    path.resize(len);
    return std::filesystem::path{path};
}

struct ResponseHead {
    int status = 0;
    std::string etag;
    std::string last_modified;
    std::string content_type;
    std::optional<std::uint64_t> content_length;
    std::optional<ContentRange> content_range;
    std::string final_url;
};

struct TransferError {
    std::string message;
    std::uint32_t win32_error = 0;
    bool cancelled = false;
};

struct RequestSpec {
    std::string url;
    std::optional<std::uint64_t> range_from;
    std::string if_range;
    bool decompress = false;
};

/// Returns true from on_head to read the body; on_chunk returns false to abort.
using HeadFn = std::function<bool(const ResponseHead&)>;
using ChunkFn = std::function<bool(const std::uint8_t* data, std::size_t size)>;

std::optional<TransferError> perform_http(
    const RequestSpec& spec,
    const HttpOptions& options,
    std::stop_token stop,
    const HeadFn& on_head,
    const ChunkFn& on_chunk) {
    const std::wstring wide_url = core::utf8_to_wide(spec.url);
    const auto cracked = crack(wide_url);
    if (!cracked) {
        return TransferError{.message = "malformed URL", .win32_error = GetLastError()};
    }
    const bool loopback = is_loopback_host(cracked->host);
    const bool secure = cracked->scheme == INTERNET_SCHEME_HTTPS;

    const UniqueInternet session(WinHttpOpen(
        options.user_agent.c_str(),
        loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) {
        return TransferError{.message = "WinHttpOpen failed", .win32_error = GetLastError()};
    }
    WinHttpSetTimeouts(
        session.get(),
        static_cast<int>(clamp_timeout(options.resolve_timeout)),
        static_cast<int>(clamp_timeout(options.connect_timeout)),
        static_cast<int>(clamp_timeout(options.send_timeout)),
        static_cast<int>(clamp_timeout(options.receive_timeout)));

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        // Windows 10 builds without TLS 1.3 in WinHTTP reject the combined mask.
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));

    const UniqueInternet connection(WinHttpConnect(session.get(), cracked->host.c_str(), cracked->port, 0));
    if (!connection) {
        return TransferError{.message = "WinHttpConnect failed", .win32_error = GetLastError()};
    }

    HINTERNET raw_request = WinHttpOpenRequest(
        connection.get(),
        L"GET",
        cracked->path_and_query.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (raw_request == nullptr) {
        return TransferError{.message = "WinHttpOpenRequest failed", .win32_error = GetLastError()};
    }

    // A stop request closes the request handle, which makes a blocked WinHTTP call return at once.
    std::mutex handle_mutex;
    HINTERNET request = raw_request;
    std::atomic<bool> cancelled{false};
    const auto close_request = [&]() noexcept {
        std::scoped_lock lock(handle_mutex);
        if (request != nullptr) {
            WinHttpCloseHandle(request);
            request = nullptr;
        }
    };
    std::optional<TransferError> result;
    {
        const std::stop_callback on_stop(stop, [&]() noexcept {
            cancelled.store(true);
            close_request();
        });

        const auto fail = [&](std::string message) {
            const DWORD code = GetLastError();
            if (cancelled.load()) {
                return TransferError{.message = "cancelled", .win32_error = ERROR_CANCELLED, .cancelled = true};
            }
            return TransferError{.message = std::move(message) + " (" + detail::win32_message(code) + ")", .win32_error = code};
        };

        if (spec.decompress) {
            DWORD flags = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(raw_request, WINHTTP_OPTION_DECOMPRESSION, &flags, sizeof(flags));
        }

        std::wstring headers;
        for (const auto& [name, value] : options.headers) {
            headers += name + L": " + value + L"\r\n";
        }
        if (spec.range_from.has_value() && *spec.range_from > 0) {
            headers += L"Range: bytes=" + std::to_wstring(*spec.range_from) + L"-\r\n";
            if (!spec.if_range.empty()) {
                headers += L"If-Range: " + core::utf8_to_wide(spec.if_range) + L"\r\n";
            }
        }
        if (!spec.decompress) {
            headers += L"Accept-Encoding: identity\r\n";
        }

        if (!WinHttpSendRequest(
                raw_request,
                headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                headers.empty() ? 0 : static_cast<DWORD>(-1L),
                WINHTTP_NO_REQUEST_DATA,
                0,
                0,
                0)) {
            result = fail("WinHttpSendRequest failed");
        } else if (!WinHttpReceiveResponse(raw_request, nullptr)) {
            result = fail("WinHttpReceiveResponse failed");
        } else {
            ResponseHead head;
            DWORD status = 0;
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(
                raw_request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &size,
                WINHTTP_NO_HEADER_INDEX);
            head.status = static_cast<int>(status);
            if (const auto v = query_header(raw_request, WINHTTP_QUERY_ETAG)) head.etag = core::wide_to_utf8(*v);
            if (const auto v = query_header(raw_request, WINHTTP_QUERY_LAST_MODIFIED)) head.last_modified = core::wide_to_utf8(*v);
            if (const auto v = query_header(raw_request, WINHTTP_QUERY_CONTENT_TYPE)) head.content_type = core::wide_to_utf8(*v);
            if (const auto v = query_header(raw_request, WINHTTP_QUERY_CONTENT_LENGTH)) head.content_length = parse_u64(*v);
            if (const auto v = query_header(raw_request, WINHTTP_QUERY_CONTENT_RANGE)) head.content_range = parse_content_range(*v);

            DWORD url_size = 0;
            WinHttpQueryOption(raw_request, WINHTTP_OPTION_URL, nullptr, &url_size);
            if (url_size > 0) {
                std::wstring final_url(url_size / sizeof(wchar_t), L'\0');
                if (WinHttpQueryOption(raw_request, WINHTTP_OPTION_URL, final_url.data(), &url_size)) {
                    final_url.resize(wcsnlen(final_url.c_str(), final_url.size()));
                    head.final_url = core::wide_to_utf8(final_url);
                }
            }
            if (head.final_url.empty()) {
                head.final_url = spec.url;
            }

            const std::string refused = check_url_allowed(head.final_url, options);
            if (!refused.empty()) {
                result = TransferError{.message = "redirect refused: " + refused};
            } else if (on_head(head)) {
                std::vector<std::uint8_t> buffer(256 * 1024);
                while (true) {
                    if (stop.stop_requested()) {
                        result = TransferError{.message = "cancelled", .win32_error = ERROR_CANCELLED, .cancelled = true};
                        break;
                    }
                    DWORD read = 0;
                    if (!WinHttpReadData(raw_request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
                        result = fail("WinHttpReadData failed");
                        break;
                    }
                    if (read == 0) {
                        break;
                    }
                    if (!on_chunk(buffer.data(), read)) {
                        result = TransferError{.message = "aborted by receiver"};
                        break;
                    }
                }
            }
        }
    } // on_stop unregistered here; it can no longer race with the close below.
    close_request();
    if (!result && cancelled.load()) {
        result = TransferError{.message = "cancelled", .win32_error = ERROR_CANCELLED, .cancelled = true};
    }
    return result;
}

class ProgressReporter {
public:
    ProgressReporter(const DownloadProgressFn& fn, std::chrono::milliseconds interval)
        : m_fn(fn)
        , m_interval(interval)
        , m_start(std::chrono::steady_clock::now()) {}

    void report(DownloadProgress progress, bool force) {
        if (!m_fn) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - m_last < m_interval) {
            return;
        }
        m_last = now;
        const double seconds = std::chrono::duration<double>(now - m_start).count();
        const std::uint64_t session = progress.received - std::min(progress.received, progress.resumed_from);
        progress.bytes_per_second = seconds > 0.0 ? static_cast<double>(session) / seconds : 0.0;
        try {
            m_fn(progress);
        } catch (...) {
            // A throwing progress callback must not abort or corrupt the transfer.
        }
    }

private:
    const DownloadProgressFn& m_fn;
    std::chrono::milliseconds m_interval;
    std::chrono::steady_clock::time_point m_start;
    std::chrono::steady_clock::time_point m_last{};
};

bool write_all(HANDLE file, const std::uint8_t* data, std::size_t size) {
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(file, data, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30)), &written, nullptr) || written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

std::filesystem::path with_suffix(const std::filesystem::path& path, std::wstring_view suffix) {
    std::filesystem::path out = path;
    out += suffix;
    return out;
}

DownloadResult finish_download(
    const std::filesystem::path& partial,
    const std::filesystem::path& meta,
    const std::filesystem::path& destination,
    DownloadResult result) {
    if (!MoveFileExW(partial.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.ok = false;
        result.win32_error = GetLastError();
        result.error = "could not move the finished download into place (" + detail::win32_message(result.win32_error) + ")";
        return result;
    }
    DeleteFileW(meta.c_str());
    result.ok = true;
    return result;
}

DownloadResult download_from_file_url(
    std::string_view url,
    const std::filesystem::path& destination,
    const DownloadProgressFn& on_progress,
    std::stop_token stop,
    const DownloadOptions& download_options) {
    DownloadResult result;
    const auto source_path = file_url_to_path(url);
    if (!source_path) {
        result.error = "malformed file:// URL";
        return result;
    }
    const UniqueHandle source = make_handle(CreateFileW(
        source_path->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!source) {
        result.win32_error = GetLastError();
        result.status = 404;
        result.error = "cannot open " + core::wide_to_utf8(source_path->wstring()) + " (" + detail::win32_message(result.win32_error) + ")";
        return result;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(source.get(), &size);
    const auto total = static_cast<std::uint64_t>(size.QuadPart);
    if (download_options.expected_size.has_value() && *download_options.expected_size != total) {
        result.error = "size mismatch: expected " + std::to_string(*download_options.expected_size) + ", source has " + std::to_string(total);
        return result;
    }

    const auto partial = with_suffix(destination, L".partial");
    const auto meta = with_suffix(destination, L".partial.json");
    std::uint64_t offset = 0;
    std::error_code ec;
    if (download_options.resume && std::filesystem::exists(partial, ec)) {
        PartialMeta saved;
        const auto text = detail::read_text_file(meta);
        if (text && !glz::read<glz::opts{.error_on_unknown_keys = false}>(saved, *text) && saved.url == url) {
            offset = std::filesystem::file_size(partial, ec);
            if (ec || offset > total) {
                offset = 0;
            }
        }
    }
    {
        std::string meta_json;
        (void)glz::write_json(PartialMeta{.url = std::string(url)}, meta_json);
        detail::write_text_file(meta, meta_json);
    }

    UniqueHandle out = make_handle(CreateFileW(
        partial.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, offset > 0 ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!out) {
        result.win32_error = GetLastError();
        result.error = "cannot create " + core::wide_to_utf8(partial.wstring());
        return result;
    }
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    SetFilePointerEx(out.get(), pos, nullptr, FILE_BEGIN);
    SetEndOfFile(out.get());
    SetFilePointerEx(source.get(), pos, nullptr, FILE_BEGIN);
    result.resumed = offset > 0;

    ProgressReporter progress(on_progress, download_options.progress_interval);
    DownloadProgress state{.received = offset, .total = total, .resumed_from = offset};
    std::vector<std::uint8_t> buffer(256 * 1024);
    while (true) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            result.error = "cancelled";
            result.bytes = state.received;
            return result;
        }
        DWORD read = 0;
        if (!ReadFile(source.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            result.win32_error = GetLastError();
            result.error = "read failed (" + detail::win32_message(result.win32_error) + ")";
            return result;
        }
        if (read == 0) {
            break;
        }
        if (!write_all(out.get(), buffer.data(), read)) {
            result.win32_error = GetLastError();
            result.error = "write failed (" + detail::win32_message(result.win32_error) + ")";
            return result;
        }
        state.received += read;
        progress.report(state, false);
    }
    progress.report(state, true);
    FlushFileBuffers(out.get());
    out.reset();
    result.status = 200;
    result.bytes = state.received;
    return finish_download(partial, meta, destination, result);
}

} // namespace

UrlInfo inspect_url(std::string_view url) {
    UrlInfo info;
    std::string_view lowered_scheme = url.substr(0, std::min<std::size_t>(url.size(), 8));
    std::string scheme;
    for (const char ch : lowered_scheme) {
        scheme.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (scheme.starts_with("file:")) {
        info.scheme = UrlScheme::File;
        return info;
    }
    const std::wstring wide = core::utf8_to_wide(url);
    const auto cracked = crack(wide);
    if (!cracked || cracked->host.empty()) {
        return info;
    }
    info.host = cracked->host;
    info.loopback = is_loopback_host(cracked->host);
    info.scheme = cracked->scheme == INTERNET_SCHEME_HTTPS ? UrlScheme::Https
                  : cracked->scheme == INTERNET_SCHEME_HTTP ? UrlScheme::Http
                                                             : UrlScheme::Invalid;
    return info;
}

std::string check_url_allowed(std::string_view url, const HttpOptions& options) {
    const UrlInfo info = inspect_url(url);
    switch (info.scheme) {
    case UrlScheme::Invalid:
        return "unsupported or malformed URL";
    case UrlScheme::File:
        return options.allow_file_urls ? std::string{} : std::string{"file:// URLs are not allowed"};
    case UrlScheme::Http:
        if (!options.require_https) {
            return {};
        }
        if (info.loopback && options.allow_insecure_loopback) {
            return {};
        }
        return "plain http:// is not allowed (HTTPS required)";
    case UrlScheme::Https:
        return {};
    }
    return "unsupported URL";
}

std::string resolve_url(std::string_view base, std::string_view reference) {
    if (reference.empty()) {
        return std::string(base);
    }
    if (inspect_url(reference).scheme != UrlScheme::Invalid) {
        return std::string(reference);
    }
    const std::wstring wide_base = core::utf8_to_wide(base);
    const std::wstring wide_ref = core::utf8_to_wide(reference);
    std::wstring out(4096, L'\0');
    DWORD len = static_cast<DWORD>(out.size());
    if (FAILED(UrlCombineW(wide_base.c_str(), wide_ref.c_str(), out.data(), &len, 0))) {
        return std::string(reference);
    }
    out.resize(len);
    return core::wide_to_utf8(out);
}

HttpResponse http_get(std::string_view url, const HttpOptions& options, std::stop_token stop) {
    HttpResponse response;
    if (const std::string refused = check_url_allowed(url, options); !refused.empty()) {
        response.error = refused;
        return response;
    }

    if (inspect_url(url).scheme == UrlScheme::File) {
        const auto path = file_url_to_path(url);
        if (!path) {
            response.error = "malformed file:// URL";
            return response;
        }
        auto text = detail::read_text_file(*path, options.max_body_bytes);
        if (!text) {
            response.status = 404;
            response.win32_error = GetLastError();
            response.error = "cannot read " + core::wide_to_utf8(path->wstring());
            return response;
        }
        response.ok = true;
        response.status = 200;
        response.body = std::move(*text);
        response.final_url = std::string(url);
        return response;
    }

    RequestSpec spec{.url = std::string(url), .decompress = true};
    bool too_large = false;
    const auto error = perform_http(
        spec,
        options,
        stop,
        [&](const ResponseHead& head) {
            response.status = head.status;
            response.etag = head.etag;
            response.last_modified = head.last_modified;
            response.content_type = head.content_type;
            response.final_url = head.final_url;
            return true;
        },
        [&](const std::uint8_t* data, std::size_t size) {
            if (response.body.size() + size > options.max_body_bytes) {
                too_large = true;
                return false;
            }
            response.body.append(reinterpret_cast<const char*>(data), size);
            return true;
        });
    if (too_large) {
        response.error = "response body exceeds max_body_bytes";
        return response;
    }
    if (error) {
        response.error = error->message;
        response.win32_error = error->win32_error;
        return response;
    }
    response.ok = response.status >= 200 && response.status < 300;
    if (!response.ok) {
        response.error = "HTTP status " + std::to_string(response.status);
    }
    return response;
}

DownloadResult download_file(
    std::string_view url,
    const std::filesystem::path& destination,
    const HttpOptions& options,
    const DownloadProgressFn& on_progress,
    std::stop_token stop,
    const DownloadOptions& download_options) {
    DownloadResult result;
    if (const std::string refused = check_url_allowed(url, options); !refused.empty()) {
        result.error = refused;
        return result;
    }
    std::error_code ec;
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), ec);
    }
    if (inspect_url(url).scheme == UrlScheme::File) {
        return download_from_file_url(url, destination, on_progress, stop, download_options);
    }

    const auto partial = with_suffix(destination, L".partial");
    const auto meta_path = with_suffix(destination, L".partial.json");

    for (int attempt = 0; attempt < 2; ++attempt) {
        std::uint64_t offset = 0;
        std::string validator;
        if (download_options.resume && std::filesystem::exists(partial, ec)) {
            PartialMeta saved;
            const auto text = detail::read_text_file(meta_path);
            if (text && !glz::read<glz::opts{.error_on_unknown_keys = false}>(saved, *text) && saved.url == url) {
                // Only a strong ETag or Last-Modified lets If-Range prove the server still has the same file.
                validator = !saved.etag.empty() && !saved.etag.starts_with("W/") ? saved.etag : saved.last_modified;
                if (!validator.empty()) {
                    offset = std::filesystem::file_size(partial, ec);
                    if (ec) {
                        offset = 0;
                    }
                }
            }
            if (offset == 0) {
                DeleteFileW(partial.c_str());
            }
        }

        UniqueHandle out;
        std::optional<std::uint64_t> total;
        bool retry_from_zero = false;
        std::string head_error;
        ProgressReporter progress(on_progress, download_options.progress_interval);
        DownloadProgress state{};

        RequestSpec spec{.url = std::string(url), .range_from = offset, .if_range = validator};
        const auto error = perform_http(
            spec,
            options,
            stop,
            [&](const ResponseHead& head) {
                result.status = head.status;
                std::uint64_t start = 0;
                if (head.status == 206 && offset > 0 && head.content_range.has_value() && head.content_range->start == offset) {
                    start = offset;
                    total = head.content_range->total;
                    result.resumed = true;
                } else if (head.status == 200 || head.status == 206) {
                    if (head.status == 206) {
                        // A range we did not ask for: start over without Range.
                        retry_from_zero = true;
                        return false;
                    }
                    total = head.content_length;
                } else if (head.status == 416 && offset > 0) {
                    retry_from_zero = true;
                    return false;
                } else {
                    head_error = "HTTP status " + std::to_string(head.status);
                    return false;
                }
                if (download_options.expected_size.has_value() && total.has_value() && *total != *download_options.expected_size) {
                    head_error = "size mismatch: expected " + std::to_string(*download_options.expected_size) + ", server reports " + std::to_string(*total);
                    return false;
                }

                std::string meta_json;
                (void)glz::write_json(PartialMeta{.url = std::string(url), .etag = head.etag, .last_modified = head.last_modified}, meta_json);
                detail::write_text_file(meta_path, meta_json);

                out = make_handle(CreateFileW(
                    partial.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, start > 0 ? OPEN_EXISTING : CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr));
                if (!out) {
                    head_error = "cannot create " + core::wide_to_utf8(partial.wstring()) + " (" + detail::win32_message(GetLastError()) + ")";
                    return false;
                }
                LARGE_INTEGER pos{};
                pos.QuadPart = static_cast<LONGLONG>(start);
                SetFilePointerEx(out.get(), pos, nullptr, FILE_BEGIN);
                SetEndOfFile(out.get());
                state = DownloadProgress{.received = start, .total = total, .resumed_from = start};
                progress.report(state, true);
                return true;
            },
            [&](const std::uint8_t* data, std::size_t size) {
                if (!write_all(out.get(), data, size)) {
                    head_error = "write failed (" + detail::win32_message(GetLastError()) + ")";
                    return false;
                }
                state.received += size;
                progress.report(state, false);
                return true;
            });

        if (retry_from_zero) {
            out.reset();
            DeleteFileW(partial.c_str());
            DeleteFileW(meta_path.c_str());
            continue;
        }
        result.bytes = state.received;
        if (!head_error.empty()) {
            result.error = head_error;
            return result;
        }
        if (error) {
            result.error = error->message;
            result.win32_error = error->win32_error;
            result.cancelled = error->cancelled;
            return result;
        }
        if (!out) {
            result.error = "no response body";
            return result;
        }
        progress.report(state, true);
        if (total.has_value() && state.received != *total) {
            result.error = "connection closed after " + std::to_string(state.received) + " of " + std::to_string(*total) + " bytes";
            return result;
        }
        FlushFileBuffers(out.get());
        out.reset();
        return finish_download(partial, meta_path, destination, result);
    }
    result.error = "server rejected the resume request twice";
    return result;
}

} // namespace boson::shipping
