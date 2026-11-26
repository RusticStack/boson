#pragma once

#include <boson/crypto/hash.hpp>
#include <boson/exec/task_runner.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/export.hpp>
#include <boson/net/http_client.hpp>
#include <boson/security/common.hpp>
#include <boson/security/scoped_fs.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace boson::ipc {

struct HttpControllerOptions {
    /// Hosts the page may reach: "api.example.com" (exact) or "*.example.com" (any sub-domain). Default deny:
    /// an empty list refuses everything. Checked on every redirect hop.
    std::vector<std::string> allowed_hosts;
    /// Client used for every request (proxy, timeouts, retries, cache). url_filter is replaced by the allow-list.
    net::HttpClientOptions client;
    /// Largest body fetch() returns inline (JSON IPC); bigger responses must use download().
    std::uint64_t max_fetch_bytes = 8ull * 1024 * 1024;
    std::size_t max_parallel_downloads = 4;
};

struct HttpFetchRequest {
    std::string method = "GET";
    std::string url;
    std::vector<net::HttpHeader> headers;
    std::string body;
    std::optional<std::uint32_t> timeout_ms;
    /// Return the body base64 encoded (binary responses).
    bool base64 = false;
    bool use_cache = true;
};

struct HttpFetchResponse {
    int status = 0;
    bool ok = false;
    std::vector<net::HttpHeader> headers;
    std::string body;
    bool base64 = false;
    std::string final_url;
    std::string protocol;
    std::uint32_t redirects = 0;
    std::uint32_t attempts = 0;
    bool from_cache = false;
};

struct HttpDownloadRequest {
    std::string url;
    /// Destination inside a ScopedFs root (write access required).
    std::string path;
    std::vector<net::HttpHeader> headers;
    /// "md5" | "sha1" | "sha256" | "sha384" | "sha512" (empty: inferred from expected_hash, or none).
    std::string hash;
    std::string expected_hash;
    bool overwrite = true;
    std::optional<std::uint32_t> timeout_ms;
};

struct HttpDownloadStarted {
    std::uint64_t id = 0;
};

struct HttpIdRequest {
    std::uint64_t id = 0;
};

struct HttpBoolResponse {
    bool value = false;
};

struct HttpHostsResponse {
    std::vector<std::string> hosts;
};

/// Payload of "http:progress".
struct HttpProgressEvent {
    std::uint64_t id = 0;
    std::uint64_t received = 0;
    std::optional<std::uint64_t> total;
    double fraction = 0.0;
    double bytes_per_second = 0.0;
};

/// Payload of "http:done" (success, failure and cancellation).
struct HttpDoneEvent {
    std::uint64_t id = 0;
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::string error_kind;
    int status = 0;
    std::string path;
    std::uint64_t bytes = 0;
    std::string digest;
};

/// @brief Events of HttpController, for TsGenerator::EventMaps.
struct HttpEvents {
    HttpProgressEvent http_progress;
    HttpDoneEvent http_done;
};

/// @brief True when host matches an allow-list entry ("host" exact, "*.domain" any sub-domain), ASCII
/// case-insensitive.
[[nodiscard]] inline bool host_allowed(std::string_view host, const std::vector<std::string>& allowed) {
    const auto lower = [](std::string_view s) {
        std::string out(s);
        for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    const std::string h = lower(host);
    if (h.empty()) {
        return false;
    }
    for (const auto& entry_raw : allowed) {
        const std::string entry = lower(entry_raw);
        if (entry.starts_with("*.")) {
            const std::string_view suffix = std::string_view(entry).substr(1); // ".domain"
            if (h.size() > suffix.size() && h.ends_with(suffix)) {
                return true;
            }
        } else if (h == entry) {
            return true;
        }
    }
    return false;
}

/// @brief JS "Http": fetch() for small JSON / text / base64 responses and download() to a ScopedFs path with
/// "http:progress" / "http:done" events. Every URL, including each redirect hop, must be on the host
/// allow-list (default deny). fetch() runs on the router's pool thread; downloads run on the TaskRunner.
class HttpController {
public:
    BOSON_CONTROLLER("Http");

    HttpController(Bridge& bridge, exec::TaskRunner& runner, std::shared_ptr<security::ScopedFs> fs, HttpControllerOptions options = {})
        : m_bridge(bridge), m_runner(runner), m_fs(std::move(fs)), m_shared(std::make_shared<Shared>()) {
        m_shared->allowed = std::move(options.allowed_hosts);
        m_shared->max_fetch = options.max_fetch_bytes;
        m_shared->max_parallel = options.max_parallel_downloads;
        auto client_options = std::move(options.client);
        client_options.url_filter = [shared = std::weak_ptr<Shared>(m_shared)](std::string_view url) {
            const auto s = shared.lock();
            if (!s) {
                return false;
            }
            std::scoped_lock lock(s->mutex);
            return host_allowed(net::url_host(url), s->allowed);
        };
        m_shared->client = std::make_unique<net::HttpClient>(std::move(client_options));
    }

    ~HttpController() {
        std::scoped_lock lock(m_shared->mutex);
        for (auto& [id, stop] : m_shared->downloads) {
            stop.request_stop();
        }
    }

    HttpController(const HttpController&) = delete;
    HttpController& operator=(const HttpController&) = delete;

    /// @brief C++ only: grants a host at run time (e.g. a mirror chosen by the user).
    void allow_host(std::string host) {
        std::scoped_lock lock(m_shared->mutex);
        m_shared->allowed.push_back(std::move(host));
    }

    [[nodiscard]] net::HttpClient& client() noexcept { return *m_shared->client; }

    [[nodiscard]] HttpHostsResponse allowed_hosts() const {
        std::scoped_lock lock(m_shared->mutex);
        return HttpHostsResponse{.hosts = m_shared->allowed};
    }

    [[nodiscard]] HttpFetchResponse fetch(HttpFetchRequest req) {
        const auto method = net::parse_http_method(req.method);
        if (!method) {
            throw std::runtime_error("unsupported HTTP method: " + req.method);
        }
        net::HttpRequest request{.method = *method, .url = std::move(req.url), .headers = std::move(req.headers), .body = std::move(req.body)};
        if (req.timeout_ms) {
            request.timeout = std::chrono::milliseconds{*req.timeout_ms};
        }
        request.max_body_bytes = m_shared->max_fetch;
        request.use_cache = req.use_cache;
        auto result = m_shared->client->send(std::move(request));
        if (!result) {
            throw std::runtime_error(std::string(net::to_string(result.error().kind)) + ": " + result.error().message);
        }
        HttpFetchResponse out{
            .status = result->status,
            .ok = result->ok(),
            .headers = std::move(result->headers),
            .body = {},
            .base64 = req.base64,
            .final_url = std::move(result->final_url),
            .protocol = std::move(result->protocol),
            .redirects = result->redirects,
            .attempts = result->attempts,
            .from_cache = result->from_cache,
        };
        out.body = req.base64 ? security::base64_encode(security::as_bytes(result->body)) : std::move(result->body);
        return out;
    }

    [[nodiscard]] HttpDownloadStarted download(HttpDownloadRequest req) {
        if (!m_fs) {
            throw std::runtime_error("Http.download needs a ScopedFs");
        }
        auto target = m_fs->scope().resolve_entry(req.path, security::FsAccess::Write);
        if (!target) {
            throw std::runtime_error(target.error().message);
        }
        if (target->read_only) {
            throw std::runtime_error("destination is read-only: " + req.path);
        }
        if (!host_allowed(net::url_host(req.url), allowed_hosts().hosts)) {
            throw std::runtime_error("host not allowed: " + net::url_host(req.url));
        }
        net::DownloadOptions options;
        options.overwrite = req.overwrite;
        options.expected_hash = req.expected_hash;
        if (!req.hash.empty()) {
            options.hash = crypto::parse_hash_algorithm(req.hash);
            if (!options.hash) {
                throw std::runtime_error("unknown hash algorithm: " + req.hash);
            }
        }
        std::uint64_t id = 0;
        std::stop_token token;
        {
            std::scoped_lock lock(m_shared->mutex);
            if (m_shared->downloads.size() >= m_shared->max_parallel) {
                throw std::runtime_error("too many downloads in progress");
            }
            id = ++m_shared->next_id;
            token = m_shared->downloads[id].get_token();
        }
        net::HttpRequest request{.method = net::HttpMethod::Get, .url = std::move(req.url), .headers = std::move(req.headers)};
        request.stop = token;
        if (req.timeout_ms) {
            request.timeout = std::chrono::milliseconds{*req.timeout_ms};
        }
        m_runner.spawn([&bridge = m_bridge, shared = m_shared, id, request = std::move(request), options = std::move(options),
                        dest = std::filesystem::path(target->win32), display = target->display]() mutable {
            options.on_progress = [&bridge, id](const net::TransferProgress& p) {
                bridge.emit("http:progress", HttpProgressEvent{
                    .id = id,
                    .received = p.received,
                    .total = p.total,
                    .fraction = p.total && *p.total > 0 ? static_cast<double>(p.received) / static_cast<double>(*p.total) : 0.0,
                    .bytes_per_second = p.bytes_per_second,
                });
            };
            auto result = shared->client->download(std::move(request), dest, options);
            HttpDoneEvent done{.id = id, .path = display};
            if (result) {
                done.ok = true;
                done.status = result->response.status;
                done.bytes = result->bytes;
                done.digest = result->digest ? result->digest->hex() : std::string{};
            } else {
                done.cancelled = result.error().kind == net::HttpErrorKind::Cancelled;
                done.error = result.error().message;
                done.error_kind = std::string(net::to_string(result.error().kind));
                done.status = result.error().status;
            }
            {
                std::scoped_lock lock(shared->mutex);
                shared->downloads.erase(id);
            }
            bridge.emit("http:done", done);
        });
        return HttpDownloadStarted{.id = id};
    }

    [[nodiscard]] HttpBoolResponse cancel(HttpIdRequest req) {
        std::scoped_lock lock(m_shared->mutex);
        const auto it = m_shared->downloads.find(req.id);
        if (it == m_shared->downloads.end()) {
            return HttpBoolResponse{.value = false};
        }
        it->second.request_stop();
        return HttpBoolResponse{.value = true};
    }

private:
    struct Shared {
        mutable std::mutex mutex;
        std::vector<std::string> allowed;
        std::map<std::uint64_t, std::stop_source> downloads;
        std::uint64_t next_id = 0;
        std::uint64_t max_fetch = 0;
        std::size_t max_parallel = 4;
        std::unique_ptr<net::HttpClient> client;
    };

    Bridge& m_bridge;
    exec::TaskRunner& m_runner;
    std::shared_ptr<security::ScopedFs> m_fs;
    std::shared_ptr<Shared> m_shared;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::HttpController> {
    using T = boson::ipc::HttpController;
    static constexpr auto value = glz::object(
        "fetch", &T::fetch,
        "download", &T::download,
        "cancel", &T::cancel,
        "allowed_hosts", &T::allowed_hosts);
};

template <>
struct glz::meta<boson::ipc::HttpEvents> {
    using T = boson::ipc::HttpEvents;
    static constexpr auto value = glz::object(
        "http:progress", &T::http_progress,
        "http:done", &T::http_done);
};
