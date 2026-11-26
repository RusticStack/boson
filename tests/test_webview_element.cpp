// <boson-webview> / guest / CDP tests: headless unit checks plus checks against the installed WebView2 runtime
// (skipped when no runtime is installed). Opens one small non-activated window and a loopback HTTP server.
//
// Optional: BOSON_PLAYWRIGHT_DIR=<folder with node_modules/playwright-core> runs tests/js/playwright_cdp.mjs
// against the CdpServer (skipped with a message otherwise).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

/// Element rect as measured by the host page (glaze needs a type with external linkage).
namespace we_test {
struct JsRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    double dpr = 1;
};
} // namespace we_test

namespace {

using we_test::JsRect;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_webview_element.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define WE_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

bool contains(const char* haystack, std::string_view needle) {
    return std::string_view(haystack).find(needle) != std::string_view::npos;
}

bool contains(const std::optional<std::string>& haystack, std::string_view needle) {
    return haystack.has_value() && haystack->find(needle) != std::string::npos;
}

bool rect_eq(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool close_to(LONG a, LONG b, LONG tolerance = 1) {
    return std::abs(a - b) <= tolerance;
}

template <typename Fn>
double ns_per_op(std::size_t iterations, Fn&& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        fn(i);
    }
    const auto t1 = std::chrono::steady_clock::now();
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / static_cast<double>(iterations);
}

// ---- Headless unit checks ------------------------------------------------------------------------------

void unit_tests() {
    using namespace boson::webview;
    step("unit: element placement (DPR, rounding, clip, visibility)");
    {
        const POINT origin{10, 20};
        auto p = compute_element_placement(CssRect{.x = 5, .y = 7, .width = 100, .height = 50}, std::nullopt, 1.0, origin, true);
        WE_CHECK(rect_eq(p.bounds, RECT{15, 27, 115, 77}));
        WE_CHECK(p.visible && !p.clipped);
        // 1.5x: each edge is rounded on its own, so adjacent elements never overlap or leave a gap.
        p = compute_element_placement(CssRect{.x = 0.5, .y = 0.5, .width = 99.5, .height = 33.3}, std::nullopt, 1.5, POINT{0, 0}, true);
        WE_CHECK(rect_eq(p.bounds, RECT{1, 1, 150, 51}));
        const auto left = compute_element_placement(CssRect{.x = 0, .y = 0, .width = 33.3, .height = 10}, std::nullopt, 1.25, POINT{0, 0}, true);
        const auto right = compute_element_placement(CssRect{.x = 33.3, .y = 0, .width = 33.3, .height = 10}, std::nullopt, 1.25, POINT{0, 0}, true);
        WE_CHECK(left.bounds.right == right.bounds.left);
        // Clip: top half scrolled out of a container.
        p = compute_element_placement(CssRect{.x = 0, .y = -50, .width = 200, .height = 100},
                                      CssRect{.x = 0, .y = 0, .width = 200, .height = 50}, 2.0, POINT{0, 0}, true);
        WE_CHECK(p.visible && p.clipped);
        WE_CHECK(rect_eq(p.bounds, RECT{0, -100, 400, 100}));
        WE_CHECK(rect_eq(p.clip, RECT{0, 100, 400, 200}));
        // A clip that covers the whole element is no clip.
        p = compute_element_placement(CssRect{.x = 0, .y = 0, .width = 10, .height = 10}, CssRect{.x = -5, .y = -5, .width = 100, .height = 100}, 1.0, POINT{0, 0}, true);
        WE_CHECK(p.visible && !p.clipped);
        // Fully clipped away, zero size, hidden.
        p = compute_element_placement(CssRect{.x = 0, .y = 0, .width = 10, .height = 10}, CssRect{.x = 20, .y = 20, .width = 5, .height = 5}, 1.0, POINT{0, 0}, true);
        WE_CHECK(!p.visible);
        WE_CHECK(!compute_element_placement(CssRect{.x = 0, .y = 0, .width = 0, .height = 10}, std::nullopt, 1.0, POINT{0, 0}, true).visible);
        WE_CHECK(!compute_element_placement(CssRect{.x = 0, .y = 0, .width = 10, .height = 10}, std::nullopt, 1.0, POINT{0, 0}, false).visible);
        // Bad DPR falls back to 1.
        p = compute_element_placement(CssRect{.x = 1, .y = 2, .width = 3, .height = 4}, std::nullopt, std::nan(""), POINT{0, 0}, true);
        WE_CHECK(rect_eq(p.bounds, RECT{1, 2, 4, 6}));
        WE_CHECK(p == compute_element_placement(CssRect{.x = 1, .y = 2, .width = 3, .height = 4}, std::nullopt, 0.0, POINT{0, 0}, true));
    }

    step("unit: partitions and policies");
    {
        auto d = parse_partition("");
        WE_CHECK(d.has_value() && d->profile_name.empty() && !d->in_private);
        auto persist = parse_partition("persist:Work");
        WE_CHECK(persist.has_value() && persist->profile_name == L"Work" && !persist->in_private);
        auto mem = parse_partition("scratch");
        WE_CHECK(mem.has_value() && mem->profile_name == L"scratch" && mem->in_private);
        auto anon = parse_partition("inprivate");
        WE_CHECK(anon.has_value() && anon->profile_name.empty() && anon->in_private);
        WE_CHECK(!parse_partition("persist:").has_value());
        WE_CHECK(!parse_partition("bad/name").has_value());
        WE_CHECK(!parse_partition("trailing.").has_value());
        WE_CHECK(parse_popup_policy("guest") == std::optional{PopupPolicy::Guest});
        WE_CHECK(parse_popup_policy("") == std::optional{PopupPolicy::Deny});
        WE_CHECK(!parse_popup_policy("tab").has_value());
        WE_CHECK(parse_prompt_policy("ask") == std::optional{PromptPolicy::Ask});
        WE_CHECK(!parse_prompt_policy("maybe").has_value());
        for (const auto p : {PopupPolicy::Deny, PopupPolicy::Guest, PopupPolicy::Window}) {
            WE_CHECK(parse_popup_policy(to_string(p)) == std::optional{p});
        }
        for (const auto p : {PromptPolicy::Default, PromptPolicy::Deny, PromptPolicy::Allow, PromptPolicy::Ask}) {
            WE_CHECK(parse_prompt_policy(to_string(p)) == std::optional{p});
        }
    }

    step("unit: GuestCreateRequest -> GuestOptions");
    {
        boson::ipc::GuestCreateRequest req;
        req.src = "https://example.com/";
        req.allowpopups = true;
        req.reserved_accelerators = {"Ctrl+W", "F5"};
        const auto o = boson::ipc::to_guest_options(req, nullptr);
        WE_CHECK(o.popups == PopupPolicy::Guest);
        WE_CHECK(o.reserved_accelerators.size() == 2);
        WE_CHECK(o.certificate_errors == PromptPolicy::Deny);
        const auto throws = [](boson::ipc::GuestCreateRequest r) {
            try {
                (void)boson::ipc::to_guest_options(r, nullptr);
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        boson::ipc::GuestCreateRequest bad = req;
        bad.partition = "a/b";
        WE_CHECK(throws(bad));
        bad = req;
        bad.zoom = 0.0;
        WE_CHECK(throws(bad));
        bad = req;
        bad.popups = "tabs";
        WE_CHECK(throws(bad));
        bad = req;
        bad.reserved_accelerators = {"Ctrl+Nope"};
        WE_CHECK(throws(bad));
        bad = req;
        bad.preload = "boson://app/preload.js"; // no resolver: rejected
        WE_CHECK(throws(bad));
    }

    step("unit: accelerator forwarding and DOM key names");
    {
        using boson::menu::KeyModifiers;
        WE_CHECK(dom_key_of('T', false) == "t");
        WE_CHECK(dom_key_of('T', true) == "T");
        WE_CHECK(dom_key_of(VK_F5, false) == "F5");
        WE_CHECK(dom_key_of(VK_LEFT, false) == "ArrowLeft");
        WE_CHECK(dom_key_of(VK_OEM_PLUS, true) == "+");
        WE_CHECK(dom_code_of('T') == "KeyT");
        WE_CHECK(dom_code_of('1') == "Digit1");
        WE_CHECK(dom_code_of(VK_F12) == "F12");
        WE_CHECK(is_forwarded_accelerator('T', KeyModifiers::Ctrl));
        WE_CHECK(is_forwarded_accelerator(VK_F5, KeyModifiers::None));
        WE_CHECK(is_forwarded_accelerator('1', KeyModifiers::Alt));
        WE_CHECK(!is_forwarded_accelerator('A', KeyModifiers::None));
        WE_CHECK(!is_forwarded_accelerator('A', KeyModifiers::Shift));
        WE_CHECK(!is_forwarded_accelerator(VK_CONTROL, KeyModifiers::Ctrl));
    }

    step("unit: guest event names");
    {
        WE_CHECK(guest_event_name(GuestEvent{GuestDidNavigate{}}) == "guest:did-navigate");
        WE_CHECK(guest_event_name(GuestEvent{GuestIpcMessage{}}) == "guest:ipc-message");
        WE_CHECK(guest_event_name(GuestEvent{GuestDownload{.phase = "started"}}) == "guest:download-started");
        WE_CHECK(guest_event_name(GuestEvent{GuestDownload{.phase = "updated"}}) == "guest:download-updated");
        WE_CHECK(guest_event_name(GuestEvent{GuestDownload{.phase = "done"}}) == "guest:download-done");
        std::string json;
        WE_CHECK(!glz::write_json(GuestIpcMessage{.id = 3, .channel = "c", .args = glz::raw_json{"[1,\"x\"]"}}, json));
        WE_CHECK(json == R"({"id":3,"channel":"c","args":[1,"x"]})");
        WE_CHECK(Guest::guest_runtime_script().find(L"sendToHost") != std::wstring::npos);
    }

    step("unit: CDP domain table");
    {
        WE_CHECK(cdp_domain_of("Network.requestWillBeSent") == "Network");
        WE_CHECK(cdp_domain_of("nodot").empty());
        const auto page = cdp_domain_events("Page");
        WE_CHECK(std::ranges::find(page, std::string_view("Page.loadEventFired")) != page.end());
        WE_CHECK(cdp_domain_events("NoSuchDomain").empty());
        const auto domains = cdp_known_domains();
        WE_CHECK(domains.size() > 10);
        WE_CHECK(std::ranges::is_sorted(domains));
        WE_CHECK(CdpServer::target_id_of(0x1234).size() == 32);
        WE_CHECK(CdpServer::target_id_of(1) != CdpServer::target_id_of(2));
    }

    step("unit: WebSocket frames and handshake");
    {
        using namespace boson::webview::ws;
        WE_CHECK(accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="); // RFC 6455 section 1.3
        for (const std::size_t size : {std::size_t{0}, std::size_t{125}, std::size_t{126}, std::size_t{65535}, std::size_t{65536}, std::size_t{300000}}) {
            std::string payload(size, 'x');
            for (std::size_t i = 0; i < size; ++i) {
                payload[i] = static_cast<char>('a' + (i % 26));
            }
            std::string wire;
            append_frame(wire, Opcode::Text, payload, true, 0xA1B2C3D4u);
            Frame f;
            auto r = parse_frame(wire, f, 1u << 20, true);
            WE_CHECK(r.status == ParseStatus::Complete && r.consumed == wire.size() && f.payload == payload && f.opcode == Opcode::Text && f.fin);
            // Every prefix is incomplete.
            if (!wire.empty()) {
                Frame partial;
                WE_CHECK(parse_frame(std::string_view(wire).substr(0, wire.size() - 1), partial, 1u << 20, true).status == ParseStatus::NeedMore);
            }
            std::string server_wire;
            append_frame(server_wire, Opcode::Text, payload);
            Frame g;
            WE_CHECK(parse_frame(server_wire, g, 1u << 20, true).status == ParseStatus::Error); // clients must mask
            WE_CHECK(parse_frame(server_wire, g, 1u << 20, false).status == ParseStatus::Complete && g.payload == payload);
        }
        std::string big;
        append_frame(big, Opcode::Binary, std::string(2000, 'z'), true, 1u);
        Frame f;
        WE_CHECK(parse_frame(big, f, 1000, true).status == ParseStatus::Error); // over the limit
        std::string two;
        append_frame(two, Opcode::Text, "one", true, 7u);
        append_frame(two, Opcode::Ping, "p", true, 9u);
        auto r1 = parse_frame(two, f, 1000, true);
        WE_CHECK(r1.status == ParseStatus::Complete && f.payload == "one");
        auto r2 = parse_frame(std::string_view(two).substr(r1.consumed), f, 1000, true);
        WE_CHECK(r2.status == ParseStatus::Complete && f.opcode == Opcode::Ping && f.payload == "p");
        std::string reserved = "\xC1\x80";
        reserved += std::string(4, '\0');
        WE_CHECK(parse_frame(reserved, f, 1000, true).status == ParseStatus::Error); // RSV1 set
    }

    step("unit: perf (placement math, frame parsing)");
    {
        volatile LONG sink = 0;
        const double placement_ns = ns_per_op(2'000'000, [&](std::size_t i) {
            const double d = static_cast<double>(i & 255);
            const auto p = compute_element_placement(CssRect{.x = d, .y = d * 0.5, .width = 400, .height = 300},
                                                     CssRect{.x = 0, .y = 20, .width = 600, .height = 400}, 1.5, POINT{8, 40}, true);
            sink = sink + p.bounds.left;
        });
        std::string frame;
        ws::append_frame(frame, ws::Opcode::Text, std::string(1024, 'q'), true, 0x01020304u);
        ws::Frame out;
        const double parse_ns = ns_per_op(200'000, [&](std::size_t) {
            const auto r = ws::parse_frame(frame, out, 1u << 20, true);
            sink = sink + static_cast<LONG>(r.consumed);
        });
        std::cout << "  compute_element_placement: " << placement_ns << " ns/op\n";
        std::cout << "  ws::parse_frame (1 KiB masked): " << parse_ns << " ns/frame (" << (1024.0 / parse_ns * 1000.0) << " MB/s)\n";
        (void)sink;
    }
}

// ---- Loopback HTTP server (one thread per connection: downloads trickle while pages load) -----------------

constexpr std::size_t kDownloadSize = 300000;

class LoopbackServer {
public:
    LoopbackServer() {
        m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listen == INVALID_SOCKET) {
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(m_listen, 32) != 0) {
            return;
        }
        int len = sizeof(addr);
        getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        m_thread = std::thread([this]() { run(); });
    }

    ~LoopbackServer() {
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
        std::vector<std::thread> workers;
        {
            std::scoped_lock lock(m_mutex);
            workers.swap(m_workers);
        }
        for (auto& w : workers) {
            w.join();
        }
        if (m_listen != INVALID_SOCKET) {
            closesocket(m_listen);
        }
    }

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }
    [[nodiscard]] int slow_bytes_sent_kb() const noexcept { return m_slow_kb.load(); }

private:
    void run() {
        while (!m_stop) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(m_listen, &set);
            timeval tv{0, 100 * 1000};
            if (select(0, &set, nullptr, nullptr, &tv) <= 0) {
                continue;
            }
            SOCKET client = accept(m_listen, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                continue;
            }
            std::scoped_lock lock(m_mutex);
            m_workers.emplace_back([this, client]() {
                handle(client);
                closesocket(client);
            });
        }
    }

    static std::string page(std::string_view title, std::string_view body = {}) {
        return "<!doctype html><html><head><meta charset=utf-8><title>" + std::string(title) + "</title></head><body>" +
               (body.empty() ? std::string(title) : std::string(body)) + "</body></html>";
    }

    static std::string ok(std::string_view body, std::string_view type = "text/html", std::string_view extra = {}) {
        return "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(type) + "\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nCache-Control: no-store\r\nConnection: close\r\n" + std::string(extra) + "\r\n" + std::string(body);
    }

    void handle(SOCKET client) {
        DWORD timeout_ms = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 64 * 1024) {
            const int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) {
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        const std::string_view line = std::string_view(request).substr(0, request.find("\r\n"));
        const std::size_t sp1 = line.find(' ');
        const std::size_t sp2 = line.find(' ', sp1 + 1);
        const std::string path(line.substr(sp1 + 1, sp2 - sp1 - 1));
        std::string response;
        if (path == "/guest") {
            response = ok(page("Guest", "<h1>guest</h1><script>"
                                        "window.addEventListener('load',()=>{if(window.bosonGuest){bosonGuest.on('ping',(e,n)=>{document.title='pong'+n;});}});"
                                        "</script>"));
        } else if (path == "/pw") {
            response = ok(page("PW", "<h1 id=h>Hello Playwright</h1><input id=name><button id=go "
                                     "onclick=\"document.getElementById('out').textContent='hi '+document.getElementById('name').value\">go</button>"
                                     "<p id=out></p>"));
        } else if (path.starts_with("/page/")) {
            response = ok(page(path.substr(6)));
        } else if (path == "/auth") {
            if (request.find("Authorization: Basic dXNlcjpwYXNz") != std::string::npos) { // user:pass
                response = ok(page("Authed"));
            } else {
                const std::string body = page("Denied");
                response = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"boson\"\r\nContent-Type: text/html\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            }
        } else if (path == "/download") {
            const std::string body(kDownloadSize, 'D');
            response = ok(body, "application/octet-stream", "Content-Disposition: attachment; filename=\"boson-dl.bin\"\r\n");
        } else if (path == "/slow") {
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 104857600\r\n"
                                     "Content-Disposition: attachment; filename=\"boson-slow.bin\"\r\nConnection: close\r\n\r\n";
            if (send(client, head.data(), static_cast<int>(head.size()), 0) <= 0) {
                return;
            }
            const std::string chunk(16 * 1024, 'S');
            for (int i = 0; i < 6400 && !m_stop; ++i) {
                if (send(client, chunk.data(), static_cast<int>(chunk.size()), 0) <= 0) {
                    return; // cancelled: the browser closed the connection
                }
                m_slow_kb += 16;
                Sleep(20);
            }
            return;
        } else {
            response = ok(page("Loopback"));
        }
        send(client, response.data(), static_cast<int>(response.size()), 0);
        shutdown(client, SD_SEND);
    }

    SOCKET m_listen = INVALID_SOCKET;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_slow_kb{0};
    std::mutex m_mutex;
    std::vector<std::thread> m_workers;
    std::thread m_thread;
};

// ---- Minimal blocking CDP client (HTTP + WebSocket), run on a worker thread -------------------------------------

class TestCdpClient {
public:
    explicit TestCdpClient(std::uint16_t port) : m_port(port) {}
    ~TestCdpClient() { close(); }
    TestCdpClient(const TestCdpClient&) = delete;
    TestCdpClient& operator=(const TestCdpClient&) = delete;

    /// Plain HTTP GET; returns "<status> <body>".
    std::string http_get(std::string_view path) {
        SOCKET s = connect_socket();
        if (s == INVALID_SOCKET) {
            return "connect failed";
        }
        const std::string req = "GET " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(m_port) + "\r\nConnection: close\r\n\r\n";
        send(s, req.data(), static_cast<int>(req.size()), 0);
        std::string resp;
        char buf[8192];
        for (;;) {
            const int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            resp.append(buf, static_cast<std::size_t>(n));
        }
        closesocket(s);
        const std::size_t sp = resp.find(' ');
        const std::size_t body = resp.find("\r\n\r\n");
        if (sp == std::string::npos || body == std::string::npos) {
            return "bad response";
        }
        return resp.substr(sp + 1, 3) + " " + resp.substr(body + 4);
    }

    bool open_websocket(std::string_view path, std::string* accept_header = nullptr) {
        m_socket = connect_socket();
        if (m_socket == INVALID_SOCKET) {
            return false;
        }
        const std::string req = "GET " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(m_port) +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "Sec-WebSocket-Version: 13\r\n\r\n";
        send(m_socket, req.data(), static_cast<int>(req.size()), 0);
        while (m_buffer.find("\r\n\r\n") == std::string::npos) {
            if (!fill()) {
                return false;
            }
        }
        const std::size_t end = m_buffer.find("\r\n\r\n") + 4;
        const std::string head = m_buffer.substr(0, end);
        m_buffer.erase(0, end);
        if (accept_header != nullptr) {
            const std::size_t at = head.find("Sec-WebSocket-Accept: ");
            if (at != std::string::npos) {
                *accept_header = head.substr(at + 22, head.find("\r\n", at) - at - 22);
            }
        }
        return head.starts_with("HTTP/1.1 101");
    }

    bool send_text(std::string_view text) {
        std::string wire;
        boson::webview::ws::append_frame(wire, boson::webview::ws::Opcode::Text, text, true, 0x5A17C0DEu ^ static_cast<std::uint32_t>(++m_mask_seed));
        return send(m_socket, wire.data(), static_cast<int>(wire.size()), 0) == static_cast<int>(wire.size());
    }

    /// Next text message, or nullopt on timeout / close.
    std::optional<std::string> recv_text() {
        for (;;) {
            boson::webview::ws::Frame frame;
            const auto r = boson::webview::ws::parse_frame(m_buffer, frame, 512u << 20, false);
            if (r.status == boson::webview::ws::ParseStatus::Error) {
                return std::nullopt;
            }
            if (r.status == boson::webview::ws::ParseStatus::Complete) {
                m_buffer.erase(0, r.consumed);
                if (frame.opcode == boson::webview::ws::Opcode::Close) {
                    return std::nullopt;
                }
                if (frame.opcode == boson::webview::ws::Opcode::Text) {
                    return frame.payload;
                }
                continue;
            }
            if (!fill()) {
                return std::nullopt;
            }
        }
    }

    /// Sends a command and waits for its response; events received meanwhile go to events.
    std::optional<std::string> command(int id, std::string_view method, std::string_view params = "{}", std::string_view session = {}) {
        std::string msg = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + std::string(method) + "\",\"params\":" + std::string(params);
        if (!session.empty()) {
            msg += ",\"sessionId\":\"" + std::string(session) + "\"";
        }
        msg += "}";
        if (!send_text(msg)) {
            return std::nullopt;
        }
        const std::string tag = "\"id\":" + std::to_string(id);
        for (;;) {
            auto m = recv_text();
            if (!m) {
                return std::nullopt;
            }
            if (m->find(tag + ",") != std::string::npos || m->find(tag + "}") != std::string::npos) {
                return m;
            }
            events.push_back(std::move(*m));
        }
    }

    void close() {
        if (m_socket != INVALID_SOCKET) {
            std::string wire;
            boson::webview::ws::append_frame(wire, boson::webview::ws::Opcode::Close, std::string("\x03\xE8", 2), true, 0x11223344u);
            send(m_socket, wire.data(), static_cast<int>(wire.size()), 0);
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }
    }

    std::vector<std::string> events;

private:
    SOCKET connect_socket() const {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            return s;
        }
        DWORD timeout_ms = 15000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(m_port);
        if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        return s;
    }

    bool fill() {
        char buf[65536];
        const int n = recv(m_socket, buf, sizeof(buf), 0);
        if (n <= 0) {
            return false;
        }
        m_buffer.append(buf, static_cast<std::size_t>(n));
        return true;
    }

    std::uint16_t m_port;
    SOCKET m_socket = INVALID_SOCKET;
    std::string m_buffer;
    int m_mask_seed = 0;
};

/// The value of "key":"<value>" in a JSON text (enough for sessionId / targetId).
std::string json_string_field(std::string_view json, std::string_view key) {
    const std::string tag = "\"" + std::string(key) + "\":\"";
    const std::size_t at = json.find(tag);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::size_t start = at + tag.size();
    return std::string(json.substr(start, json.find('"', start) - start));
}

// ---- Runtime helpers -----------------------------------------------------------------------------------

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return true;
}

void pump_for(std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    pump_until([until]() { return std::chrono::steady_clock::now() >= until; }, duration + std::chrono::seconds{1});
}

struct AsyncText {
    bool done = false;
    bool ok = false;
    std::string text;
};

/// ExecuteScript on a WebView; the JSON result.
std::optional<std::string> eval(boson::core::WebView& webview, std::string_view js) {
    auto state = std::make_shared<AsyncText>();
    if (!webview.execute_script(js, [state](HRESULT hr, std::string value) {
            state->ok = SUCCEEDED(hr);
            state->text = std::move(value);
            state->done = true;
        })) {
        return std::nullopt;
    }
    pump_until([&]() { return state->done; }, std::chrono::seconds{10});
    if (!state->done || !state->ok) {
        return std::nullopt;
    }
    return state->text;
}

/// Guest::execute_javascript (CDP Runtime.evaluate); the result text, or nullopt with the error logged.
std::optional<std::string> gjs(boson::webview::Guest& guest, std::string_view code, bool user_gesture = false) {
    auto state = std::make_shared<AsyncText>();
    if (!guest.execute_javascript(code, user_gesture, [state](bool ok, std::string text) {
            state->ok = ok;
            state->text = std::move(text);
            state->done = true;
        })) {
        return std::nullopt;
    }
    pump_until([&]() { return state->done; }, std::chrono::seconds{10});
    if (!state->done) {
        return std::nullopt;
    }
    if (!state->ok) {
        std::cerr << "  guest js error: " << state->text << '\n';
        return std::nullopt;
    }
    return state->text;
}

std::optional<std::string> cdp_call(boson::webview::Cdp& cdp, std::string_view method, std::string_view params, bool* ok_out = nullptr) {
    auto state = std::make_shared<AsyncText>();
    if (!cdp.call(method, params, [state](bool ok, std::string json) {
            state->ok = ok;
            state->text = std::move(json);
            state->done = true;
        })) {
        return std::nullopt;
    }
    pump_until([&]() { return state->done; }, std::chrono::seconds{10});
    if (ok_out != nullptr) {
        *ok_out = state->ok;
    }
    if (!state->done) {
        return std::nullopt;
    }
    return state->text;
}

/// BOSON_WE_ONLY=element,cdp,... runs only the named runtime sections (debugging aid); unset = everything.
bool want(std::string_view section) {
    static const std::string only = []() {
        char buf[256]{};
        const DWORD n = GetEnvironmentVariableA("BOSON_WE_ONLY", buf, sizeof(buf));
        return n > 0 && n < sizeof(buf) ? std::string(buf, n) : std::string{};
    }();
    return only.empty() || only.find(section) != std::string::npos;
}

std::string title_of(boson::core::WebView& webview) {
    return webview.document_title().value_or(std::string{});
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// The element bundle (frontend/src/webview_element.ts as an IIFE, built by CMake with bun), if available.
std::string g_element_js;
std::string g_host_html;

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    const auto bytes = [](const std::string& s) { return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()); };
    if (path == "/host.html") {
        return bytes(g_host_html);
    }
    if (path == "/element.js") {
        return bytes(g_element_js);
    }
    return std::nullopt;
}


/// The element's rect as the host page sees it.
std::optional<JsRect> element_rect(boson::core::WebView& host) {
    auto r = eval(host, "(()=>{const r=document.getElementById('wv').getBoundingClientRect();"
                        "return JSON.stringify({x:r.left,y:r.top,width:r.width,height:r.height,dpr:devicePixelRatio});})()");
    if (!r) {
        return std::nullopt;
    }
    std::string inner;
    JsRect out;
    if (glz::read_json(inner, *r) || glz::read_json(out, inner)) {
        return std::nullopt;
    }
    return out;
}

/// Whether the guest's native bounds follow the element (within a pixel of rounding).
bool follows(boson::webview::Guest& guest, boson::core::WebView& host, const JsRect& r) {
    const RECT origin = host.bounds();
    const RECT b = guest.webview().bounds();
    return close_to(b.left, origin.left + static_cast<LONG>(std::lround(r.x * r.dpr))) &&
           close_to(b.top, origin.top + static_cast<LONG>(std::lround(r.y * r.dpr))) &&
           close_to(b.right - b.left, static_cast<LONG>(std::lround(r.width * r.dpr))) &&
           close_to(b.bottom - b.top, static_cast<LONG>(std::lround(r.height * r.dpr)));
}

/// Pumps until the host's event log (window.__ev) has an entry matching the JS predicate over e.
bool wait_host_event(boson::core::WebView& host, std::string_view predicate, std::chrono::milliseconds timeout = std::chrono::seconds{15}) {
    const std::string probe = "window.__ev.some(e=>" + std::string(predicate) + ")";
    const bool ok = pump_until([&]() {
        if (eval(host, probe) == std::optional<std::string>{"true"}) {
            return true;
        }
        pump_for(std::chrono::milliseconds{30});
        return false;
    }, timeout);
    if (!ok) {
        std::cout << "  last host events: " << eval(host, "JSON.stringify(window.__ev.slice(-4))").value_or("?") << '\n';
    }
    return ok;
}

#ifndef BOSON_ELEMENT_JS
#define BOSON_ELEMENT_JS ""
#endif
#ifndef BOSON_SOURCE_DIR
#define BOSON_SOURCE_DIR ""
#endif

template <typename E, typename Pred>
const E* find_ev(const std::vector<std::pair<std::uint64_t, boson::webview::GuestEvent>>& events, std::uint64_t id, Pred pred) {
    for (const auto& [gid, e] : events) {
        if (gid == id) {
            if (const E* v = std::get_if<E>(&e); v != nullptr && pred(*v)) {
                return v;
            }
        }
    }
    return nullptr;
}

} // namespace

int main() {
    unit_tests();
    std::cout << "unit checks: " << (g_checks - g_failures) << "/" << g_checks << " passed\n";

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    {
        LPWSTR version = nullptr;
        if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
            std::cout << "SKIP runtime checks: no WebView2 runtime installed\n";
            CoUninitialize();
            return g_failures == 0 ? 0 : 1;
        }
        std::wcout << L"WebView2 runtime " << version << L'\n';
        CoTaskMemFree(version);
    }
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    using namespace std::chrono_literals;
    using boson::webview::Guest;
    using boson::webview::GuestOptions;

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path root = std::filesystem::path(temp) / (L"boson-we-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root / L"dl");
    const std::string pid = std::to_string(GetCurrentProcessId());
    {
        LoopbackServer server;
        WE_CHECK(server.port() != 0);
        const std::string origin = "http://127.0.0.1:" + std::to_string(server.port());

        if (std::string_view(BOSON_ELEMENT_JS).size() > 0 && std::filesystem::exists(BOSON_ELEMENT_JS)) {
            g_element_js = read_text(BOSON_ELEMENT_JS);
        }
        g_host_html = "<!doctype html><html><head><meta charset=utf-8><title>Host</title><style>html,body{margin:0;padding:0;overflow:hidden}"
                      "#scroller{position:absolute;left:20px;top:30px;width:600px;height:400px;overflow:auto}"
                      "boson-webview{display:block;width:400px;height:300px}</style></head><body>"
                      "<div id=scroller><div id=spacer style='height:150px'></div><div style='height:1200px'></div></div>" +
                      std::string(g_element_js.empty() ? "" : "<script src='element.js'></script>") + "</body></html>";

        boson::core::WindowConfig window_cfg;
        window_cfg.title = L"Boson webview element test";
        window_cfg.width = 1000;
        window_cfg.height = 720;
        window_cfg.quit_on_destroy = false;
        auto window = std::make_unique<boson::core::Window>(window_cfg);
        WE_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

        boson::exec::Scheduler scheduler(2);
        boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
        window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
        boson::exec::TaskRunner runner(scheduler, marshal);

        boson::core::EnvironmentConfig env_cfg;
        env_cfg.user_data_folder = (root / L"udf").wstring();
        const auto env = boson::core::Environment::create(env_cfg);
        auto host = std::make_unique<boson::core::WebViewHost>(*window, env);
        host->set_default_asset_lookup(&lookup);

        boson::core::WebViewConfig h_cfg;
        h_cfg.initial_url = "boson://app/host.html";
        boson::core::WebView& h = host->create(h_cfg);
        host->set_layout([&](boson::core::WebViewHost&, const boson::core::LayoutContext& ctx) {
            h.set_bounds(RECT{0, 0, ctx.width(), ctx.height()});
        });

        auto manager = std::make_unique<boson::webview::GuestManager>(*host, boson::webview::GuestManagerOptions{
            .resolve_preload = [](std::string_view url) -> std::optional<std::string> {
                if (url == "boson://app/preload.js") {
                    return std::string("window.__fromResolver = true;");
                }
                return std::nullopt;
            },
        });
        boson::webview::GuestManager& mgr = *manager;
        std::vector<std::pair<std::uint64_t, boson::webview::GuestEvent>> events;
        const auto listener = mgr.add_listener([&](Guest& g, const boson::webview::GuestEvent& e) { events.emplace_back(g.id(), e); });

        auto controller = std::make_unique<boson::ipc::GuestController>(mgr, marshal);
        boson::ipc::Router<boson::ipc::GuestController> router(*controller);
        auto bridge = std::make_unique<boson::ipc::Bridge>(runner, marshal, router);
        connect_events(*controller, *bridge);
        bridge->attach(h);

        WE_CHECK(pump_until([&]() { return h.is_ready() && title_of(h) == "Host"; }, 30s));
        if (!g_element_js.empty()) {
            // The bundle runs after the title is parsed: wait for the element definition.
            WE_CHECK(pump_until([&]() { return eval(h, "!!customElements.get('boson-webview')") == std::optional<std::string>{"true"}; }, 10s));
        }

        const auto make = [&](GuestOptions o) -> Guest* {
            Guest* g = mgr.create(0, std::move(o));
            WE_CHECK(g != nullptr);
            return g;
        };
        const auto wait_title = [&](Guest* g, std::string_view t, std::chrono::milliseconds timeout = 30s) {
            return g != nullptr && pump_until([&]() { return g->is_ready() && title_of(g->webview()) == t; }, timeout);
        };

        // ---- <boson-webview> end to end (host page + GuestController + bundled element) -----------------
        if (!want("element")) {
        } else if (g_element_js.empty()) {
            std::cout << "SKIP element checks: webview_element bundle not built (bun not found at configure time)\n";
        } else {
            step("element: create from the host page");
            const std::string create_js =
                "window.__ev=[];const w=document.createElement('boson-webview');w.id='wv';w.setAttribute('src','" + origin +
                "/guest');w.setAttribute('partition','elem" + pid +
                "');for(const t of ['guest-ready','did-start-loading','did-navigate','did-finish-load','dom-ready','page-title-updated',"
                "'ipc-message','console-message','new-window','did-fail-load'])w.addEventListener(t,e=>window.__ev.push({type:t,d:e.detail}));"
                "document.getElementById('spacer').after(w);1";
            WE_CHECK(eval(h, create_js).has_value());
            Guest* eg = nullptr;
            WE_CHECK(pump_until([&]() {
                for (Guest* g : mgr.guests()) {
                    if (g->owner_id() == h.id()) {
                        eg = g;
                    }
                }
                return eg != nullptr && eg->is_ready();
            }, 30s));
            WE_CHECK(wait_host_event(h, "e.type==='guest-ready'"));
            WE_CHECK(wait_host_event(h, "e.type==='page-title-updated'&&e.d.title==='Guest'", 30s));
            WE_CHECK(wait_host_event(h, "e.type==='did-navigate'&&e.d.httpResponseCode===200"));
            WE_CHECK(wait_host_event(h, "e.type==='did-finish-load'"));
            WE_CHECK(wait_host_event(h, "e.type==='dom-ready'"));
            if (eg != nullptr) {
                Guest& g = *eg;
                const auto settle = [&](const std::function<bool(const JsRect&)>& extra, std::chrono::milliseconds timeout = 5s) {
                    return pump_until([&]() {
                        const auto r = element_rect(h);
                        return r.has_value() && g.webview().is_visible() && follows(g, h, *r) && extra(*r);
                    }, timeout);
                };
                step("element: native view follows layout");
                const bool placed = settle([](const JsRect&) { return true; });
                WE_CHECK(placed);
                if (!placed) {
                    std::cout << "  layout stats " << eval(h, "JSON.stringify(customElements.get('boson-webview').layoutStats())").value_or("?")
                              << " visible=" << g.webview().is_visible() << " placed=" << g.placement().visible << '\n';
                }
                {
                    const RECT b = g.webview().bounds();
                    const auto r = element_rect(h);
                    std::cout << "  element " << (r ? r->x : -1) << "," << (r ? r->y : -1) << " " << (r ? r->width : -1) << "x" << (r ? r->height : -1)
                              << " @" << (r ? r->dpr : -1) << " -> native " << b.left << "," << b.top << " " << (b.right - b.left) << "x" << (b.bottom - b.top) << '\n';
                }
                step("element: scroll inside an overflow container (move, then clip)");
                auto t0 = std::chrono::steady_clock::now();
                WE_CHECK(eval(h, "document.getElementById('scroller').scrollTop=100;1").has_value());
                WE_CHECK(settle([&](const JsRect& r) { return std::abs(r.y - 80.0) < 0.5 && !g.placement().clipped; }));
                const double scroll_ms = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()) / 1000.0;
                std::cout << "  scroll -> native move observed after " << scroll_ms << " ms (includes the ExecuteScript round trip)\n";
                WE_CHECK(eval(h, "document.getElementById('scroller').scrollTop=200;1").has_value());
                WE_CHECK(settle([&](const JsRect& r) { return std::abs(r.y + 20.0) < 0.5 && g.placement().clipped && g.placement().clip.top > 0; }));
                {
                    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
                    WE_CHECK(GetWindowRgn(g.webview().container_hwnd(), rgn) != ERROR);
                    RECT box{};
                    GetRgnBox(rgn, &box);
                    const auto r = element_rect(h);
                    std::cout << "  clipped region top " << box.top << " (expected " << (r ? std::lround(50.0 * r->dpr) : -1) << ")\n";
                    WE_CHECK(r.has_value() && close_to(box.top, static_cast<LONG>(std::lround(50.0 * r->dpr))));
                    DeleteObject(rgn);
                }
                WE_CHECK(eval(h, "document.getElementById('scroller').scrollTop=100;1").has_value());
                WE_CHECK(settle([&](const JsRect& r) { return std::abs(r.y - 80.0) < 0.5 && !g.placement().clipped; }));
                {
                    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
                    WE_CHECK(GetWindowRgn(g.webview().container_hwnd(), rgn) == ERROR); // region removed again
                    DeleteObject(rgn);
                }
                WE_CHECK(eval(h, "document.getElementById('scroller').scrollTop=0;1").has_value());
                // At the top the element (150 + 300 px) overflows the 400 px container: clipped at the bottom.
                WE_CHECK(settle([&](const JsRect& r) {
                    return std::abs(r.y - 180.0) < 0.5 && g.placement().clipped && g.placement().clip.top == 0 &&
                           g.placement().clip.bottom == static_cast<LONG>(std::lround(250.0 * r.dpr));
                }));
                step("element: resize and position-only moves");
                WE_CHECK(eval(h, "document.getElementById('wv').style.width='500px';1").has_value());
                WE_CHECK(settle([&](const JsRect& r) { return std::abs(r.width - 500.0) < 0.5; }));
                WE_CHECK(eval(h, "document.getElementById('spacer').style.height='190px';1").has_value());
                WE_CHECK(settle([&](const JsRect& r) { return std::abs(r.y - 220.0) < 0.5; }));
                WE_CHECK(eval(h, "document.getElementById('scroller').style.left='60px';1").has_value());
                WE_CHECK(settle([&](const JsRect& r) { return std::abs(r.x - 60.0) < 0.5; }));
                step("element: hide / show (display, ancestor visibility, scrolled out)");
                WE_CHECK(eval(h, "document.getElementById('wv').style.display='none';1").has_value());
                WE_CHECK(pump_until([&]() { return !g.webview().is_visible(); }, 5s));
                WE_CHECK(eval(h, "document.getElementById('wv').style.display='block';1").has_value());
                WE_CHECK(settle([](const JsRect&) { return true; }));
                WE_CHECK(eval(h, "document.getElementById('scroller').style.visibility='hidden';1").has_value());
                WE_CHECK(pump_until([&]() { return !g.webview().is_visible(); }, 5s));
                WE_CHECK(eval(h, "document.getElementById('scroller').style.visibility='';1").has_value());
                WE_CHECK(settle([](const JsRect&) { return true; }));
                WE_CHECK(eval(h, "document.getElementById('scroller').scrollTop=800;1").has_value());
                WE_CHECK(pump_until([&]() { return !g.webview().is_visible(); }, 5s));
                WE_CHECK(eval(h, "document.getElementById('scroller').scrollTop=0;1").has_value());
                WE_CHECK(settle([](const JsRect&) { return true; }));

                step("element: guest -> host ipc-message");
                WE_CHECK(gjs(g, "bosonGuest.sendToHost('hello', 1, 'two'); true").has_value());
                WE_CHECK(wait_host_event(h, "e.type==='ipc-message'&&e.d.channel==='hello'&&JSON.stringify(e.d.args)==='[1,\"two\"]'"));
                step("element: executeJavaScript / send / console-message / navigation");
                WE_CHECK(eval(h, "window.__x=null;document.getElementById('wv').executeJavaScript('Promise.resolve(6*7)').then(v=>window.__x=v);1").has_value());
                WE_CHECK(pump_until([&]() { return eval(h, "window.__x") == std::optional<std::string>{"42"}; }, 10s));
                WE_CHECK(eval(h, "document.getElementById('wv').send('ping', 7);1").has_value());
                WE_CHECK(pump_until([&]() { return title_of(g.webview()) == "pong7"; }, 10s));
                WE_CHECK(wait_host_event(h, "e.type==='page-title-updated'&&e.d.title==='pong7'"));
                WE_CHECK(gjs(g, "console.warn('elem-console'); 1").has_value());
                const bool console_ok = wait_host_event(h, "e.type==='console-message'&&e.d.message==='elem-console'&&e.d.level===2");
                WE_CHECK(console_ok);
                if (!console_ok) {
                    std::cout << "  guest console_messages=" << g.options().console_messages << '\n';
                }
                WE_CHECK(eval(h, "document.getElementById('wv').loadURL('" + origin + "/page/Second');1").has_value());
                WE_CHECK(wait_host_event(h, "e.type==='page-title-updated'&&e.d.title==='Second'"));
                WE_CHECK(pump_until([&]() { return eval(h, "document.getElementById('wv').canGoBack()") == std::optional<std::string>{"true"}; }, 5s));
                WE_CHECK(eval(h, "document.getElementById('wv').goBack();1").has_value());
                WE_CHECK(pump_until([&]() { return title_of(g.webview()) == "Guest"; }, 10s));
                WE_CHECK(eval(h, "window.__cap=null;document.getElementById('wv').capturePage().then(c=>window.__cap=c.mimeType+':'+c.data.slice(0,8));1").has_value());
                WE_CHECK(pump_until([&]() { return contains(eval(h, "window.__cap"), "image/png:iVBORw0K"); }, 15s));
                WE_CHECK(eval(h, "window.__pdf=0;document.getElementById('wv').printToPDF().then(b=>window.__pdf=b.length);1").has_value());
                WE_CHECK(pump_until([&]() { const auto n = eval(h, "window.__pdf"); return n.has_value() && std::atoi(n->c_str()) > 1000; }, 30s));
                WE_CHECK(eval(h, "window.__id=document.getElementById('wv').getWebContentsId();1").has_value());
                WE_CHECK(eval(h, "window.__id") == std::optional<std::string>{std::to_string(g.id())});

                step("element: Guest.* rejects unknown guest ids");
                {
                    boson::ipc::GuestController& ctl = *controller;
                    const std::uint64_t gid = g.id();
                    std::atomic<int> outcome{0};
                    std::thread worker([&]() {
                        try {
                            (void)ctl.load_url(boson::ipc::GuestUrlRequest{.id = gid + 100000, .url = "about:blank"});
                            outcome = 1;
                        } catch (const std::invalid_argument&) {
                            outcome = 2;
                        } catch (...) {
                            outcome = 3;
                        }
                    });
                    WE_CHECK(pump_until([&]() { return outcome.load() != 0; }, 10s));
                    worker.join();
                    WE_CHECK(outcome.load() == 2);
                }

                step("element: removing the element destroys the guest");
                WE_CHECK(eval(h, "document.getElementById('wv').remove();1").has_value());
                WE_CHECK(pump_until([&]() {
                    for (Guest* x : mgr.guests()) {
                        if (x->owner_id() == h.id()) {
                            return false;
                        }
                    }
                    return true;
                }, 10s));
            }
        }

        using namespace boson::webview;

        // ---- Partitions -----------------------------------------------------------------------------------
        step("partitions: cookies are isolated per partition and shared within one");
        Guest* ga = make(GuestOptions{.src = origin + "/page/A", .partition = "persist:BosonElemA" + pid});
        Guest* gb = make(GuestOptions{.src = origin + "/page/B", .partition = "elemB" + pid});
        Guest* gc = make(GuestOptions{.src = origin + "/page/C"});
        Guest* gb2 = make(GuestOptions{.src = origin + "/page/B2", .partition = "elemB" + pid});
        WE_CHECK(wait_title(ga, "A") && wait_title(gb, "B") && wait_title(gc, "C") && wait_title(gb2, "B2"));
        if (want("partition") && ga != nullptr && gb != nullptr && gc != nullptr && gb2 != nullptr) {
            WE_CHECK(contains(gjs(*ga, "document.cookie='k=alpha; path=/'; document.cookie"), "k=alpha"));
            WE_CHECK(contains(gjs(*gb, "document.cookie='k=beta; path=/'; document.cookie"), "k=beta"));
            const auto in_c = gjs(*gc, "document.cookie");
            WE_CHECK(in_c.has_value() && !contains(in_c, "k="));
            WE_CHECK(contains(gjs(*ga, "document.cookie"), "k=alpha"));
            WE_CHECK(contains(gjs(*gb2, "document.cookie"), "k=beta"));  // same partition, same jar
            WE_CHECK(!contains(gjs(*gb2, "document.cookie"), "alpha"));
            const auto pa = profile_info(ga->webview());
            const auto pb = profile_info(gb->webview());
            WE_CHECK(pa.has_value() && !pa->in_private && pa->name == "BosonElemA" + pid);
            WE_CHECK(pb.has_value() && pb->in_private);
        }

        // ---- Preload --------------------------------------------------------------------------------------
        step("preload runs in the guest (every document) and never in the host");
        Guest* gp = make(GuestOptions{.src = origin + "/page/P", .preload_script = "window.__preloaded = (window.__preloaded || 0) + 1; window.__preTitle = document.title;"});
        WE_CHECK(wait_title(gp, "P"));
        if (want("preload") && gp != nullptr && gc != nullptr) {
            WE_CHECK(gjs(*gp, "window.__preloaded") == std::optional<std::string>{"1"});
            WE_CHECK(contains(gjs(*gp, "JSON.stringify(window.__preTitle)"), "\"\"")); // ran before the document parsed
            WE_CHECK(gp->load_url(origin + "/page/P2"));
            WE_CHECK(wait_title(gp, "P2"));
            WE_CHECK(gjs(*gp, "window.__preloaded") == std::optional<std::string>{"1"});
            WE_CHECK(contains(gjs(*gc, "typeof window.__preloaded"), "undefined"));
            WE_CHECK(contains(gjs(*gc, "typeof window.bosonGuest"), "object"));
            WE_CHECK(contains(eval(h, "typeof window.__preloaded"), "undefined"));
            WE_CHECK(contains(eval(h, "typeof window.bosonGuest"), "undefined"));
            boson::ipc::GuestCreateRequest req;
            req.preload = "boson://app/preload.js";
            WE_CHECK(boson::ipc::to_guest_options(req, &mgr).preload_script == "window.__fromResolver = true;");
        }

        // ---- Guest <-> host messages -------------------------------------------------------------------------
        step("guest -> host ipc-message and host -> guest send");
        if (want("ipc") && gc != nullptr) {
            WE_CHECK(gjs(*gc, "bosonGuest.sendToHost('native-ch', {a: 1}, 'z'); true").has_value());
            const std::uint64_t id = gc->id();
            WE_CHECK(pump_until([&]() {
                return find_ev<GuestIpcMessage>(events, id, [](const GuestIpcMessage& m) { return m.channel == "native-ch" && m.args.str == R"([{"a":1},"z"])"; }) != nullptr;
            }, 10s));
            WE_CHECK(gjs(*gc, "window.__got = null; bosonGuest.on('hello', (e, a, b) => { window.__got = e.channel + ':' + (a + b); }); true").has_value());
            WE_CHECK(gc->send("hello", "[1,2]"));
            WE_CHECK(pump_until([&]() { return contains(gjs(*gc, "window.__got"), "hello:3"); }, 10s));
            // Plain chrome.webview messages without the guest tag are not ipc-message.
            const std::size_t before = events.size();
            WE_CHECK(gjs(*gc, "chrome.webview.postMessage({channel: 'spoof'}); true").has_value());
            pump_for(300ms);
            WE_CHECK(find_ev<GuestIpcMessage>(events, id, [](const GuestIpcMessage& m) { return m.channel == "spoof"; }) == nullptr);
            (void)before;
        }

        // ---- Popups ---------------------------------------------------------------------------------------
        step("popups: new guest (allowpopups) and denied");
        Guest* go = make(GuestOptions{.src = origin + "/page/Opener", .popups = PopupPolicy::Guest});
        WE_CHECK(wait_title(go, "Opener"));
        if (want("popup") && go != nullptr) {
            const std::uint64_t id = go->id();
            const std::size_t count = mgr.size();
            WE_CHECK(gjs(*go, "window.open('" + origin + "/page/Popup'); true", true).has_value());
            const GuestNewWindow* nw = nullptr;
            WE_CHECK(pump_until([&]() {
                nw = find_ev<GuestNewWindow>(events, id, [](const GuestNewWindow& e) { return e.disposition == "guest"; });
                return nw != nullptr;
            }, 15s));
            if (nw != nullptr) {
                const std::uint64_t popup_id = nw->new_guest_id;
                WE_CHECK(contains(nw->url, "/page/Popup"));
                Guest* popup = mgr.find(popup_id);
                WE_CHECK(popup != nullptr && popup->opener_id() == id);
                WE_CHECK(wait_title(popup, "Popup"));
                WE_CHECK(find_ev<GuestCreated>(events, popup_id, [](const GuestCreated& e) { return e.reason == "popup"; }) != nullptr);
                WE_CHECK(mgr.size() == count + 1);
                WE_CHECK(mgr.destroy(popup_id));
                WE_CHECK(find_ev<GuestDestroyed>(events, popup_id, [](const GuestDestroyed&) { return true; }) != nullptr);
            }
            go->options().popups = PopupPolicy::Deny;
            WE_CHECK(gjs(*go, "String(window.open('" + origin + "/page/Popup2'))", true).has_value());
            WE_CHECK(pump_until([&]() {
                return find_ev<GuestNewWindow>(events, id, [](const GuestNewWindow& e) { return e.disposition == "denied" && contains(e.url, "Popup2"); }) != nullptr;
            }, 10s));
            WE_CHECK(mgr.size() == count);
        }

        // ---- Downloads ------------------------------------------------------------------------------------
        step("downloads: progress to completion, then cancel");
        Guest* gd = make(GuestOptions{.src = origin + "/page/D", .download_folder = (root / L"dl").wstring()});
        WE_CHECK(wait_title(gd, "D"));
        if (want("download") && gd != nullptr) {
            const std::uint64_t id = gd->id();
            WE_CHECK(gd->load_url(origin + "/download"));
            const GuestDownload* done = nullptr;
            WE_CHECK(pump_until([&]() {
                done = find_ev<GuestDownload>(events, id, [](const GuestDownload& d) { return d.phase == "done" && contains(d.url, "/download"); });
                return done != nullptr;
            }, 30s));
            WE_CHECK(find_ev<GuestDownload>(events, id, [](const GuestDownload& d) { return d.phase == "started" && contains(d.url, "/download"); }) != nullptr);
            if (done != nullptr) {
                std::cout << "  download " << done->state << " " << done->received_bytes << " bytes -> " << done->path << '\n';
                WE_CHECK(done->state == "completed");
                WE_CHECK(done->received_bytes == static_cast<std::int64_t>(kDownloadSize));
                const std::filesystem::path file = boson::core::utf8_to_wide(done->path);
                WE_CHECK(file.parent_path() == root / L"dl");
                std::error_code ec;
                WE_CHECK(std::filesystem::file_size(file, ec) == kDownloadSize);
            }
            WE_CHECK(gd->load_url(origin + "/slow"));
            const GuestDownload* started = nullptr;
            WE_CHECK(pump_until([&]() {
                started = find_ev<GuestDownload>(events, id, [](const GuestDownload& d) { return d.phase == "started" && contains(d.url, "/slow"); });
                return started != nullptr;
            }, 30s));
            if (started != nullptr) {
                const std::uint64_t did = started->download_id;
                WE_CHECK(pump_until([&]() {
                    return find_ev<GuestDownload>(events, id, [did](const GuestDownload& d) { return d.download_id == did && d.phase == "updated" && d.received_bytes > 0; }) != nullptr;
                }, 15s));
                WE_CHECK(gd->active_downloads() == 1);
                WE_CHECK(gd->download_cancel(did));
                const GuestDownload* cancelled = nullptr;
                WE_CHECK(pump_until([&]() {
                    cancelled = find_ev<GuestDownload>(events, id, [did](const GuestDownload& d) { return d.download_id == did && d.phase == "done"; });
                    return cancelled != nullptr;
                }, 15s));
                if (cancelled != nullptr) {
                    std::cout << "  slow download " << cancelled->state << " (" << cancelled->interrupt_reason << ") after " << cancelled->received_bytes << " bytes\n";
                    WE_CHECK(cancelled->state == "cancelled");
                }
                WE_CHECK(gd->active_downloads() == 0);
                WE_CHECK(!gd->download_cancel(did));
            }
        }

        // ---- Guest hooks: permissions, basic auth, user agent, zoom, CSS, find ---------------------------------
        step("hooks: permission prompt (ask), basic auth (ask), user agent, zoom, insertCSS, find");
        Guest* gh = make(GuestOptions{.src = origin + "/page/Hooks", .permissions = PromptPolicy::Ask, .basic_auth = PromptPolicy::Ask});
        WE_CHECK(wait_title(gh, "Hooks"));
        if (want("hooks") && gh != nullptr) {
            const std::uint64_t id = gh->id();
            WE_CHECK(gjs(*gh, "Notification.requestPermission().then(p => { document.title = 'perm:' + p; }); 1", true).has_value());
            const GuestPermissionRequest* pr = nullptr;
            WE_CHECK(pump_until([&]() {
                pr = find_ev<GuestPermissionRequest>(events, id, [](const GuestPermissionRequest& e) { return e.permission == "notifications"; });
                return pr != nullptr;
            }, 15s));
            if (pr != nullptr) {
                WE_CHECK(gh->pending_requests() == 1);
                WE_CHECK(gh->respond_permission(pr->request_id, boson::core::PermissionState::Allow));
                WE_CHECK(wait_title(gh, "perm:granted", 10s));
                WE_CHECK(!gh->respond_permission(pr->request_id, boson::core::PermissionState::Allow)); // answered once
            }
            WE_CHECK(gh->load_url(origin + "/auth"));
            const GuestLoginRequest* lr = nullptr;
            WE_CHECK(pump_until([&]() {
                lr = find_ev<GuestLoginRequest>(events, id, [](const GuestLoginRequest& e) { return contains(e.url, "/auth"); });
                return lr != nullptr;
            }, 15s));
            if (lr != nullptr) {
                std::cout << "  login challenge: " << lr->challenge << '\n';
                WE_CHECK(gh->respond_login(lr->request_id, Credentials{.username = "user", .password = "pass"}));
                WE_CHECK(wait_title(gh, "Authed", 15s));
            }
            WE_CHECK(gh->set_user_agent("BosonGuestUA/1.0"));
            WE_CHECK(gh->reload());
            WE_CHECK(pump_until([&]() { return contains(gjs(*gh, "navigator.userAgent"), "BosonGuestUA/1.0"); }, 10s));
            WE_CHECK(gh->set_zoom_factor(1.5));
            WE_CHECK(pump_until([&]() { const auto z = gh->webview().zoom_factor(); return z.has_value() && std::abs(*z - 1.5) < 0.01; }, 5s));
            WE_CHECK(!gh->set_zoom_factor(0.0));
            auto css = std::make_shared<AsyncText>();
            WE_CHECK(gh->insert_css("body { background-color: rgb(1, 2, 3) !important; }", [css](bool ok, std::string key) {
                css->ok = ok;
                css->text = std::move(key);
                css->done = true;
            }));
            WE_CHECK(pump_until([&]() { return css->done; }, 5s) && css->ok);
            WE_CHECK(contains(gjs(*gh, "getComputedStyle(document.body).backgroundColor"), "rgb(1, 2, 3)"));
            WE_CHECK(gh->remove_inserted_css(css->text));
            WE_CHECK(pump_until([&]() { return !contains(gjs(*gh, "getComputedStyle(document.body).backgroundColor"), "rgb(1, 2, 3)"); }, 5s));
            const std::uint64_t find_id = gh->find_in_page("Authed", true, false, false);
            WE_CHECK(find_id != 0);
            WE_CHECK(pump_until([&]() {
                return find_ev<GuestFoundInPage>(events, id, [find_id](const GuestFoundInPage& e) { return e.request_id == find_id && e.matches >= 1; }) != nullptr;
            }, 10s));
            WE_CHECK(gh->stop_find_in_page(StopFindAction::ClearSelection));
        }

        step("crashed: a renderer crash is reported (isolated partition)");
        Guest* gx = make(GuestOptions{.src = origin + "/page/Crash", .partition = "crash" + pid});
        WE_CHECK(wait_title(gx, "Crash"));
        if (want("crash") && gx != nullptr) {
            const std::uint64_t id = gx->id();
            WE_CHECK(gx->cdp().call("Page.crash", "{}", Cdp::ResultFn{}));
            const GuestCrashed* crash = nullptr;
            WE_CHECK(pump_until([&]() {
                crash = find_ev<GuestCrashed>(events, id, [](const GuestCrashed&) { return true; });
                return crash != nullptr;
            }, 15s));
            if (crash != nullptr) {
                std::cout << "  crashed: " << crash->kind << " reason " << crash->reason << '\n';
            }
            WE_CHECK(gx->is_crashed());
            WE_CHECK(mgr.destroy(id));
        }

        // ---- CDP (C++ API) -----------------------------------------------------------------------------------
        step("cdp: commands, errors, events, relay");
        if (want("cdp") && gc != nullptr) {
            Cdp& cdp = gc->cdp();
            WE_CHECK(contains(cdp_call(cdp, "Runtime.evaluate", R"({"expression":"6*7","returnByValue":true})"), "\"value\":42"));
            bool ok = true;
            const auto err = cdp_call(cdp, "Nope.nothing", "{}", &ok);
            WE_CHECK(!ok && err.has_value());
            std::vector<std::string> console;
            const std::size_t receivers = cdp.receiver_count();
            const auto sub = cdp.subscribe("Runtime.consoleAPICalled", [&](std::string_view method, std::string_view params, std::string_view) {
                if (method == "Runtime.consoleAPICalled") {
                    console.emplace_back(params);
                }
            });
            WE_CHECK(sub != 0);
            WE_CHECK(cdp.receiver_count() == receivers + 1);
            WE_CHECK(cdp_call(cdp, "Runtime.enable", "{}").has_value());
            WE_CHECK(gjs(*gc, "console.log('cdp-console-test'); 1").has_value());
            WE_CHECK(pump_until([&]() { return std::ranges::any_of(console, [](const std::string& p) { return contains(p, "cdp-console-test"); }); }, 10s));
            WE_CHECK(cdp.unsubscribe(sub));
            WE_CHECK(cdp.receiver_count() == receivers);
            WE_CHECK(cdp_call(cdp, "Runtime.disable", "{}").has_value());

            constexpr int kRounds = 100;
            const auto t0 = std::chrono::steady_clock::now();
            int good = 0;
            for (int i = 0; i < kRounds; ++i) {
                good += contains(cdp_call(cdp, "Runtime.evaluate", R"({"expression":"1","returnByValue":true})"), "\"value\":1") ? 1 : 0;
            }
            const double us = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()) / kRounds;
            std::cout << "  Cdp::call Runtime.evaluate round trip: " << us << " us avg over " << kRounds << '\n';
            WE_CHECK(good == kRounds);

            const std::vector<std::string> relay{"Page.frameNavigated"};
            gc->cdp_subscribe(relay);
            WE_CHECK(cdp_call(cdp, "Page.enable", "{}").has_value());
            const std::uint64_t id = gc->id();
            WE_CHECK(gc->load_url(origin + "/page/C2"));
            WE_CHECK(pump_until([&]() {
                return find_ev<GuestCdpEvent>(events, id, [](const GuestCdpEvent& e) { return e.method == "Page.frameNavigated" && contains(e.params.str, "/page/C2"); }) != nullptr;
            }, 15s));
            gc->cdp_unsubscribe(relay);
            WE_CHECK(cdp_call(cdp, "Page.disable", "{}").has_value());
        }

        step("cdp: WebViewController.cdp_send / cdp_subscribe on the host page");
        if (want("wvc")) {
            boson::ipc::WebViewController wvc(h, marshal);
            std::vector<std::string> got;
            wvc.on_cdp_event = [&](const boson::ipc::CdpEventPayload& e) { got.push_back(e.method + " " + e.params.str); };
            std::atomic<int> phase{0};
            std::string result;
            std::thread worker([&]() {
                try {
                    result = wvc.cdp_send(boson::ipc::CdpCommand{.method = "Runtime.evaluate", .params = glz::raw_json{R"({"expression":"1+1","returnByValue":true})"}, .session_id = {}}).result.str;
                    (void)wvc.cdp_subscribe(boson::ipc::CdpSubscription{.events = {"Runtime.consoleAPICalled"}, .subscribe = true});
                    (void)wvc.cdp_send(boson::ipc::CdpCommand{.method = "Runtime.enable", .params = glz::raw_json{"{}"}, .session_id = {}});
                    bool threw = false;
                    try {
                        (void)wvc.cdp_send(boson::ipc::CdpCommand{.method = "Nope.nothing", .params = glz::raw_json{"{}"}, .session_id = {}});
                    } catch (const std::runtime_error&) {
                        threw = true;
                    }
                    phase = threw ? 1 : 2;
                } catch (...) {
                    phase = 3;
                }
            });
            WE_CHECK(pump_until([&]() { return phase.load() != 0; }, 20s));
            worker.join();
            WE_CHECK(phase.load() == 1);
            WE_CHECK(contains(result, "\"value\":2"));
            WE_CHECK(eval(h, "console.log('wvc-cdp'); 1").has_value());
            WE_CHECK(pump_until([&]() { return std::ranges::any_of(got, [](const std::string& s) { return contains(s, "wvc-cdp"); }); }, 10s));
            phase = 0;
            std::thread worker2([&]() {
                try {
                    (void)wvc.cdp_subscribe(boson::ipc::CdpSubscription{.events = {"Runtime.consoleAPICalled"}, .subscribe = false});
                    (void)wvc.cdp_send(boson::ipc::CdpCommand{.method = "Runtime.disable", .params = glz::raw_json{"{}"}, .session_id = {}});
                    phase = 1;
                } catch (...) {
                    phase = 3;
                }
            });
            WE_CHECK(pump_until([&]() { return phase.load() != 0; }, 20s));
            worker2.join();
            WE_CHECK(phase.load() == 1);
        }

        // ---- Layout batching cost -------------------------------------------------------------------------
        step("perf: GuestManager::apply_layout");
        if (want("perf")) {
            std::vector<GuestLayoutItem> items;
            for (Guest* g : mgr.guests()) {
                items.push_back(GuestLayoutItem{.id = g->id(), .rect = CssRect{.x = 0, .y = 0, .width = 120, .height = 90}, .clip = std::nullopt, .visible = true, .z = 0});
            }
            constexpr int kFrames = 120;
            const auto frame_us = [&](const std::function<void(int)>& mutate, int frames) {
                const auto t0 = std::chrono::steady_clock::now();
                for (int f = 0; f < frames; ++f) {
                    mutate(f);
                    (void)mgr.apply_layout(0, items, 1.0);
                }
                return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) / frames / 1000.0;
            };
            // Scroll: every guest moves, size and clip unchanged.
            const double move_us = frame_us([&](int f) {
                for (std::size_t i = 0; i < items.size(); ++i) {
                    items[i].rect.x = static_cast<double>(static_cast<int>(i) * 130);
                    items[i].rect.y = static_cast<double>(300 + (f % 50));
                    items[i].clip = std::nullopt;
                }
            }, kFrames);
            // Scroll inside an overflow container: moves and the window region changes every frame.
            const double clip_us = frame_us([&](int f) {
                for (std::size_t i = 0; i < items.size(); ++i) {
                    items[i].rect.y = static_cast<double>(300 + (f % 50));
                    items[i].clip = CssRect{.x = 0, .y = 320, .width = 1000, .height = 300};
                }
            }, kFrames);
            // Resize: every guest changes size.
            const double resize_us = frame_us([&](int f) {
                for (std::size_t i = 0; i < items.size(); ++i) {
                    items[i].rect.width = static_cast<double>(100 + (f % 40));
                    items[i].clip = std::nullopt;
                }
            }, kFrames);
            const double still_us = frame_us([](int) {}, kFrames * 10);
            std::cout << "  apply_layout, " << items.size() << " guests: move " << move_us << " us/frame, move+clip " << clip_us
                      << " us/frame, resize " << resize_us << " us/frame, unchanged " << still_us << " us/frame\n";
            // Batched moves land: every container sits exactly where its placement says (client coordinates of the parent).
            for (std::size_t i = 0; i < items.size(); ++i) {
                items[i].rect = CssRect{.x = 10.0 + static_cast<double>(i) * 7, .y = 20.0 + static_cast<double>(i) * 3, .width = 111, .height = 77};
                items[i].clip = std::nullopt;
            }
            (void)mgr.apply_layout(0, items, 1.0);
            const auto container_rect = [&](Guest* g) {
                RECT r{};
                HWND container = g->webview().container_hwnd();
                GetWindowRect(container, &r);
                MapWindowPoints(nullptr, GetParent(container), reinterpret_cast<POINT*>(&r), 2);
                return r;
            };
            for (Guest* g : mgr.guests()) {
                const RECT want_rect = g->placement().bounds;
                const RECT got = container_rect(g);
                WE_CHECK(EqualRect(&want_rect, &got) != FALSE);
            }
            // A hidden WebView keeps the latest bounds but only applies them when it is shown again.
            if (Guest* g0 = mgr.guests().empty() ? nullptr : mgr.guests().front(); g0 != nullptr) {
                boson::core::WebView& wv0 = g0->webview();
                const RECT before = container_rect(g0);
                const RECT moved{before.left + 40, before.top + 25, before.left + 40 + 90, before.top + 25 + 60};
                wv0.set_visible(false);
                wv0.set_bounds(moved);
                const RECT hidden_rect = container_rect(g0);
                WE_CHECK(EqualRect(&before, &hidden_rect) != FALSE);
                const RECT stored = wv0.bounds();
                WE_CHECK(EqualRect(&moved, &stored) != FALSE);
                wv0.set_visible(true);
                const RECT shown_rect = container_rect(g0);
                WE_CHECK(EqualRect(&moved, &shown_rect) != FALSE);
            }
            for (auto& it : items) {
                it.visible = false;
            }
            (void)mgr.apply_layout(0, items, 1.0);
            for (Guest* g : mgr.guests()) {
                WE_CHECK(!g->webview().is_visible());
            }
        }

        // ---- CDP endpoint (WebSocket) -----------------------------------------------------------------------
        step("cdp server: HTTP discovery, WebSocket, Target domain, flattened sessions");
        if (want("server")) {
            CdpServer cdp_server;
            WE_CHECK(cdp_server.start());
            WE_CHECK(cdp_server.running() && cdp_server.port() != 0 && cdp_server.token().size() == 32);
            std::cout << "  endpoint " << cdp_server.http_endpoint() << '\n';
            Guest* gpw = make(GuestOptions{.src = origin + "/pw"});
            WE_CHECK(wait_title(gpw, "PW"));
            if (gpw != nullptr) {
                const RECT hb = h.bounds();
                gpw->place(compute_element_placement(CssRect{.x = 20, .y = 20, .width = 640, .height = 480}, std::nullopt, 1.0, POINT{hb.left, hb.top}, true));
            }
            auto cdp_bridge = std::make_unique<CdpGuestBridge>(cdp_server, mgr, CdpGuestBridgeOptions{.owner_id = 0, .defaults = {}, .expose_existing = true});
            WE_CHECK(pump_until([&]() { return cdp_server.stats().targets == mgr.size(); }, 10s));

            struct ClientResult {
                std::string no_token;
                std::string version;
                std::string list;
                bool upgraded = false;
                std::string accept;
                std::optional<std::string> browser_version;
                std::optional<std::string> targets;
                std::string session;
                bool attached_event = false;
                std::optional<std::string> eval;
                bool console_event = false;
                double round_trip_us = 0;
                int round_trips_ok = 0;
                std::string created_target;
                std::string created_title;
                std::optional<std::string> closed;
                std::optional<std::string> unknown;
            } res;
            WE_CHECK(pump_until([&]() { return gpw != nullptr && !cdp_server.target_id(gpw->id()).empty(); }, 10s));
            const std::string pw_target = gpw != nullptr ? cdp_server.target_id(gpw->id()) : std::string{};
            WE_CHECK(pw_target.size() == 32 && pw_target != CdpServer::target_id_of(gpw != nullptr ? gpw->id() : 0)); // the main frame id
            const std::string token = cdp_server.token();
            const std::string ws_url = cdp_server.browser_websocket_url();
            const std::string ws_path = ws_url.substr(ws_url.find('/', std::string_view("ws://").size()));
            std::atomic<bool> client_done{false};
            std::thread client([&]() {
                TestCdpClient c(cdp_server.port());
                res.no_token = c.http_get("/json/version");
                res.version = c.http_get("/" + token + "/json/version");
                res.list = c.http_get("/" + token + "/json/list");
                res.upgraded = c.open_websocket(ws_path, &res.accept);
                if (res.upgraded) {
                    res.browser_version = c.command(1, "Browser.getVersion");
                    res.targets = c.command(2, "Target.getTargets");
                    const auto attach = c.command(3, "Target.attachToTarget", R"({"targetId":")" + pw_target + R"(","flatten":true})");
                    res.session = attach ? json_string_field(*attach, "sessionId") : std::string{};
                    res.attached_event = std::ranges::any_of(c.events, [](const std::string& e) { return contains(e, "Target.attachedToTarget"); });
                    res.eval = c.command(4, "Runtime.evaluate", R"({"expression":"6*7","returnByValue":true})", res.session);
                    (void)c.command(5, "Runtime.enable", "{}", res.session);
                    c.events.clear();
                    (void)c.command(6, "Runtime.evaluate", R"({"expression":"console.log('ws-console'),1"})", res.session);
                    const auto deadline = std::chrono::steady_clock::now() + 5s;
                    while (!res.console_event && std::chrono::steady_clock::now() < deadline) {
                        res.console_event = std::ranges::any_of(c.events, [&](const std::string& e) {
                            return contains(e, "Runtime.consoleAPICalled") && contains(e, "ws-console") && contains(e, res.session);
                        });
                        if (!res.console_event) {
                            if (auto m = c.recv_text()) {
                                c.events.push_back(std::move(*m));
                            }
                        }
                    }
                    constexpr int kRounds = 200;
                    const auto t0 = std::chrono::steady_clock::now();
                    for (int i = 0; i < kRounds; ++i) {
                        res.round_trips_ok += contains(c.command(100 + i, "Runtime.evaluate", R"({"expression":"1","returnByValue":true})", res.session), "\"value\":1") ? 1 : 0;
                    }
                    res.round_trip_us = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()) / kRounds;
                    const auto created = c.command(7, "Target.createTarget", R"({"url":")" + origin + R"(/page/Created"})");
                    res.created_target = created ? json_string_field(*created, "targetId") : std::string{};
                    if (!res.created_target.empty()) {
                        const auto a2 = c.command(8, "Target.attachToTarget", R"({"targetId":")" + res.created_target + R"(","flatten":true})");
                        const std::string s2 = a2 ? json_string_field(*a2, "sessionId") : std::string{};
                        for (int i = 0; i < 50 && res.created_title != "Created"; ++i) {
                            const auto t = c.command(200 + i, "Runtime.evaluate", R"({"expression":"document.title","returnByValue":true})", s2);
                            res.created_title = t ? json_string_field(*t, "value") : std::string{};
                            if (res.created_title != "Created") {
                                Sleep(100);
                            }
                        }
                        res.closed = c.command(9, "Target.closeTarget", R"({"targetId":")" + res.created_target + R"("})");
                    }
                    res.unknown = c.command(10, "Nope.nothing");
                    c.close();
                }
                client_done = true;
            });
            WE_CHECK(pump_until([&]() { return client_done.load(); }, 90s));
            client.join();
            WE_CHECK(res.no_token.starts_with("404"));
            WE_CHECK(res.version.starts_with("200") && contains(res.version, "webSocketDebuggerUrl") && contains(res.version, "\"Browser\""));
            WE_CHECK(res.list.starts_with("200") && contains(res.list, pw_target));
            WE_CHECK(res.upgraded);
            WE_CHECK(res.accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
            WE_CHECK(contains(res.browser_version, "\"product\""));
            WE_CHECK(contains(res.targets, pw_target));
            WE_CHECK(!res.session.empty());
            WE_CHECK(res.attached_event);
            WE_CHECK(contains(res.eval, "\"value\":42"));
            WE_CHECK(res.console_event);
            WE_CHECK(res.round_trips_ok == 200);
            std::cout << "  CDP over WebSocket Runtime.evaluate round trip: " << res.round_trip_us << " us avg over 200\n";
            WE_CHECK(!res.created_target.empty());
            WE_CHECK(res.created_title == "Created");
            WE_CHECK(contains(res.closed, "\"success\":true"));
            WE_CHECK(contains(res.unknown, "\"error\""));
            WE_CHECK(pump_until([&]() { return cdp_server.stats().targets == mgr.size(); }, 10s));
            const auto stats = cdp_server.stats();
            std::cout << "  server stats: " << stats.connections << " ws connections, " << stats.http_requests << " http, " << stats.messages_in << " in / "
                      << stats.messages_out << " out, " << stats.commands_forwarded << " forwarded, " << stats.events_forwarded << " events\n";
            WE_CHECK(stats.commands_forwarded > 200);

            // ---- Playwright (optional) -------------------------------------------------------------------
            step("playwright: chromium.connectOverCDP against the endpoint");
            wchar_t pw_dir[MAX_PATH]{};
            const DWORD pw_len = GetEnvironmentVariableW(L"BOSON_PLAYWRIGHT_DIR", pw_dir, MAX_PATH);
            const std::filesystem::path pw_root = pw_len > 0 && pw_len < MAX_PATH ? std::filesystem::path(pw_dir) : std::filesystem::path{};
            wchar_t node[MAX_PATH]{};
            const bool have_node = SearchPathW(nullptr, L"node.exe", nullptr, MAX_PATH, node, nullptr) > 0;
            const std::filesystem::path script = std::filesystem::path(BOSON_SOURCE_DIR) / "tests" / "js" / "playwright_cdp.mjs";
            if (pw_root.empty() || !std::filesystem::exists(pw_root / "node_modules" / "playwright-core") || !have_node || !std::filesystem::exists(script)) {
                std::cout << "SKIP playwright: set BOSON_PLAYWRIGHT_DIR to a folder with node_modules/playwright-core (npm install playwright-core "
                             "outside the repo) and put node on PATH\n";
            } else {
                const std::filesystem::path log = root / L"playwright.log";
                SECURITY_ATTRIBUTES sa{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
                HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                std::wstring cmd = L"\"" + std::wstring(node) + L"\" \"" + script.wstring() + L"\" \"" + pw_root.wstring() + L"\" \"" +
                                   boson::core::utf8_to_wide(cdp_server.http_endpoint()) + L"\" \"" + boson::core::utf8_to_wide(origin) + L"\"";
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdOutput = out;
                si.hStdError = out;
                si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                PROCESS_INFORMATION pi{};
                const bool spawned = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE;
                WE_CHECK(spawned);
                if (spawned) {
                    const bool finished = pump_until([&]() { return WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0; }, 180s);
                    if (!finished) {
                        TerminateProcess(pi.hProcess, 99); // our own child
                    }
                    DWORD code = 1;
                    GetExitCodeProcess(pi.hProcess, &code);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    CloseHandle(out);
                    out = INVALID_HANDLE_VALUE;
                    const std::string text = read_text(log);
                    std::istringstream lines(text);
                    std::string line;
                    int ok_ops = 0;
                    int failed_ops = 0;
                    while (std::getline(lines, line)) {
                        std::cout << "  " << line << '\n';
                        ok_ops += line.find(" ok ") != std::string::npos ? 1 : 0;
                        failed_ops += line.find(" FAIL ") != std::string::npos ? 1 : 0;
                    }
                    std::cout << "  playwright: " << ok_ops << " operations ok, " << failed_ops << " failed (exit " << code << ")\n";
                    WE_CHECK(finished && code == 0);
                }
                if (out != INVALID_HANDLE_VALUE) {
                    CloseHandle(out);
                }
            }
            cdp_bridge.reset();
            cdp_server.stop();
            WE_CHECK(!cdp_server.running());
        }

        step("shutdown");
        disconnect_events(*controller);
        bridge.reset();
        controller.reset();
        mgr.remove_listener(listener);
        manager->destroy_all();
        WE_CHECK(mgr.size() == 0);
        manager.reset();
        bool shut_down = false;
        env->shutdown([&]() { shut_down = true; });
        WE_CHECK(pump_until([&]() { return shut_down; }, 15s));
        host.reset();
        window.reset();
    }
    std::error_code ec;
    for (int i = 0; i < 20; ++i) {
        std::filesystem::remove_all(root, ec);
        if (!std::filesystem::exists(root)) {
            break;
        }
        Sleep(250);
    }
    WSACleanup();
    std::cout << (g_checks - g_failures) << "/" << g_checks << " webview element checks passed\n";
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
