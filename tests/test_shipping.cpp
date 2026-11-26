// Shipping tests: versions, crypto, manifests, logger, WinHTTP downloads against an in-process loopback
// server, the update helper (portable swap + rollback, installer exe), crash dumps in child processes,
// WebView2 runtime detection, Authenticode verification and the IPC controllers.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlwapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

namespace fs = std::filesystem;
namespace sh = boson::shipping;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_shipping.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define SH_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

fs::path g_root;

std::string read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_all(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::wstring self_exe() {
    return boson::app::executable_path();
}

struct Child {
    HANDLE process = nullptr;
    DWORD pid = 0;
    ~Child() {
        if (process != nullptr) {
            CloseHandle(process);
        }
    }
    std::optional<DWORD> wait(DWORD ms) const {
        if (WaitForSingleObject(process, ms) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        return code;
    }
};

std::unique_ptr<Child> spawn_self(const std::vector<std::wstring>& args) {
    std::wstring cmd = boson::app::build_command_line(self_exe(), args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    auto child = std::make_unique<Child>();
    child->process = pi.hProcess;
    child->pid = pi.dwProcessId;
    return child;
}

// ---- Minimal HTTP/1.1 server on 127.0.0.1 (Range + If-Range + ETag, scripted connection aborts) ---------
class LoopbackServer {
public:
    LoopbackServer() {
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
        listen(m_listen, 8);
        m_thread = std::jthread([this]() { loop(); });
    }

    ~LoopbackServer() {
        m_stop = true;
        closesocket(m_listen);
        m_thread.join();
        WSACleanup();
    }

    void set(const std::string& path, std::string body, std::string etag = "\"v1\"") {
        std::scoped_lock lock(m_mutex);
        m_routes[path] = Route{std::move(body), std::move(etag)};
    }
    void abort_next(const std::string& path, std::size_t after_bytes) {
        std::scoped_lock lock(m_mutex);
        m_abort[path] = after_bytes;
    }
    [[nodiscard]] std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(m_port) + std::string(path);
    }
    std::atomic<int> range_requests{0};
    std::atomic<int> requests{0};

private:
    struct Route {
        std::string body;
        std::string etag;
    };

    void loop() {
        while (!m_stop) {
            SOCKET client = accept(m_listen, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                return;
            }
            handle(client);
        }
    }

    static std::string header_value(const std::string& head, std::string_view name) {
        std::string lower = head;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string key = "\r\n" + std::string(name) + ":";
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto pos = lower.find(key);
        if (pos == std::string::npos) return {};
        auto start = pos + key.size();
        auto end = head.find("\r\n", start);
        std::string v = head.substr(start, end - start);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        return v;
    }

    void handle(SOCKET client) {
        ++requests;
        std::string head;
        char buf[4096];
        while (head.find("\r\n\r\n") == std::string::npos) {
            const int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) {
                closesocket(client);
                return;
            }
            head.append(buf, static_cast<std::size_t>(n));
        }
        const auto sp1 = head.find(' ');
        const auto sp2 = head.find(' ', sp1 + 1);
        const std::string path = head.substr(sp1 + 1, sp2 - sp1 - 1);
        std::optional<Route> route;
        std::optional<std::size_t> abort_after;
        {
            std::scoped_lock lock(m_mutex);
            if (const auto it = m_routes.find(path); it != m_routes.end()) route = it->second;
            if (const auto it = m_abort.find(path); it != m_abort.end()) {
                abort_after = it->second;
                m_abort.erase(it);
            }
        }
        std::string response;
        std::string body;
        if (!route) {
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        } else {
            const std::string range = header_value(head, "Range");
            const std::string if_range = header_value(head, "If-Range");
            std::size_t start = 0;
            bool partial = false;
            if (range.starts_with("bytes=") && (if_range.empty() || if_range == route->etag)) {
                start = std::stoull(range.substr(6));
                partial = start > 0 && start < route->body.size();
                ++range_requests;
            }
            body = route->body.substr(partial ? start : 0);
            if (partial) {
                response = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(start) + "-" +
                           std::to_string(route->body.size() - 1) + "/" + std::to_string(route->body.size()) + "\r\n";
            } else {
                response = "HTTP/1.1 200 OK\r\n";
            }
            response += "Content-Length: " + std::to_string(body.size()) + "\r\nETag: " + route->etag +
                        "\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n";
        }
        send(client, response.data(), static_cast<int>(response.size()), 0);
        std::size_t to_send = abort_after.has_value() ? std::min(*abort_after, body.size()) : body.size();
        std::size_t off = 0;
        while (off < to_send) {
            const int n = send(client, body.data() + off, static_cast<int>(std::min<std::size_t>(to_send - off, 65536)), 0);
            if (n <= 0) break;
            off += static_cast<std::size_t>(n);
        }
        if (abort_after.has_value()) {
            linger l{1, 0}; // RST: an abrupt network failure mid-body
            setsockopt(client, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
        } else {
            shutdown(client, SD_SEND);
        }
        closesocket(client);
    }

    SOCKET m_listen = INVALID_SOCKET;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::mutex m_mutex;
    std::map<std::string, Route> m_routes;
    std::map<std::string, std::size_t> m_abort;
    std::jthread m_thread;
};

// ---- Child-process modes ---------------------------------------------------------------------------------
volatile int g_sink = 0;

int recurse_forever(int depth) {
    volatile char frame[4096];
    frame[0] = static_cast<char>(depth);
    frame[4095] = frame[0];
    if (g_sink >= 0) {
        return recurse_forever(depth + 1) + frame[4095];
    }
    return frame[0];
}

int run_crash_child(const fs::path& dir, std::wstring_view mode) {
    sh::CrashReporter reporter(sh::CrashReporterConfig{.dump_dir = dir, .app_name = L"BosonCrashTest", .app_version = "9.9.9"});
    if (!reporter.install()) {
        return 3;
    }
    if (mode == L"av") {
        volatile int* null_ptr = nullptr;
        *null_ptr = 42;
    } else if (mode == L"stack") {
        g_sink = recurse_forever(0);
    } else if (mode == L"terminate") {
        std::terminate();
    }
    return 4;
}

// ---- Tests -----------------------------------------------------------------------------------------------

void test_versions() {
    step("semantic versions");
    using sh::SemVer;
    SH_CHECK(SemVer::parse("1.2.3").has_value());
    SH_CHECK(SemVer::parse("v1.2.3-beta.1+build.5").has_value());
    SH_CHECK(SemVer::parse("1.2.3.4")->revision == 4);
    SH_CHECK(!SemVer::parse("").has_value());
    SH_CHECK(!SemVer::parse("1.2.3.4.5").has_value());
    SH_CHECK(!SemVer::parse("01.2.3").has_value());
    SH_CHECK(!SemVer::parse("1.2.x").has_value());
    SH_CHECK(!SemVer::parse("1.2.3-").has_value());
    SH_CHECK(!SemVer::parse("1.2.3-01").has_value());
    SH_CHECK(!SemVer::parse("1..3").has_value());
    // SemVer 2.0.0 section 11 precedence chain.
    const char* chain[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                           "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "1.1.0.1", "2.0.0", "10.0.0"};
    for (std::size_t i = 0; i + 1 < std::size(chain); ++i) {
        SH_CHECK(sh::compare_versions(chain[i], chain[i + 1]) == std::strong_ordering::less);
        SH_CHECK(sh::is_newer_version(chain[i + 1], chain[i]));
        SH_CHECK(!sh::is_newer_version(chain[i], chain[i + 1]));
    }
    SH_CHECK(sh::compare_versions("1.2", "1.2.0.0") == std::strong_ordering::equal);
    SH_CHECK(sh::compare_versions("1.0.0+a", "1.0.0+b") == std::strong_ordering::equal);
    SH_CHECK(!sh::compare_versions("1.0", "garbage").has_value());
    SH_CHECK(!sh::is_newer_version("garbage", "1.0"));
    SH_CHECK(SemVer::parse("v2.5")->to_string() == "2.5.0");
    SH_CHECK(SemVer::parse("1.2.3.4-rc.1+x")->to_string() == "1.2.3.4-rc.1+x");
    SH_CHECK(SemVer::parse("1.70000.3")->to_four_part() == "1.65535.3.0");
}

void test_crypto() {
    step("SHA-256, hex, base64");
    SH_CHECK(sh::to_hex(*sh::sha256(std::string_view{})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    SH_CHECK(sh::to_hex(*sh::sha256(std::string_view{"abc"})) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    {
        sh::Sha256 h;
        SH_CHECK(h.update(std::string_view{"ab"}) && h.update(std::string_view{"c"}));
        SH_CHECK(sh::to_hex(*h.finish()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        SH_CHECK(!h.finish().has_value());
    }
    const fs::path big = g_root / "big.bin";
    std::string big_data(3 * 1024 * 1024 + 17, '\0');
    for (std::size_t i = 0; i < big_data.size(); ++i) big_data[i] = static_cast<char>((i * 131) & 0xFF);
    write_all(big, big_data);
    SH_CHECK(sh::sha256_file(big) == sh::sha256(std::string_view{big_data}));
    SH_CHECK(!sh::sha256_file(g_root / "missing.bin").has_value());

    SH_CHECK(sh::from_hex("00ff10Ab") == sh::Bytes({0x00, 0xff, 0x10, 0xab}));
    SH_CHECK(!sh::from_hex("abc").has_value());
    SH_CHECK(!sh::from_hex("zz").has_value());
    SH_CHECK(sh::hex_equals("ABcd", "abCD"));
    SH_CHECK(!sh::hex_equals("abcd", "abce"));

    const sh::Bytes raw = {'h', 'e', 'l', 'l', 'o', '!', 0, 255};
    const std::string b64 = sh::base64_encode(raw);
    SH_CHECK(b64 == "aGVsbG8hAP8=");
    SH_CHECK(sh::base64_decode(b64) == raw);
    SH_CHECK(sh::base64_decode("  aGVsbG8hAP8=\n") == raw);
    SH_CHECK(!sh::base64_decode("aGVsbG8hAP8").has_value());
    SH_CHECK(!sh::base64_decode("aGVs*G8hAP8=").has_value());
    SH_CHECK(!sh::base64_decode("aG=sbG8h").has_value());
    SH_CHECK(sh::random_bytes(32)->size() == 32);

    step("ECDSA P-256 sign / verify / tamper");
    const auto keys = sh::ecdsa_p256_generate();
    SH_CHECK(keys.has_value());
    if (!keys) return;
    SH_CHECK(keys->public_key.size() == 64 && keys->private_key.size() == 96);
    const std::string message = sh::update_signing_payload("1.2.3", "AB" + std::string(62, '0'));
    SH_CHECK(message == "boson-update-v1\n1.2.3\nab" + std::string(62, '0') + "\n");
    const auto sig = sh::ecdsa_p256_sign(keys->private_key, sh::as_bytes(message));
    SH_CHECK(sig.has_value() && sig->size() == 64);
    if (!sig) return;
    SH_CHECK(sh::ecdsa_p256_verify(keys->public_key, sh::as_bytes(message), *sig));
    std::string tampered = message;
    tampered[17] ^= 1;
    SH_CHECK(!sh::ecdsa_p256_verify(keys->public_key, sh::as_bytes(tampered), *sig));
    auto bad_sig = *sig;
    bad_sig[10] ^= 0x40;
    SH_CHECK(!sh::ecdsa_p256_verify(keys->public_key, sh::as_bytes(message), bad_sig));
    const auto other = sh::ecdsa_p256_generate();
    SH_CHECK(other && !sh::ecdsa_p256_verify(other->public_key, sh::as_bytes(message), *sig));
    SH_CHECK(!sh::ecdsa_p256_verify(std::span<const std::uint8_t>(keys->public_key.data(), 63), sh::as_bytes(message), *sig));
    SH_CHECK(!sh::ecdsa_p256_verify(keys->public_key, sh::as_bytes(message), std::span<const std::uint8_t>(sig->data(), 63)));
    SH_CHECK(!sh::ecdsa_p256_sign(std::span<const std::uint8_t>(keys->private_key.data(), 95), sh::as_bytes(message)).has_value());
    // ECDSA is randomized: two signatures differ but both verify.
    const auto sig2 = sh::ecdsa_p256_sign(keys->private_key, sh::as_bytes(message));
    SH_CHECK(sig2 && *sig2 != *sig && sh::ecdsa_p256_verify(keys->public_key, sh::as_bytes(message), *sig2));
}

void test_manifest() {
    step("manifest parsing, package selection, rollout");
    const std::string hash(64, 'a');
    const std::string json = R"({"version":"1.4.0","url":"pkg/app-1.4.0.zip","sha256":")" + hash +
                             R"(","signature":"c2ln","size":10,"notes":"Hi","mandatory":false,"rollout":25.5,"unknown_key":[1,2],
        "packages":{"msi":{"url":"https://cdn.example.com/app.msi","sha256":")" + hash + R"(","signature":"x"}}})";
    const auto parsed = sh::parse_update_manifest(json, "https://updates.example.com/stable/manifest.json");
    SH_CHECK(parsed.manifest.has_value());
    if (parsed.manifest) {
        SH_CHECK(parsed.manifest->url == "https://updates.example.com/stable/pkg/app-1.4.0.zip");
        SH_CHECK(parsed.manifest->rollout == 25.5);
        SH_CHECK(parsed.manifest->size == std::optional<std::uint64_t>{10});
        SH_CHECK(parsed.manifest->packages.at("msi").url == "https://cdn.example.com/app.msi");
        SH_CHECK(parsed.manifest->notes == "Hi");
    }
    SH_CHECK(!sh::parse_update_manifest(R"({"version":"x","url":"a","sha256":")" + hash + R"("})").manifest);
    SH_CHECK(!sh::parse_update_manifest(R"({"version":"1.0.0","url":"a","sha256":"abc"})").manifest);
    SH_CHECK(!sh::parse_update_manifest(R"({"version":"1.0.0"})").manifest);
    SH_CHECK(!sh::parse_update_manifest(R"({"version":"1.0.0","url":"a","sha256":")" + hash + R"(","rollout":150})").manifest);
    SH_CHECK(!sh::parse_update_manifest(R"({"version":"1.0.0","url":"a","sha256":")" + hash + R"(","kind":"deb"})").manifest);
    SH_CHECK(!sh::parse_update_manifest("not json").manifest);
    SH_CHECK(!sh::parse_update_manifest("not json").error.empty());

    SH_CHECK(sh::package_kind_from_url("https://x/y/app.MSI?sig=1") == sh::PackageKind::Msi);
    SH_CHECK(sh::package_kind_from_url("https://x/y/app.zip") == sh::PackageKind::Zip);
    SH_CHECK(sh::package_kind_from_url("https://x/y.d/app") == sh::PackageKind::Auto);

    SH_CHECK(sh::rollout_bucket("install-a", "1.0.0") == sh::rollout_bucket("install-a", "1.0.0"));
    SH_CHECK(sh::rollout_bucket("install-a", "1.0.0") < 10000);
    SH_CHECK(!sh::in_rollout(0.0, "a", "1"));
    SH_CHECK(sh::in_rollout(100.0, "a", "1"));
    int inside = 0;
    for (int i = 0; i < 4000; ++i) {
        inside += sh::in_rollout(30.0, "id-" + std::to_string(i), "2.0.0") ? 1 : 0;
    }
    SH_CHECK(inside > 1000 && inside < 1400); // ~30 % of 4000
    const std::string id = sh::default_install_id();
    SH_CHECK(id.size() == 64 && id == sh::default_install_id());
}

void test_logger() {
    step("async rotating logger");
    const fs::path dir = g_root / "logs";
    {
        sh::Logger logger(sh::LoggerConfig{
            .directory = dir, .file_name = L"app.log", .max_file_bytes = 2048, .max_files = 3, .min_level = sh::LogLevel::Debug,
            .mirror_to_debugger = false});
        logger.info("test", "hello {} {}", 1, "two");
        logger.log(sh::LogLevel::Trace, "test", "filtered out");
        logger.error("test", "line\nbreak");
        logger.flush();
        const std::string first = read_all(dir / "app.log");
        SH_CHECK(first.find("INFO  [test] hello 1 two") != std::string::npos);
        SH_CHECK(first.find("filtered out") == std::string::npos);
        SH_CHECK(first.find("ERROR [test] line break") != std::string::npos);
        std::vector<std::jthread> writers;
        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&logger, t]() {
                for (int i = 0; i < 100; ++i) logger.info("mt", "thread {} line {} {}", t, i, std::string(40, 'x'));
            });
        }
        writers.clear();
        logger.flush();
        SH_CHECK(logger.dropped_lines() == 0);
        SH_CHECK(fs::exists(dir / "app.1.log"));
        SH_CHECK(fs::exists(dir / "app.2.log"));
        SH_CHECK(!fs::exists(dir / "app.3.log"));
        SH_CHECK(fs::file_size(dir / "app.log") <= 2048);
        SH_CHECK(fs::file_size(dir / "app.1.log") <= 2048);
    }
    SH_CHECK(sh::parse_log_level("WARNING") == sh::LogLevel::Warn);
    SH_CHECK(sh::parse_log_level("nope", sh::LogLevel::Error) == sh::LogLevel::Error);
}

void test_http(LoopbackServer& server) {
    step("WinHTTP GET / download / resume / cancel against a loopback server");
    sh::HttpOptions insecure;
    insecure.allow_insecure_loopback = true;
    server.set("/hello.json", R"({"ok":true})");
    {
        const auto r = sh::http_get(server.url("/hello.json"), insecure);
        SH_CHECK(r.ok && r.status == 200 && r.body == R"({"ok":true})");
        SH_CHECK(r.etag == "\"v1\"");
    }
    {
        const auto r = sh::http_get(server.url("/hello.json")); // HTTPS required by default
        SH_CHECK(!r.ok && r.error.find("HTTPS") != std::string::npos);
        const auto missing = sh::http_get(server.url("/missing"), insecure);
        SH_CHECK(!missing.ok && missing.status == 404);
        SH_CHECK(!sh::check_url_allowed("https://example.com/a", {}).size());
        SH_CHECK(sh::check_url_allowed("file:///C:/x", {}).size() > 0);
        SH_CHECK(sh::resolve_url("https://a.com/x/m.json", "../p.zip") == "https://a.com/p.zip");
    }

    std::string payload(2 * 1024 * 1024 + 123, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>((i * 7 + 3) & 0xFF);
    server.set("/payload.bin", payload, "\"etag-payload\"");
    const fs::path dest = g_root / "dl" / "payload.bin";

    server.abort_next("/payload.bin", 700 * 1024);
    std::uint64_t last_progress = 0;
    const auto first = sh::download_file(server.url("/payload.bin"), dest, insecure,
                                         [&](const sh::DownloadProgress& p) { last_progress = p.received; });
    SH_CHECK(!first.ok);
    SH_CHECK(!fs::exists(dest));
    SH_CHECK(fs::exists(fs::path(dest).concat(L".partial")));
    const auto partial_size = fs::file_size(fs::path(dest).concat(L".partial"));
    SH_CHECK(partial_size > 0 && partial_size <= 700 * 1024);
    std::cout << "  interrupted after " << partial_size << " bytes: " << first.error << '\n';

    int progress_calls = 0;
    sh::DownloadProgress final_progress;
    const int ranges_before = server.range_requests.load();
    const auto second = sh::download_file(server.url("/payload.bin"), dest, insecure, [&](const sh::DownloadProgress& p) {
        ++progress_calls;
        final_progress = p;
    });
    SH_CHECK(second.ok && second.resumed && second.status == 206);
    SH_CHECK(server.range_requests.load() == ranges_before + 1);
    SH_CHECK(final_progress.received == payload.size() && final_progress.total == std::optional<std::uint64_t>{payload.size()});
    SH_CHECK(final_progress.resumed_from == partial_size);
    SH_CHECK(progress_calls >= 2);
    SH_CHECK(read_all(dest) == payload);
    SH_CHECK(!fs::exists(fs::path(dest).concat(L".partial")) && !fs::exists(fs::path(dest).concat(L".partial.json")));

    // Changed file on the server (new ETag): If-Range makes the server send the whole new file.
    server.abort_next("/payload.bin", 100 * 1024);
    fs::remove(dest);
    (void)sh::download_file(server.url("/payload.bin"), dest, insecure);
    std::string payload2 = payload;
    payload2[5] = 'Z';
    server.set("/payload.bin", payload2, "\"etag-2\"");
    const auto third = sh::download_file(server.url("/payload.bin"), dest, insecure);
    SH_CHECK(third.ok && !third.resumed && third.status == 200);
    SH_CHECK(read_all(dest) == payload2);

    // Size pinning.
    const auto wrong = sh::download_file(server.url("/payload.bin"), g_root / "dl" / "wrong.bin", insecure, {}, {},
                                         sh::DownloadOptions{.expected_size = 5});
    SH_CHECK(!wrong.ok && wrong.error.find("size mismatch") != std::string::npos);

    // Cancellation from another thread.
    std::stop_source stop;
    bool cancelled_in_cb = false;
    const auto cancelled = sh::download_file(server.url("/payload.bin"), g_root / "dl" / "cancel.bin", insecure,
        [&](const sh::DownloadProgress& p) {
            if (p.received > 0 && !cancelled_in_cb) {
                cancelled_in_cb = true;
                stop.request_stop();
            }
        },
        stop.get_token(), sh::DownloadOptions{.progress_interval = std::chrono::milliseconds{0}});
    SH_CHECK(!cancelled.ok && cancelled.cancelled);

    // file:// source.
    sh::HttpOptions files;
    files.allow_file_urls = true;
    wchar_t url_buf[2048]{};
    DWORD url_len = 2048;
    UrlCreateFromPathW((g_root / "big.bin").c_str(), url_buf, &url_len, 0);
    const auto from_file = sh::download_file(boson::core::wide_to_utf8(url_buf), g_root / "dl" / "copy.bin", files);
    SH_CHECK(from_file.ok && read_all(g_root / "dl" / "copy.bin") == read_all(g_root / "big.bin"));
}

struct SignedPackage {
    std::string sha256;
    std::string signature;
};

SignedPackage sign_file(const sh::EcdsaP256KeyPair& keys, const fs::path& file, std::string_view version) {
    SignedPackage out;
    out.sha256 = sh::to_hex(*sh::sha256_file(file));
    const auto sig = sh::ecdsa_p256_sign(keys.private_key, sh::as_bytes(sh::update_signing_payload(version, out.sha256)));
    out.signature = sh::base64_encode(*sig);
    return out;
}

void test_portable_rollback() {
    step("portable file swap with rollback on a locked file");
    const fs::path staged = g_root / "rb" / "staged";
    const fs::path install = g_root / "rb" / "install";
    const fs::path backup = install / ".boson-update-backup";
    write_all(staged / "a.txt", "A2");
    write_all(staged / "b.txt", "B2");
    write_all(staged / "c.txt", "C2-new");
    write_all(install / "a.txt", "A1");
    write_all(install / "b.txt", "B1");
    // b.txt is opened without FILE_SHARE_DELETE, so it cannot be renamed aside.
    const HANDLE lock = CreateFileW((install / "b.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    SH_CHECK(lock != INVALID_HANDLE_VALUE);
    const auto r = sh::apply_portable_update(staged, install, backup, std::chrono::milliseconds{300});
    CloseHandle(lock);
    SH_CHECK(!r.ok && r.rolled_back);
    SH_CHECK(read_all(install / "a.txt") == "A1");
    SH_CHECK(read_all(install / "b.txt") == "B1");
    SH_CHECK(!fs::exists(install / "c.txt"));
    SH_CHECK(!fs::exists(backup));
    const auto ok = sh::apply_portable_update(staged, install, backup);
    SH_CHECK(ok.ok && ok.files_replaced == 3);
    SH_CHECK(read_all(install / "b.txt") == "B2" && read_all(install / "c.txt") == "C2-new");
    SH_CHECK(!fs::exists(backup));
}

void test_updater(LoopbackServer& server) {
    step("Updater: check, download + verify, tamper, helper install (zip and exe)");
    const auto keys = sh::ecdsa_p256_generate();
    SH_CHECK(keys.has_value());
    if (!keys) return;

    const fs::path src = g_root / "upd" / "src";
    write_all(src / "app.txt", "version 1.1.0");
    write_all(src / "data" / "new.txt", "brand new");
    const fs::path zip = g_root / "upd" / "app-1.1.0.zip";
    {
        wchar_t sys[MAX_PATH]{};
        GetSystemDirectoryW(sys, MAX_PATH);
        const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
        std::wstring cmd = boson::app::build_command_line(tar, std::vector<std::wstring>{L"-a", L"-c", L"-f", zip.wstring(), L"-C", src.wstring(), L"app.txt", L"data"});
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        SH_CHECK(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    SH_CHECK(fs::exists(zip));
    const SignedPackage signed_zip = sign_file(*keys, zip, "1.1.0");
    server.set("/pkg/app-1.1.0.zip", read_all(zip), "\"zip1\"");
    const std::string manifest = R"({"version":"1.1.0","url":"pkg/app-1.1.0.zip","sha256":")" + signed_zip.sha256 +
                                 R"(","signature":")" + signed_zip.signature + R"(","size":)" + std::to_string(fs::file_size(zip)) +
                                 R"(,"notes":"Zip update","rollout":100})";
    server.set("/stable/manifest.json", "");
    server.set("/manifest.json", manifest);

    const fs::path install = g_root / "upd" / "install";
    write_all(install / "app.txt", "version 1.0.0");
    write_all(install / "keep.txt", "untouched");

    sh::UpdaterConfig cfg;
    cfg.manifest_url = server.url("/manifest.json");
    cfg.current_version = "1.0.0";
    cfg.public_key = sh::base64_encode(keys->public_key);
    cfg.work_dir = g_root / "upd" / "work";
    cfg.install_id = "test-install";
    cfg.install_kind = sh::PackageKind::Zip;
    cfg.install_dir = install;
    cfg.http.allow_insecure_loopback = true;
    sh::Updater updater(cfg);

    const auto checked = updater.check();
    SH_CHECK(checked.ok);
    SH_CHECK(checked.info.available && checked.info.newer && !checked.info.mandatory);
    SH_CHECK(checked.info.kind == sh::PackageKind::Zip);
    SH_CHECK(checked.info.package.url == server.url("/pkg/app-1.1.0.zip"));
    SH_CHECK(checked.info.notes == "Zip update");

    std::uint64_t seen = 0;
    const auto downloaded = updater.download(checked.info, [&](const sh::DownloadProgress& p) { seen = p.received; });
    SH_CHECK(downloaded.ok);
    if (!downloaded.ok) std::cerr << "  download error: " << downloaded.error << '\n';
    SH_CHECK(seen == fs::file_size(zip));
    SH_CHECK(read_all(downloaded.update.staged_dir / "app.txt") == "version 1.1.0");
    SH_CHECK(updater.verify_package(checked.info, downloaded.update.package));

    // Tampering: a valid signature for another version, a flipped signature, a wrong key, a wrong hash.
    {
        auto info = checked.info;
        info.version = "1.2.0";
        std::string err;
        SH_CHECK(!updater.verify_package(info, downloaded.update.package, &err) && err.find("signature") != std::string::npos);
        info = checked.info;
        auto sig = *sh::base64_decode(info.package.signature);
        sig[3] ^= 1;
        info.package.signature = sh::base64_encode(sig);
        SH_CHECK(!updater.verify_package(info, downloaded.update.package));
        info = checked.info;
        info.package.sha256 = std::string(64, '0');
        SH_CHECK(!updater.verify_package(info, downloaded.update.package, &err) && err.find("SHA-256") != std::string::npos);
        auto other_cfg = cfg;
        other_cfg.public_key = sh::base64_encode(sh::ecdsa_p256_generate()->public_key);
        sh::Updater other(other_cfg);
        SH_CHECK(!other.verify_package(checked.info, downloaded.update.package));
        auto unsigned_cfg = cfg;
        unsigned_cfg.public_key.clear();
        sh::Updater unsigned_updater(unsigned_cfg);
        SH_CHECK(!unsigned_updater.verify_package(checked.info, downloaded.update.package));
        // A server-side swap of the package bytes is caught by download().
        server.set("/evil.zip", "not the package");
        auto evil = checked.info;
        evil.package.url = server.url("/evil.zip");
        evil.package.size.reset();
        evil.version = "1.1.1";
        const auto bad = updater.download(evil);
        SH_CHECK(!bad.ok && bad.error.find("SHA-256") != std::string::npos);
    }

    // Up to date / rollout / mandatory.
    {
        auto c2 = cfg;
        c2.current_version = "1.1.0";
        sh::Updater same(c2);
        const auto r = same.check();
        SH_CHECK(r.ok && !r.info.available && !r.info.newer);
        server.set("/m0.json", R"({"version":"1.1.0","url":"pkg/app-1.1.0.zip","sha256":")" + signed_zip.sha256 + R"(","rollout":0,"minimum_version":"0.9.0"})");
        c2 = cfg;
        c2.manifest_url = server.url("/m0.json");
        sh::Updater none(c2);
        const auto r0 = none.check();
        SH_CHECK(r0.ok && r0.info.newer && !r0.info.in_rollout && !r0.info.available);
        c2.current_version = "0.5.0";
        sh::Updater old(c2);
        const auto r1 = old.check();
        SH_CHECK(r1.ok && r1.info.mandatory && r1.info.available);
        c2.manifest_url = server.url("/missing.json");
        sh::Updater missing(c2);
        SH_CHECK(!missing.check().ok);
    }

    // The helper (a copy of this test executable) waits for a sleeping child, then swaps the files.
    {
        auto sleeper = spawn_self({L"--sleep-ms=1500"});
        SH_CHECK(sleeper != nullptr);
        if (!sleeper) return;
        auto helper_cfg = cfg;
        helper_cfg.helper_executable = self_exe();
        sh::Updater installer(helper_cfg);
        std::string err;
        const bool scheduled = installer.schedule_install(downloaded.update,
            sh::InstallOptions{.relaunch = false, .wait_timeout = std::chrono::seconds{30}, .wait_pid = sleeper->pid}, &err);
        SH_CHECK(scheduled);
        if (!scheduled) std::cerr << "  schedule error: " << err << '\n';
        SH_CHECK(installer.has_scheduled_install());
        SH_CHECK(read_all(install / "app.txt") == "version 1.0.0"); // not before the app exits
        const auto code = installer.wait_for_scheduled_install(std::chrono::seconds{60});
        SH_CHECK(code == std::optional<std::uint32_t>{0});
        SH_CHECK(sleeper->wait(0).has_value());
        SH_CHECK(read_all(install / "app.txt") == "version 1.1.0");
        SH_CHECK(read_all(install / "data" / "new.txt") == "brand new");
        SH_CHECK(read_all(install / "keep.txt") == "untouched");
        SH_CHECK(!fs::exists(install / ".boson-update-backup"));
        const auto result = installer.take_last_result();
        SH_CHECK(result.has_value() && result->success && result->version == "1.1.0" && result->kind == "zip");
        SH_CHECK(!installer.take_last_result().has_value());
    }

    // Installer-exe flavour: the "installer" is this executable in --fake-installer mode.
    {
        const fs::path fake = g_root / "upd" / "fake-setup.exe";
        fs::copy_file(self_exe(), fake, fs::copy_options::overwrite_existing);
        const SignedPackage signed_exe = sign_file(*keys, fake, "2.0.0");
        const fs::path marker = g_root / "upd" / "installer-ran.txt";
        sh::UpdateInfo info;
        info.version = "2.0.0";
        info.kind = sh::PackageKind::Exe;
        info.available = true;
        info.package = sh::UpdatePackage{.url = "https://example.invalid/setup.exe", .sha256 = signed_exe.sha256, .signature = signed_exe.signature,
                                         .installer_args = "--fake-installer=\"" + boson::core::wide_to_utf8(marker.wstring()) + "\""};
        auto exe_cfg = cfg;
        exe_cfg.helper_executable = self_exe();
        sh::Updater installer(exe_cfg);
        sh::DownloadedUpdate update{.info = info, .package = fake};
        SH_CHECK(installer.schedule_install(update, sh::InstallOptions{.relaunch = false, .wait_pid = GetCurrentProcessId() + 1000003u}));
        SH_CHECK(installer.wait_for_scheduled_install(std::chrono::seconds{60}) == std::optional<std::uint32_t>{0});
        SH_CHECK(read_all(marker) == "installed");
        const auto result = installer.take_last_result();
        SH_CHECK(result && result->success && result->kind == "exe");

        // cancel_scheduled_install(): the helper waits for us (forever) and is killed.
        sh::DownloadedUpdate again{.info = info, .package = g_root / "upd" / "fake-setup2.exe"};
        fs::copy_file(self_exe(), again.package, fs::copy_options::overwrite_existing);
        const SignedPackage s2 = sign_file(*keys, again.package, "2.0.0");
        again.info.package.sha256 = s2.sha256;
        again.info.package.signature = s2.signature;
        fs::remove(marker);
        SH_CHECK(installer.schedule_install(again, sh::InstallOptions{.relaunch = false}));
        SH_CHECK(installer.has_scheduled_install());
        SH_CHECK(installer.cancel_scheduled_install());
        SH_CHECK(!installer.has_scheduled_install());
        SH_CHECK(!fs::exists(marker));
    }
}

void test_crash_reporter() {
    step("crash reporter: dumps from crashing child processes");
    const struct {
        const wchar_t* mode;
        DWORD code;
        const char* reason;
    } cases[] = {
        {L"av", 0xC0000005, "exception"},
        {L"stack", 0xC00000FD, "exception"},
        {L"terminate", 0xE0B05001, "terminate"},
    };
    for (const auto& c : cases) {
        const fs::path dir = g_root / "crash" / c.mode;
        auto child = spawn_self({std::wstring(L"--crash-child=") + dir.wstring(), std::wstring(L"--mode=") + c.mode});
        SH_CHECK(child != nullptr);
        if (!child) continue;
        const auto code = child->wait(60000);
        SH_CHECK(code == std::optional<DWORD>{c.code});
        sh::CrashReporter reader(sh::CrashReporterConfig{.dump_dir = dir, .app_name = L"BosonCrashTest"});
        const auto reports = reader.pending_reports();
        SH_CHECK(reports.size() == 1);
        if (reports.size() == 1) {
            const auto& r = reports.front();
            SH_CHECK(r.dump_bytes > 1024);
            SH_CHECK(read_all(r.dump).substr(0, 4) == "MDMP");
            SH_CHECK(r.reason == c.reason);
            SH_CHECK(r.exception_code == c.code);
            SH_CHECK(r.app_version == "9.9.9" && r.app_name == "BosonCrashTest");
            SH_CHECK(r.pid == child->pid);
            std::wcout << L"  " << c.mode << L": " << r.dump.filename().wstring() << L" (" << r.dump_bytes << L" bytes)\n";
        }
        std::vector<std::string> uploaded;
        SH_CHECK(reader.process_pending_reports([&](const sh::CrashReport& r) { uploaded.push_back(r.reason); return true; }) == 1);
        SH_CHECK(uploaded.size() == 1);
        SH_CHECK(reader.pending_reports().empty());
    }

    step("crash reporter: in-process manual dump, pruning, ProcessFailed logging");
    const fs::path dir = g_root / "crash" / "manual";
    {
        sh::CrashReporter reporter(sh::CrashReporterConfig{.dump_dir = dir, .app_name = L"Manual", .max_dumps = 2});
        SH_CHECK(reporter.install());
        SH_CHECK(reporter.installed());
        sh::CrashReporter second(sh::CrashReporterConfig{.dump_dir = dir});
        SH_CHECK(!second.install());
        for (int i = 0; i < 3; ++i) {
            const auto path = reporter.write_dump_now("manual");
            SH_CHECK(!path.empty() && fs::exists(path));
            Sleep(5);
        }
        SH_CHECK(reporter.pending_reports().size() == 3);
        SH_CHECK(reporter.pending_reports().front().reason == "manual");
        reporter.uninstall();
        SH_CHECK(!reporter.installed());
        sh::CrashReporter pruner(sh::CrashReporterConfig{.dump_dir = dir, .max_dumps = 2});
        SH_CHECK(pruner.install());
        SH_CHECK(pruner.pending_reports().size() == 2);

        boson::core::WebView webview(boson::core::WebViewConfig{});
        int previous_calls = 0;
        int extra_calls = 0;
        webview.events().on_process_failed = [&](const boson::core::ProcessFailedEvent&) { ++previous_calls; };
        pruner.attach_webview(webview, [&](const boson::core::ProcessFailedEvent&) { ++extra_calls; });
        const boson::core::ProcessFailedEvent event{
            .kind = boson::core::ProcessFailedKind::GpuProcessExited,
            .reason = COREWEBVIEW2_PROCESS_FAILED_REASON_CRASHED,
            .exit_code = static_cast<int>(0xC0000005),
            .process_description = "GPU",
        };
        webview.events().on_process_failed(event);
        SH_CHECK(previous_calls == 1 && extra_calls == 1);
        const std::string text = sh::CrashReporter::describe(event);
        SH_CHECK(text == "GPU process exited (reason: crashed, exit code 0xC0000005, GPU)");
        pruner.config().logger->flush();
        SH_CHECK(read_all(dir / "crash.log").find(text) != std::string::npos);
    }
}

void test_runtime_and_authenticode() {
    step("WebView2 runtime detection, consent, Authenticode");
    const auto version = sh::installed_webview2_version();
    std::wcout << L"  WebView2 runtime: " << (version ? *version : std::wstring(L"(none)")) << L'\n';
    const auto ensured = sh::ensure_webview2_runtime();
    SH_CHECK(ensured.ok() == version.has_value());
    if (version) {
        SH_CHECK(ensured.status == sh::RuntimeStatus::AlreadyInstalled && ensured.version == *version);
    }
    int asked = 0;
    sh::RuntimeBootstrapOptions declined;
    declined.assume_missing = true;
    declined.consent = [&]() { ++asked; return false; };
    const auto r = sh::ensure_webview2_runtime(declined);
    SH_CHECK(asked == 1 && r.status == sh::RuntimeStatus::ConsentDeclined && !r.ok());
    sh::RuntimeBootstrapOptions refused;
    refused.assume_missing = true;
    refused.bootstrapper_url = "http://example.invalid/setup.exe"; // plain HTTP is refused before any I/O
    const auto r2 = sh::ensure_webview2_runtime(refused);
    SH_CHECK(r2.status == sh::RuntimeStatus::DownloadFailed && r2.error.find("HTTPS") != std::string::npos);

    // A Microsoft-signed binary with an embedded signature: the WebView2 runtime's own executable.
    std::optional<fs::path> signed_binary;
    if (version) {
        for (const wchar_t* base : {L"C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application", L"C:\\Program Files\\Microsoft\\EdgeWebView\\Application"}) {
            const fs::path p = fs::path(base) / *version / L"msedgewebview2.exe";
            if (fs::exists(p)) {
                signed_binary = p;
                break;
            }
        }
    }
    if (signed_binary) {
        const auto good = sh::verify_authenticode(*signed_binary, L"Microsoft Corporation");
        std::wcout << L"  " << signed_binary->filename().wstring() << L" signed by " << good.signer << L'\n';
        SH_CHECK(good.trusted && good.ok());
        const auto wrong = sh::verify_authenticode(*signed_binary, L"Contoso Ltd");
        SH_CHECK(wrong.trusted && !wrong.ok());
    } else {
        std::cout << "  (no msedgewebview2.exe found; skipping the trusted-signature check)\n";
    }
    write_all(g_root / "unsigned.exe", "MZ not really");
    SH_CHECK(!sh::verify_authenticode(g_root / "unsigned.exe").trusted);
    SH_CHECK(!sh::verify_authenticode(self_exe()).trusted);
}

void test_controllers(LoopbackServer& server) {
    step("Log / Updater IPC controllers through the Router");
    auto logger = std::make_shared<sh::Logger>(sh::LoggerConfig{.directory = g_root / "ctl", .file_name = L"web.log", .mirror_to_debugger = false});
    boson::ipc::LogController log_controller(logger);

    const auto keys = sh::ecdsa_p256_generate();
    const std::string body(50000, 'p');
    server.set("/ctl/app.zip", body);
    const std::string sha = sh::to_hex(*sh::sha256(std::string_view{body}));
    const auto sig = sh::ecdsa_p256_sign(keys->private_key, sh::as_bytes(sh::update_signing_payload("3.0.0", sha)));
    server.set("/ctl/manifest.json", R"({"version":"3.0.0","url":"app.zip","kind":"exe","sha256":")" + sha + R"(","signature":")" +
                                         sh::base64_encode(*sig) + R"(","notes":"ctl"})");

    sh::UpdaterConfig cfg;
    cfg.manifest_url = server.url("/ctl/manifest.json");
    cfg.current_version = "2.9.9";
    cfg.public_key = sh::base64_encode(keys->public_key);
    cfg.work_dir = g_root / "ctl" / "work";
    cfg.install_id = "ctl";
    cfg.http.allow_insecure_loopback = true;
    sh::Updater updater(cfg);

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([]() {});
    boson::exec::TaskRunner runner(scheduler, marshal);
    // Events go nowhere here (no WebView attached); the bridge only needs to exist.
    boson::ipc::Bridge bridge(runner, marshal, [](std::string_view, boson::ipc::Bridge::ReplyFn) {});
    boson::ipc::UpdaterController updater_controller(updater, bridge, runner);
    boson::ipc::Router<boson::ipc::LogController, boson::ipc::UpdaterController> router(log_controller, updater_controller);

    const std::string logged = router.dispatch_sync(R"({"id":1,"method":"Log.write","params":{"level":"warn","category":"ui","message":"hello from js"}})");
    SH_CHECK(logged.find("\"success\"") != std::string::npos);
    logger->flush();
    SH_CHECK(read_all(g_root / "ctl" / "web.log").find("WARN  [web:ui] hello from js") != std::string::npos);

    const std::string early = router.dispatch_sync(R"({"id":2,"method":"Updater.download","params":null})");
    SH_CHECK(early.find("call check() first") != std::string::npos);
    const std::string checked = router.dispatch_sync(R"({"id":3,"method":"Updater.check","params":null})");
    SH_CHECK(checked.find("\"available\":true") != std::string::npos);
    SH_CHECK(checked.find("\"version\":\"3.0.0\"") != std::string::npos);
    SH_CHECK(checked.find("\"kind\":\"exe\"") != std::string::npos);
    const std::string started = router.dispatch_sync(R"({"id":4,"method":"Updater.download","params":null})");
    SH_CHECK(started.find("\"started\":true") != std::string::npos);
    std::string status;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (std::chrono::steady_clock::now() < deadline) {
        status = router.dispatch_sync(R"({"id":5,"method":"Updater.status","params":null})");
        if (status.find("\"state\":\"ready\"") != std::string::npos || status.find("\"state\":\"error\"") != std::string::npos) {
            break;
        }
        Sleep(20);
    }
    SH_CHECK(status.find("\"state\":\"ready\"") != std::string::npos);
    SH_CHECK(status.find("\"received\":50000") != std::string::npos);
    const std::string last = router.dispatch_sync(R"({"id":6,"method":"Updater.last_result","params":null})");
    SH_CHECK(last.find("\"has_result\":false") != std::string::npos);
    const std::string bad = router.dispatch_sync(R"({"id":7,"method":"Updater.install","params":{"relaunch":false,"quit_now":false}})");
    SH_CHECK(bad.find("\"scheduled\":") != std::string::npos);
    updater.cancel_scheduled_install();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (const auto helper = sh::Updater::run_helper_if_requested()) {
        return *helper;
    }
    std::optional<fs::path> crash_dir;
    std::wstring crash_mode = L"av";
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg.starts_with(L"--sleep-ms=")) {
            Sleep(static_cast<DWORD>(std::stoul(std::wstring(arg.substr(11)))));
            return 0;
        }
        if (arg.starts_with(L"--fake-installer=")) {
            std::ofstream(fs::path(arg.substr(17)), std::ios::binary) << "installed";
            return 0;
        }
        if (arg.starts_with(L"--crash-child=")) crash_dir = fs::path(arg.substr(14));
        if (arg.starts_with(L"--mode=")) crash_mode = std::wstring(arg.substr(7));
    }
    if (crash_dir) {
        return run_crash_child(*crash_dir, crash_mode);
    }

    g_root = fs::temp_directory_path() / (L"boson-shipping-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(g_root);
    fs::create_directories(g_root);

    test_versions();
    test_crypto();
    test_manifest();
    test_logger();
    {
        LoopbackServer server;
        test_http(server);
        test_portable_rollback();
        test_updater(server);
        test_controllers(server);
    }
    test_crash_reporter();
    test_runtime_and_authenticode();

    std::error_code ec;
    fs::remove_all(g_root, ec);
    std::cout << (g_checks - g_failures) << "/" << g_checks << " shipping checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
