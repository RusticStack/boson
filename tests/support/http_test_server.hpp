#pragma once

// Minimal threaded HTTP/1.1 server on 127.0.0.1 for tests and examples (no internet needed): keep-alive,
// Content-Length / chunked bodies, scripted delays and aborts, absolute-URI requests (proxy mode).
// Not hardened: test use only.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace boson::testing {

struct TestRequest {
    std::string method;
    std::string target; ///< "/path?q" or "http://host/path" (proxy requests)
    std::string path;   ///< target without scheme/host and without the query
    std::map<std::string, std::string> headers; ///< lower-case names
    std::string body;
    [[nodiscard]] std::string header(std::string_view name) const {
        std::string key(name);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto it = headers.find(key);
        return it == headers.end() ? std::string{} : it->second;
    }
};

struct TestResponse {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool chunked = false;
    std::size_t chunk_size = 16 * 1024;
    std::chrono::milliseconds delay_before{0};        ///< before the status line
    std::chrono::milliseconds delay_between_chunks{0}; ///< slow body
    bool close = false;
    /// Send this many body bytes, then reset the connection.
    std::optional<std::size_t> abort_after;
    /// Lie about Content-Length (e.g. to test size caps without sending the body).
    std::optional<std::uint64_t> content_length_override;
};

class HttpTestServer {
public:
    using Handler = std::function<TestResponse(const TestRequest&)>;

    HttpTestServer() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        int len = sizeof(addr);
        getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        listen(m_listen, 64);
        m_accept = std::jthread([this]() { accept_loop(); });
    }

    ~HttpTestServer() {
        m_stop = true;
        closesocket(m_listen);
        m_accept.join();
        {
            std::scoped_lock lock(m_mutex);
            for (const SOCKET s : m_clients) {
                shutdown(s, SD_BOTH);
            }
        }
        for (auto& t : m_workers) {
            if (t.joinable()) t.join();
        }
        WSACleanup();
    }

    HttpTestServer(const HttpTestServer&) = delete;
    HttpTestServer& operator=(const HttpTestServer&) = delete;

    void route(std::string path, Handler handler) {
        std::scoped_lock lock(m_mutex);
        m_routes[std::move(path)] = std::move(handler);
    }
    /// Fallback for unknown paths (e.g. proxy requests to other hosts).
    void fallback(Handler handler) {
        std::scoped_lock lock(m_mutex);
        m_fallback = std::move(handler);
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }
    [[nodiscard]] std::string base() const { return "http://127.0.0.1:" + std::to_string(m_port); }
    [[nodiscard]] std::string url(std::string_view path) const { return base() + std::string(path); }
    [[nodiscard]] int accepts() const noexcept { return m_accepts.load(); }
    [[nodiscard]] int requests() const noexcept { return m_requests.load(); }
    [[nodiscard]] TestRequest last_request() const {
        std::scoped_lock lock(m_mutex);
        return m_last;
    }

private:
    void accept_loop() {
        while (!m_stop) {
            SOCKET client = accept(m_listen, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                return;
            }
            ++m_accepts;
            std::scoped_lock lock(m_mutex);
            m_clients.push_back(client);
            m_workers.emplace_back([this, client]() { serve(client); });
        }
    }

    static bool send_all(SOCKET s, std::string_view data) {
        while (!data.empty()) {
            const int n = send(s, data.data(), static_cast<int>(std::min<std::size_t>(data.size(), 65536)), 0);
            if (n <= 0) return false;
            data.remove_prefix(static_cast<std::size_t>(n));
        }
        return true;
    }

    void sleep_for(std::chrono::milliseconds d) const {
        const auto end = std::chrono::steady_clock::now() + d;
        while (!m_stop && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void serve(SOCKET client) {
        std::string buffer;
        char chunk[8192];
        while (!m_stop) {
            std::size_t head_end;
            while ((head_end = buffer.find("\r\n\r\n")) == std::string::npos) {
                const int n = recv(client, chunk, sizeof(chunk), 0);
                if (n <= 0) {
                    closesocket(client);
                    return;
                }
                buffer.append(chunk, static_cast<std::size_t>(n));
            }
            TestRequest req;
            const std::string head = buffer.substr(0, head_end);
            buffer.erase(0, head_end + 4);
            const auto line_end = head.find("\r\n");
            const std::string line = head.substr(0, line_end);
            const auto sp1 = line.find(' ');
            const auto sp2 = line.find(' ', sp1 + 1);
            req.method = line.substr(0, sp1);
            req.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
            std::size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
            while (pos < head.size()) {
                auto e = head.find("\r\n", pos);
                if (e == std::string::npos) e = head.size();
                const std::string h = head.substr(pos, e - pos);
                pos = e + 2;
                const auto colon = h.find(':');
                if (colon == std::string::npos) continue;
                std::string name = h.substr(0, colon);
                for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::string value = h.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.erase(value.begin());
                req.headers[name] = value;
            }
            const std::size_t length = req.headers.contains("content-length") ? std::stoull(req.headers["content-length"]) : 0;
            while (buffer.size() < length) {
                const int n = recv(client, chunk, sizeof(chunk), 0);
                if (n <= 0) {
                    closesocket(client);
                    return;
                }
                buffer.append(chunk, static_cast<std::size_t>(n));
            }
            req.body = buffer.substr(0, length);
            buffer.erase(0, length);
            req.path = req.target;
            if (req.path.starts_with("http://")) {
                const auto slash = req.path.find('/', 7);
                req.path = slash == std::string::npos ? "/" : req.path.substr(slash);
            }
            if (const auto q = req.path.find('?'); q != std::string::npos) {
                req.path.resize(q);
            }
            ++m_requests;
            Handler handler;
            {
                std::scoped_lock lock(m_mutex);
                m_last = req;
                if (const auto it = m_routes.find(req.path); it != m_routes.end()) {
                    handler = it->second;
                } else {
                    handler = m_fallback;
                }
            }
            TestResponse resp = handler ? handler(req) : TestResponse{.status = 404, .body = "not found"};
            if (resp.delay_before.count() > 0) {
                sleep_for(resp.delay_before);
            }
            std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " X\r\n";
            for (const auto& [n, v] : resp.headers) {
                out += n + ": " + v + "\r\n";
            }
            if (resp.chunked) {
                out += "Transfer-Encoding: chunked\r\n";
            } else {
                out += "Content-Length: " + std::to_string(resp.content_length_override.value_or(resp.body.size())) + "\r\n";
            }
            if (resp.close) {
                out += "Connection: close\r\n";
            }
            out += "\r\n";
            if (!send_all(client, out)) {
                closesocket(client);
                return;
            }
            const bool head_only = req.method == "HEAD";
            std::size_t sent = 0;
            const std::size_t limit = resp.abort_after.value_or(resp.body.size());
            bool ok = true;
            while (!head_only && ok && sent < std::min(limit, resp.body.size())) {
                const std::size_t n = std::min({resp.chunk_size, resp.body.size() - sent, limit - sent});
                const std::string_view piece(resp.body.data() + sent, n);
                if (resp.chunked) {
                    char size_line[32];
                    std::snprintf(size_line, sizeof(size_line), "%zx\r\n", n);
                    ok = send_all(client, size_line) && send_all(client, piece) && send_all(client, "\r\n");
                } else {
                    ok = send_all(client, piece);
                }
                sent += n;
                if (resp.delay_between_chunks.count() > 0) {
                    sleep_for(resp.delay_between_chunks);
                    if (m_stop) ok = false;
                }
            }
            if (resp.abort_after) {
                linger l{1, 0};
                setsockopt(client, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
                closesocket(client);
                return;
            }
            if (ok && resp.chunked && !head_only) {
                ok = send_all(client, "0\r\n\r\n");
            }
            if (!ok || resp.close || resp.content_length_override) {
                shutdown(client, SD_SEND);
                closesocket(client);
                return;
            }
        }
        closesocket(client);
    }

    SOCKET m_listen = INVALID_SOCKET;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_accepts{0};
    std::atomic<int> m_requests{0};
    mutable std::mutex m_mutex;
    std::map<std::string, Handler> m_routes;
    Handler m_fallback;
    TestRequest m_last;
    std::vector<SOCKET> m_clients;
    std::vector<std::jthread> m_workers;
    std::jthread m_accept;
};

} // namespace boson::testing
