// Native app services: hashing vectors, ZIP reading / hostile archives, staged installs (rollback and crash
// recovery in a child process), temp-dir cleanup, the WinHTTP client against an in-process loopback server
// (redirects, gzip, chunked, timeouts, retries, cancel, size caps, cache, proxy) and boson::db on every
// linked backend (CRUD, key/value, transactions, concurrency, crash recovery in a child process).
// Headless and offline; everything lives under %TEMP%.
#include "support/http_test_server.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <boson/app/lifecycle.hpp>
#include <boson/archive/zip.hpp>
#include <boson/crypto/hash.hpp>
#include <boson/db/database.hpp>
#include <boson/db/kv_store.hpp>
#include <boson/db/table.hpp>
#include <boson/fs/install_transaction.hpp>
#include <boson/fs/known_folders.hpp>
#include <boson/net/http_client.hpp>
#include <boson/boson.hpp>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using boson::testing::HttpTestServer;
using boson::testing::TestRequest;
using boson::testing::TestResponse;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_native_services.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define NS_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) { std::cout << "[step] " << name << '\n' << std::flush; }

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

std::span<const std::byte> bytes_of(std::string_view s) {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

struct Child {
    HANDLE process = nullptr;
    ~Child() {
        if (process != nullptr) CloseHandle(process);
    }
};

std::unique_ptr<Child> spawn_self(const std::wstring& args) {
    std::wstring cmd = L"\"" + boson::app::executable_path() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    auto child = std::make_unique<Child>();
    child->process = pi.hProcess;
    return child;
}

bool wait_for_file(const fs::path& p, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        std::error_code ec;
        if (fs::exists(p, ec)) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

// ---- hashing ----------------------------------------------------------------------------------------------

void test_hashing() {
    step("hashing: MD5 / SHA-1 / SHA-256 / SHA-512 vectors, streaming, files");
    using boson::crypto::HashAlgorithm;
    NS_CHECK(boson::crypto::hash(HashAlgorithm::Md5, "abc")->hex() == "900150983cd24fb0d6963f7d28e17f72");
    NS_CHECK(boson::crypto::hash(HashAlgorithm::Md5, "")->hex() == "d41d8cd98f00b204e9800998ecf8427e");
    NS_CHECK(boson::crypto::hash(HashAlgorithm::Sha1, "abc")->hex() == "a9993e364706816aba3e25717850c26c9cd0d89d");
    NS_CHECK(boson::crypto::hash(HashAlgorithm::Sha256, "abc")->hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    NS_CHECK(boson::crypto::hash(HashAlgorithm::Sha256, "")->hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    NS_CHECK(boson::crypto::hash(HashAlgorithm::Sha512, "abc")->hex().starts_with("ddaf35a193617aba"));
    // 1,000,000 x 'a' streamed in uneven chunks.
    boson::crypto::Hasher sha1(HashAlgorithm::Sha1);
    const std::string chunk(997, 'a');
    std::size_t left = 1'000'000;
    while (left > 0) {
        const std::size_t n = std::min(left, chunk.size());
        sha1.update(std::string_view(chunk.data(), n));
        left -= n;
    }
    NS_CHECK(sha1.finish()->hex() == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    // Reusable after finish().
    sha1.update(std::string_view("abc"));
    NS_CHECK(sha1.finish()->hex() == "a9993e364706816aba3e25717850c26c9cd0d89d");
    const auto d = boson::crypto::hash(HashAlgorithm::Sha256, "abc");
    NS_CHECK(d->matches_hex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    NS_CHECK(!d->matches_hex("ba7816bf"));
    NS_CHECK(boson::crypto::parse_hash_algorithm("SHA-256") == HashAlgorithm::Sha256);
    NS_CHECK(!boson::crypto::parse_hash_algorithm("crc"));

    const fs::path file = g_root / "hash" / "big.bin";
    std::string big(8u << 20, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31u);
    write_all(file, big);
    const auto t0 = std::chrono::steady_clock::now();
    const auto fd = boson::crypto::hash_file(HashAlgorithm::Sha256, file);
    const double ms = ms_since(t0);
    NS_CHECK(fd && *fd == *boson::crypto::hash(HashAlgorithm::Sha256, big));
    std::cout << "  [perf] sha256_file 8 MiB: " << ms << " ms (" << (8.0 / (ms / 1000.0)) << " MiB/s)\n";
    std::stop_source stop;
    stop.request_stop();
    NS_CHECK(!boson::crypto::hash_file(HashAlgorithm::Sha256, file, stop.get_token()));
}

// ---- zip --------------------------------------------------------------------------------------------------

using boson::archive::ZipErrorKind;
using boson::archive::ZipReader;
using boson::archive::ZipWriter;

std::optional<ZipErrorKind> open_error(const std::vector<std::byte>& zip, boson::archive::ZipLimits limits = {}) {
    auto r = ZipReader::open_memory(zip, limits);
    if (r) return std::nullopt;
    return r.error().kind;
}

void test_zip() {
    step("zip: stored + deflate + zip64, extraction, hostile archives");
    for (const bool z64 : {false, true}) {
        ZipWriter w(ZipWriter::Options{.deflate = true, .level = 6, .force_zip64 = z64});
        w.add_directory("AddonA/");
        w.add("AddonA/AddonA.txt", std::string_view("## Title: Addon A\n## Version: 1.2\n"));
        w.add("AddonA/lib/big.lua", std::string(300'000, 'x'));
        w.add("AddonB/readme.md", std::string_view("b"));
        ZipWriter::Options stored{.deflate = false};
        (void)stored;
        const auto zip = w.finish();
        auto reader = ZipReader::open_memory(zip);
        NS_CHECK(reader.has_value());
        if (!reader) continue;
        NS_CHECK(reader->entries().size() == 4);
        NS_CHECK(reader->entries()[2].zip64 == z64);
        const auto* e = reader->find("AddonA/AddonA.txt");
        NS_CHECK(e != nullptr);
        if (e) {
            const auto data = reader->read_all(*e);
            NS_CHECK(data && std::string(reinterpret_cast<const char*>(data->data()), data->size()).starts_with("## Title"));
        }
        const fs::path dest = g_root / (z64 ? "zip64-out" : "zip-out");
        auto rep = reader->extract_to(dest);
        NS_CHECK(rep.has_value());
        if (rep) {
            NS_CHECK(rep->files == 3);
            NS_CHECK(rep->top_level.size() == 2);
        }
        NS_CHECK(read_all(dest / "AddonA" / "lib" / "big.lua").size() == 300'000);
        // Not empty any more: a second staging extraction is refused.
        NS_CHECK(!reader->extract_to(dest));
    }
    {
        ZipWriter w(ZipWriter::Options{.deflate = false});
        w.add("stored.txt", std::string_view("plain"));
        const auto zip = w.finish();
        auto r = ZipReader::open_memory(zip);
        NS_CHECK(r && r->entries()[0].method == 0);
    }

    const auto single = [](std::string_view name, std::uint32_t mode = 0) {
        ZipWriter w;
        w.add(name, std::string_view("payload"), mode);
        return w.finish();
    };
    for (const char* bad : {"../evil.txt", "a/../../evil.txt", "/abs.txt", "\\abs.txt", "C:/evil.txt", "C:evil.txt",
                            "a\\..\\..\\evil.txt", "dir/con.txt", "NUL", "com1.log", "file.txt:stream", "trailing./x",
                            "space /x", "bad\x01name", "q?.txt"}) {
        const auto err = open_error(single(bad));
        NS_CHECK(err == ZipErrorKind::UnsafePath);
        if (err != ZipErrorKind::UnsafePath) std::cerr << "  unsafe name accepted: " << bad << '\n';
    }
    NS_CHECK(open_error(single("link", 0120777)) == ZipErrorKind::Symlink);
    {
        ZipWriter w;
        w.add("Dir/File.txt", std::string_view("1"));
        w.add("dir/file.TXT", std::string_view("2"));
        NS_CHECK(open_error(w.finish()) == ZipErrorKind::Duplicate);
    }
    {
        // Declared size smaller than the real data: caught while inflating.
        ZipWriter w;
        const std::string data(100'000, 'z');
        w.add_with_declared_size("bomb.bin", bytes_of(data), 1000);
        const auto zip = w.finish();
        auto r = ZipReader::open_memory(zip);
        NS_CHECK(r.has_value());
        if (r) {
            auto out = r->read_all(r->entries()[0]);
            NS_CHECK(!out && out.error().kind == ZipErrorKind::LimitExceeded);
            const fs::path dest = g_root / "bomb-out";
            auto ex = r->extract_to(dest);
            NS_CHECK(!ex);
            NS_CHECK(!fs::exists(dest / "bomb.bin")); // partial file deleted
        }
    }
    {
        // Ratio bomb: 64 MiB of zeros compresses ~1000:1.
        ZipWriter w;
        const std::string zeros(64u << 20, '\0');
        w.add("zeros.bin", zeros);
        const auto zip = w.finish();
        NS_CHECK(open_error(zip) == ZipErrorKind::LimitExceeded);
        boson::archive::ZipLimits relaxed;
        relaxed.max_ratio = 100'000;
        NS_CHECK(!open_error(zip, relaxed));
        boson::archive::ZipLimits tight;
        tight.max_ratio = 100'000;
        tight.max_entry_size = 1u << 20;
        NS_CHECK(open_error(zip, tight) == ZipErrorKind::LimitExceeded);
    }
    {
        ZipWriter w;
        for (int i = 0; i < 20; ++i) w.add("f" + std::to_string(i), std::string_view("x"));
        boson::archive::ZipLimits lim;
        lim.max_entries = 10;
        NS_CHECK(open_error(w.finish(), lim) == ZipErrorKind::LimitExceeded);
    }
    {
        // Corrupt one data byte of a stored entry -> CRC mismatch.
        ZipWriter w(ZipWriter::Options{.deflate = false});
        w.add("a.txt", std::string_view("hello world"));
        auto zip = w.finish();
        zip[30 + 5 + 2] = std::byte{'X'};
        auto r = ZipReader::open_memory(zip);
        NS_CHECK(r.has_value());
        if (r) {
            auto out = r->read_all(r->entries()[0]);
            NS_CHECK(!out && out.error().kind == ZipErrorKind::CrcMismatch);
        }
    }
    {
        std::vector<std::byte> junk(100, std::byte{0x41});
        NS_CHECK(open_error(junk) == ZipErrorKind::NotZip);
        ZipWriter w;
        w.add("a.txt", std::string_view("hello"));
        auto zip = w.finish();
        zip.resize(zip.size() - 30); // truncated end record
        NS_CHECK(open_error(zip).has_value());
    }
    {
        // Timing: deflate archive of 2000 small files from disk.
        ZipWriter w;
        for (int i = 0; i < 2000; ++i) w.add("Addon/f" + std::to_string(i) + ".lua", std::string(2000, static_cast<char>('a' + i % 26)));
        const auto zip = w.finish();
        const fs::path zp = g_root / "many.zip";
        std::ofstream(zp, std::ios::binary).write(reinterpret_cast<const char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
        const auto t0 = std::chrono::steady_clock::now();
        auto r = ZipReader::open(zp);
        const double open_ms = ms_since(t0);
        NS_CHECK(r.has_value());
        const auto t1 = std::chrono::steady_clock::now();
        auto rep = r->extract_to(g_root / "many-out");
        const double ex_ms = ms_since(t1);
        NS_CHECK(rep && rep->files == 2000);
        std::cout << "  [perf] zip open 2000 entries: " << open_ms << " ms; extract 2000 files (4 MB): " << ex_ms << " ms\n";
    }
    NS_CHECK(boson::archive::sanitize_entry_name("a/./b//c.txt").value() == fs::path(L"a") / L"b" / L"c.txt");
    const auto gz = boson::archive::gzip_compress(bytes_of("hello hello hello"));
    NS_CHECK(gz.size() > 18 && gz[0] == std::byte{0x1f});
}

// ---- staged install ---------------------------------------------------------------------------------------

using boson::fs::InstallTransaction;

void stage_addon(InstallTransaction& tx, std::string_view name, std::string_view content) {
    write_all(tx.staging_dir() / name / "manifest.txt", content);
}

int run_install_crash_child(const fs::path& root) {
    auto tx = InstallTransaction::begin(root);
    if (!tx) return 2;
    stage_addon(*tx, "AddonA", "new");
    if (!tx->replace(L"AddonA")) return 3;
    tx->set_rename_hook_for_testing([](int done) {
        if (done == 1) {
            TerminateProcess(GetCurrentProcess(), 9); // power cut between "backup" and "install"
        }
        return true;
    });
    (void)tx->commit();
    return 4;
}

void test_install() {
    step("staged install: create, replace, rollback, locked target, crash recovery, temp cleanup");
    const fs::path root = g_root / "AddOns";
    {
        auto tx = InstallTransaction::begin(root);
        NS_CHECK(tx.has_value());
        stage_addon(*tx, "AddonA", "v1");
        stage_addon(*tx, "AddonB", "v1");
        NS_CHECK(tx->replace_all_staged() == 2u);
        auto rep = tx->commit();
        NS_CHECK(rep && rep->created == 2 && rep->replaced == 0);
        NS_CHECK(!fs::exists(tx->transaction_dir()));
    }
    NS_CHECK(read_all(root / "AddonA" / "manifest.txt") == "v1");
    {
        auto tx = InstallTransaction::begin(root);
        stage_addon(*tx, "AddonA", "v2");
        write_all(root / "AddonA" / "old-only.txt", "stale");
        NS_CHECK(tx->replace(L"AddonA").has_value());
        NS_CHECK(!tx->replace(L"..").has_value());
        NS_CHECK(!tx->replace(L"Missing").has_value());
        auto rep = tx->commit();
        NS_CHECK(rep && rep->replaced == 1);
    }
    NS_CHECK(read_all(root / "AddonA" / "manifest.txt") == "v2");
    NS_CHECK(!fs::exists(root / "AddonA" / "old-only.txt"));
    for (int fail_at = 0; fail_at < 4; ++fail_at) {
        auto tx = InstallTransaction::begin(root);
        stage_addon(*tx, "AddonA", "v3");
        stage_addon(*tx, "AddonB", "v3");
        stage_addon(*tx, "AddonC", "v3");
        (void)tx->replace_all_staged();
        tx->set_rename_hook_for_testing([fail_at](int done) { return done < fail_at; });
        auto rep = tx->commit();
        NS_CHECK(!rep);
        NS_CHECK(read_all(root / "AddonA" / "manifest.txt") == "v2");
        NS_CHECK(read_all(root / "AddonB" / "manifest.txt") == "v1");
        NS_CHECK(!fs::exists(root / "AddonC"));
        NS_CHECK(!fs::exists(tx->transaction_dir()));
    }
    {
        // A file held open without FILE_SHARE_DELETE inside the target blocks the directory rename.
        const fs::path locked = root / "AddonB" / "manifest.txt";
        HANDLE h = CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        auto tx = InstallTransaction::begin(root);
        stage_addon(*tx, "AddonA", "v4");
        stage_addon(*tx, "AddonB", "v4");
        (void)tx->replace_all_staged();
        const auto t0 = std::chrono::steady_clock::now();
        auto rep = tx->commit();
        NS_CHECK(!rep);
        std::cout << "  locked target -> " << (rep ? std::string("ok?") : rep.error().message) << " (" << ms_since(t0) << " ms)\n";
        CloseHandle(h);
        NS_CHECK(read_all(root / "AddonA" / "manifest.txt") == "v2");
        NS_CHECK(read_all(root / "AddonB" / "manifest.txt") == "v1");
    }
    {
        auto tx = InstallTransaction::begin(root);
        NS_CHECK(tx->remove(L"AddonB").has_value());
        auto rep = tx->commit();
        NS_CHECK(rep && rep->removed == 1);
        NS_CHECK(!fs::exists(root / "AddonB"));
    }
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto child = spawn_self(L"--install-crash-child=\"" + root.wstring() + L"\"");
        NS_CHECK(child != nullptr);
        if (child) {
            WaitForSingleObject(child->process, 30000);
            DWORD code = 0;
            GetExitCodeProcess(child->process, &code);
            NS_CHECK(code == 9);
        }
        NS_CHECK(!fs::exists(root / "AddonA")); // crashed after the backup rename
        const auto report = InstallTransaction::recover(root);
        NS_CHECK(report.rolled_back == 1);
        NS_CHECK(read_all(root / "AddonA" / "manifest.txt") == "v2");
        std::cout << "  crash child + recover: " << ms_since(t0) << " ms, found " << report.transactions_found << '\n';
        std::size_t leftovers = 0;
        for (const auto& e : fs::directory_iterator(root)) leftovers += e.path().filename().wstring().starts_with(L".boson-txn-") ? 1 : 0;
        NS_CHECK(leftovers == 0);
    }
    {
        auto tx = InstallTransaction::begin(root);
        stage_addon(*tx, "AddonZ", "z");
        const fs::path dir = tx->transaction_dir();
        NS_CHECK(fs::exists(dir));
        const auto live = InstallTransaction::recover(root);
        NS_CHECK(live.skipped_live == 1);
        tx->abort();
        NS_CHECK(!fs::exists(dir));
    }
    {
        const fs::path parent = g_root / "tmp-parent";
        fs::create_directories(parent / "boson-test-4000000000-dead");
        fs::create_directories(parent / "boson-test-4-system");
        auto mine = boson::fs::TempDir::create_in(parent, "boson-test");
        NS_CHECK(mine.has_value());
        const std::size_t removed = boson::fs::cleanup_stale_temp_dirs("boson-test", std::chrono::hours{24}, parent);
        NS_CHECK(removed == 1);
        NS_CHECK(!fs::exists(parent / "boson-test-4000000000-dead"));
        NS_CHECK(fs::exists(parent / "boson-test-4-system"));
        NS_CHECK(fs::exists(mine->path()));
        const fs::path p = mine->path();
        mine = boson::fs::TempDir::create_in(parent, "boson-test");
        NS_CHECK(!fs::exists(p));
    }
    {
        auto docs = boson::fs::known_folder(boson::fs::KnownFolder::Documents);
        NS_CHECK(docs.has_value());
        const auto candidates = boson::fs::document_folder_candidates();
        NS_CHECK(!candidates.empty());
        std::cout << "  Documents = " << docs->string() << " (OneDrive: " << boson::fs::is_under_onedrive(*docs) << ", "
                  << candidates.size() << " candidates, " << boson::fs::onedrive_roots().size() << " OneDrive roots)\n";
        NS_CHECK(boson::fs::known_folder(boson::fs::KnownFolder::LocalAppData).has_value());
    }
}

// ---- http -------------------------------------------------------------------------------------------------

using boson::net::HttpClient;
using boson::net::HttpClientOptions;
using boson::net::HttpErrorKind;
using boson::net::HttpMethod;
using boson::net::HttpRequest;

void test_proxy_parsing() {
    step("proxy: list parsing, scheme selection, bypass rules, system config read");
    const auto list = boson::net::parse_proxy_list("http=proxy.corp:8080;https=secure.corp:8443 ftp=ftp.corp:21");
    NS_CHECK(list.size() == 3);
    NS_CHECK(boson::net::proxy_for_scheme(list, "https")->host == "secure.corp");
    NS_CHECK(boson::net::proxy_for_scheme(list, "https")->port == 8443);
    const auto generic = boson::net::parse_proxy_list("Proxy.Example:3128");
    NS_CHECK(generic.size() == 1 && generic[0].host == "proxy.example" && generic[0].scheme.empty());
    NS_CHECK(boson::net::proxy_for_scheme(generic, "http")->port == 3128);
    const auto url_form = boson::net::parse_proxy_list("http://10.0.0.1:80/ socks=sock.host:1080 [::1]:8888 bad:port");
    NS_CHECK(url_form.size() == 3);
    NS_CHECK(url_form[0].proxy_scheme == "http" && url_form[0].host == "10.0.0.1");
    NS_CHECK(url_form[1].proxy_scheme == "socks");
    NS_CHECK(url_form[2].host == "[::1]" && url_form[2].port == 8888);
    NS_CHECK(boson::net::proxy_bypassed("intranet", "<local>"));
    NS_CHECK(!boson::net::proxy_bypassed("example.com", "<local>"));
    NS_CHECK(boson::net::proxy_bypassed("api.Corp.local", "*.corp.local;localhost"));
    NS_CHECK(boson::net::proxy_bypassed("10.1.2.3", "10.*"));
    NS_CHECK(boson::net::proxy_bypassed("host.example", "host.example:443"));
    NS_CHECK(!boson::net::proxy_bypassed("other.example", "*.corp.local; host.example"));
    const auto sys = boson::net::read_system_proxy_config();
    NS_CHECK(sys.has_value());
    if (sys) {
        std::cout << "  system proxy: auto_detect=" << sys->auto_detect << " pac='" << sys->auto_config_url << "' proxy='" << sys->proxy << "'\n";
    }
}

void test_http() {
    step("http: loopback server routes");
    HttpTestServer server;
    std::atomic<int> flaky{0};
    server.route("/hello", [](const TestRequest& r) {
        return TestResponse{.status = 200, .headers = {{"Content-Type", "text/plain"}, {"X-Echo", r.header("X-Test")}}, .body = "hello " + r.method};
    });
    server.route("/r1", [](const TestRequest&) { return TestResponse{.status = 302, .headers = {{"Location", "/r2"}}}; });
    server.route("/r2", [](const TestRequest&) { return TestResponse{.status = 301, .headers = {{"Location", "/hello"}}}; });
    server.route("/loop", [](const TestRequest&) { return TestResponse{.status = 302, .headers = {{"Location", "/loop"}}}; });
    server.route("/see-other", [](const TestRequest&) { return TestResponse{.status = 303, .headers = {{"Location", "/hello"}}}; });
    server.route("/gzip", [](const TestRequest& r) {
        const std::string text(200'000, 'g');
        if (r.header("Accept-Encoding").find("gzip") == std::string::npos) {
            return TestResponse{.status = 400, .body = "no gzip"};
        }
        const auto gz = boson::archive::gzip_compress(bytes_of(text));
        return TestResponse{.status = 200, .headers = {{"Content-Encoding", "gzip"}}, .body = std::string(reinterpret_cast<const char*>(gz.data()), gz.size())};
    });
    server.route("/chunked", [](const TestRequest&) {
        std::string body;
        for (int i = 0; i < 5000; ++i) body += "line " + std::to_string(i) + "\n";
        return TestResponse{.status = 200, .body = body, .chunked = true, .chunk_size = 1000};
    });
    server.route("/slow-head", [](const TestRequest&) { return TestResponse{.status = 200, .body = "late", .delay_before = 3000ms}; });
    server.route("/slow-body", [](const TestRequest&) {
        return TestResponse{.status = 200, .body = std::string(1u << 20, 's'), .chunk_size = 4096, .delay_between_chunks = 50ms};
    });
    server.route("/flaky", [&flaky](const TestRequest&) {
        const int n = ++flaky;
        if (n < 3) return TestResponse{.status = 503, .headers = {{"Retry-After", "0"}}, .body = "busy"};
        return TestResponse{.status = 200, .body = "recovered"};
    });
    server.route("/always-500", [](const TestRequest&) { return TestResponse{.status = 500, .body = "boom"}; });
    server.route("/huge-claim", [](const TestRequest&) {
        return TestResponse{.status = 200, .body = "", .content_length_override = 10ull << 30};
    });
    server.route("/big-chunked", [](const TestRequest&) { return TestResponse{.status = 200, .body = std::string(3u << 20, 'b'), .chunked = true}; });
    server.route("/cached", [](const TestRequest&) { return TestResponse{.status = 200, .headers = {{"Cache-Control", "max-age=60"}}, .body = "cache me"}; });
    server.route("/no-store", [](const TestRequest&) { return TestResponse{.status = 200, .headers = {{"Cache-Control", "no-store"}}, .body = "fresh"}; });
    server.route("/echo", [](const TestRequest& r) { return TestResponse{.status = 200, .body = r.method + ":" + r.body}; });
    server.route("/nope", [](const TestRequest&) { return TestResponse{.status = 404, .body = "missing"}; });
    server.route("/reset", [](const TestRequest&) { return TestResponse{.status = 200, .body = std::string(100'000, 'r'), .abort_after = 1000}; });
    const std::string payload = [] {
        std::string s(5u << 20, '\0');
        for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>((i * 7919u) >> 3);
        return s;
    }();
    server.route("/file.zip", [&payload](const TestRequest&) { return TestResponse{.status = 200, .body = payload}; });
    server.fallback([](const TestRequest& r) { return TestResponse{.status = 200, .body = "proxied " + r.target}; });

    HttpClientOptions opts;
    opts.retry.initial_backoff = 10ms;
    opts.cache = boson::net::HttpCacheOptions{};
    HttpClient client(opts);
    NS_CHECK(!client.session_open()); // lazy: nothing opened before the first request

    step("http: GET, headers, keep-alive reuse");
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = client.send(HttpRequest{.url = server.url("/hello"), .headers = {{"X-Test", "42"}}});
        const double first_ms = ms_since(t0);
        NS_CHECK(r && r->status == 200 && r->body == "hello GET");
        NS_CHECK(r && r->header("x-echo") == "42");
        NS_CHECK(r && r->protocol == "HTTP/1.1");
        NS_CHECK(client.session_open());
        const int accepts_before = server.accepts();
        const auto t1 = std::chrono::steady_clock::now();
        constexpr int kN = 200;
        for (int i = 0; i < kN; ++i) {
            auto again = client.send(HttpRequest{.url = server.url("/hello"), .use_cache = false});
            NS_CHECK(again && again->ok());
        }
        const double per = ms_since(t1) / kN;
        NS_CHECK(server.accepts() - accepts_before <= 2);
        std::cout << "  [perf] first GET " << first_ms << " ms; keep-alive GET " << per * 1000.0 << " us/request; new connections: "
                  << server.accepts() - accepts_before << " for " << kN << " requests\n";
        auto h = client.head(server.url("/hello"));
        NS_CHECK(h && h->status == 200 && h->body.empty());
        auto p = client.post(server.url("/echo"), "{\"a\":1}");
        NS_CHECK(p && p->body == "POST:{\"a\":1}");
        auto put = client.send(HttpRequest{.method = HttpMethod::Put, .url = server.url("/echo"), .body = "xyz"});
        NS_CHECK(put && put->body == "PUT:xyz");
    }

    step("http: redirects (count, loop, 303 POST -> GET, filter on every hop)");
    {
        auto r = client.get(server.url("/r1"));
        NS_CHECK(r && r->status == 200 && r->redirects == 2 && r->final_url.ends_with("/hello"));
        if (!r) std::cerr << "  redirect error: " << r.error().message << '\n';
        else if (r->status != 200 || r->redirects != 2) std::cerr << "  redirect: status " << r->status << " redirects " << r->redirects << " url " << r->final_url << '\n';
        auto loop = client.get(server.url("/loop"));
        NS_CHECK(!loop && loop.error().kind == HttpErrorKind::TooManyRedirects);
        auto so = client.post(server.url("/see-other"), "data");
        NS_CHECK(so && so->body == "hello GET");
        HttpClientOptions filtered;
        filtered.url_filter = [](std::string_view url) { return url.find("/r2") == std::string_view::npos; };
        HttpClient fclient(filtered);
        auto refused = fclient.get(server.url("/r1"));
        NS_CHECK(!refused && refused.error().kind == HttpErrorKind::Refused);
        HttpClientOptions strict;
        strict.require_https = true;
        strict.allow_insecure_loopback = false;
        HttpClient sclient(strict);
        auto plain = sclient.get(server.url("/hello"));
        NS_CHECK(!plain && plain.error().kind == HttpErrorKind::Refused);
        auto bad = client.get("ftp://example.com/x");
        NS_CHECK(!bad && bad.error().kind == HttpErrorKind::InvalidUrl);
    }

    step("http: gzip decoding, chunked bodies");
    {
        auto gz = client.get(server.url("/gzip"));
        NS_CHECK(gz && gz->status == 200 && gz->body == std::string(200'000, 'g'));
        HttpClientOptions raw_opts;
        raw_opts.decompress = false;
        HttpClient raw(raw_opts);
        auto no = raw.get(server.url("/gzip"));
        NS_CHECK(no && no->status == 400);
        auto ch = client.get(server.url("/chunked"));
        NS_CHECK(ch && ch->body.starts_with("line 0\n") && ch->body.ends_with("line 4999\n") && !ch->content_length);
    }

    step("http: timeouts (deadline + per-phase), retries, cancel, size caps");
    {
        auto t0 = std::chrono::steady_clock::now();
        auto r = client.send(HttpRequest{.url = server.url("/slow-head"), .timeout = 300ms, .retry = boson::net::RetryPolicy::none()});
        double elapsed = ms_since(t0);
        NS_CHECK(!r && r.error().kind == HttpErrorKind::Timeout);
        NS_CHECK(elapsed < 1500);
        std::cout << "  deadline 300 ms -> " << elapsed << " ms\n";
        HttpClientOptions phase;
        phase.receive_timeout = 400ms;
        phase.retry = boson::net::RetryPolicy::none();
        HttpClient pclient(phase);
        t0 = std::chrono::steady_clock::now();
        auto r2 = pclient.get(server.url("/slow-head"));
        elapsed = ms_since(t0);
        NS_CHECK(!r2 && r2.error().kind == HttpErrorKind::Timeout);
        std::cout << "  receive_timeout 400 ms -> " << elapsed << " ms (" << (r2 ? std::string("ok") : r2.error().message) << ")\n";
        NS_CHECK(elapsed < 2500);

        flaky = 0;
        auto f = client.get(server.url("/flaky"));
        NS_CHECK(f && f->status == 200 && f->body == "recovered" && f->attempts == 3);
        auto five = client.get(server.url("/always-500"));
        NS_CHECK(five && five->status == 500 && five->attempts == 3);
        flaky = 0;
        auto post = client.post(server.url("/flaky"), "x"); // not idempotent: no retry
        NS_CHECK(post && post->status == 503 && post->attempts == 1);
        auto reset = client.get(server.url("/reset"));
        NS_CHECK(!reset && (reset.error().kind == HttpErrorKind::Transport || reset.error().kind == HttpErrorKind::Connect) && reset.error().attempts == 3);

        std::stop_source stop;
        std::jthread canceller([&stop]() {
            std::this_thread::sleep_for(200ms);
            stop.request_stop();
        });
        t0 = std::chrono::steady_clock::now();
        std::uint64_t got = 0;
        auto s = client.stream(HttpRequest{.url = server.url("/slow-body"), .stop = stop.get_token()},
                               [&got](std::span<const std::byte> part) { got += part.size(); return true; });
        elapsed = ms_since(t0);
        NS_CHECK(!s && s.error().kind == HttpErrorKind::Cancelled);
        NS_CHECK(elapsed < 1500);
        std::cout << "  cancel after 200 ms -> returned at " << elapsed << " ms with " << got << " bytes\n";

        auto huge = client.get(server.url("/huge-claim"));
        NS_CHECK(!huge && huge.error().kind == HttpErrorKind::TooLarge);
        auto capped = client.send(HttpRequest{.url = server.url("/big-chunked"), .max_body_bytes = 1u << 20});
        NS_CHECK(!capped && capped.error().kind == HttpErrorKind::TooLarge);
        auto fine = client.send(HttpRequest{.url = server.url("/big-chunked"), .max_body_bytes = 4u << 20});
        NS_CHECK(fine && fine->body.size() == (3u << 20));
    }

    step("http: response cache (TTL, no-store, LRU)");
    {
        const int before = server.requests();
        auto a = client.get(server.url("/cached"));
        auto b = client.get(server.url("/cached"));
        NS_CHECK(a && !a->from_cache && b && b->from_cache && b->body == "cache me");
        NS_CHECK(server.requests() - before == 1);
        auto n1 = client.get(server.url("/no-store"));
        auto n2 = client.get(server.url("/no-store"));
        NS_CHECK(n1 && n2 && !n2->from_cache);
        auto skip = client.send(HttpRequest{.url = server.url("/cached"), .use_cache = false});
        NS_CHECK(skip && !skip->from_cache);
        boson::net::HttpCache lru(boson::net::HttpCacheOptions{.max_entries = 2});
        for (int i = 0; i < 3; ++i) {
            auto resp = std::make_shared<boson::net::HttpResponse>();
            resp->status = 200;
            resp->body = "x";
            lru.store("k" + std::to_string(i), resp);
        }
        NS_CHECK(!lru.lookup("k0") && lru.lookup("k1") && lru.lookup("k2"));
        NS_CHECK(lru.stats().evictions == 1);
        auto resp = std::make_shared<boson::net::HttpResponse>();
        resp->status = 200;
        lru.store("ttl", resp, boson::net::HttpCache::Clock::now() - std::chrono::hours{1});
        NS_CHECK(!lru.lookup("ttl"));
    }

    step("http: download with progress, hash verification, mismatch, explicit proxy");
    {
        const fs::path dest = g_root / "dl" / "file.zip";
        int calls = 0;
        boson::net::TransferProgress lastp;
        const std::string expected = boson::crypto::hash(boson::crypto::HashAlgorithm::Sha256, payload)->hex();
        const auto t0 = std::chrono::steady_clock::now();
        auto d = client.download(HttpRequest{.url = server.url("/file.zip")}, dest,
                                 boson::net::DownloadOptions{.on_progress = [&](const boson::net::TransferProgress& p) { ++calls; lastp = p; },
                                                             .progress_interval = 0ms,
                                                             .expected_hash = expected});
        const double ms = ms_since(t0);
        NS_CHECK(d && d->bytes == payload.size() && d->digest && d->digest->hex() == expected);
        NS_CHECK(calls > 1 && lastp.received == payload.size() && lastp.total == payload.size());
        NS_CHECK(read_all(dest) == payload);
        NS_CHECK(!fs::exists(dest.wstring() + L".partial"));
        std::cout << "  [perf] download 5 MiB + sha256 over loopback: " << ms << " ms (" << (5.0 / (ms / 1000.0)) << " MiB/s), progress calls "
                  << calls << '\n';
        const fs::path bad = g_root / "dl" / "bad.zip";
        auto mm = client.download(HttpRequest{.url = server.url("/file.zip")}, bad, boson::net::DownloadOptions{.expected_hash = std::string(64, '0')});
        NS_CHECK(!mm && mm.error().kind == HttpErrorKind::HashMismatch);
        NS_CHECK(!fs::exists(bad) && !fs::exists(bad.wstring() + L".partial"));
        auto nf = client.download(HttpRequest{.url = server.url("/nope")}, g_root / "dl" / "nope.bin");
        NS_CHECK(!nf && nf.error().kind == HttpErrorKind::Status && nf.error().status == 404);
        if (!nf) std::cerr << "  404 download: " << boson::net::to_string(nf.error().kind) << " " << nf.error().message << '\n';

        HttpClientOptions px;
        px.proxy = boson::net::ProxySettings{.mode = boson::net::ProxyMode::Explicit, .server = "127.0.0.1:" + std::to_string(server.port()), .bypass = ""};
        HttpClient proxied(px);
        auto pr = proxied.get("http://boson-proxy-test.invalid/some/path?q=1");
        NS_CHECK(pr && pr->status == 200);
        NS_CHECK(pr && pr->body == "proxied http://boson-proxy-test.invalid/some/path?q=1");
    }

    step("http: senders (sync_wait, stop from when_all)");
    {
        exec::static_thread_pool pool(2);
        auto sched = pool.get_scheduler();
        auto result = stdexec::sync_wait(client.send_on(sched, HttpRequest{.url = server.url("/hello")}));
        NS_CHECK(result.has_value());
        if (result) {
            auto& [resp] = *result;
            NS_CHECK(resp && resp->body == "hello GET");
        }
        const auto t0 = std::chrono::steady_clock::now();
        bool threw = false;
        try {
            auto failing = stdexec::schedule(sched) | stdexec::then([]() {
                std::this_thread::sleep_for(200ms);
                throw std::runtime_error("stop the others");
            });
            (void)stdexec::sync_wait(stdexec::when_all(client.send_on(sched, HttpRequest{.url = server.url("/slow-body")}), std::move(failing)));
        } catch (const std::exception&) {
            threw = true;
        }
        const double elapsed = ms_since(t0);
        NS_CHECK(threw);
        NS_CHECK(elapsed < 2000);
        std::cout << "  when_all stop -> " << elapsed << " ms\n";
        auto dl = stdexec::sync_wait(client.download_on(sched, HttpRequest{.url = server.url("/file.zip")}, g_root / "dl" / "sender.zip"));
        NS_CHECK(dl && std::get<0>(*dl) && std::get<0>(*dl)->bytes == payload.size());
    }
    const auto stats = client.stats();
    NS_CHECK(stats.sessions_opened >= 1 && stats.retries >= 2 && stats.redirects >= 2 && stats.cache_hits >= 1);
}

// ---- db -----------------------------------------------------------------------------------------------------

} // namespace

namespace native_test_records {
struct AddonRecord {
    std::string uid;
    std::string name;
    std::int64_t version = 0;
    double rating = 0.0;
    bool enabled = false;
    std::optional<std::string> note;
    std::vector<std::string> tags;
};

struct Counter {
    std::int64_t id = 0;
    std::int64_t value = 0;
};

struct Prefs {
    int volume = 0;
    std::string lang;
};
} // namespace native_test_records

using native_test_records::AddonRecord;
using native_test_records::Counter;
using native_test_records::Prefs;

namespace {

boson::db::DatabaseOptions db_options(boson::db::Backend backend, const fs::path& base) {
    boson::db::DatabaseOptions o;
    o.backend = backend;
    o.path = backend == boson::db::Backend::Sqlite ? base / "app.sqlite" : base / "zephyr";
    return o;
}

int run_db_crash_child(const fs::path& base, boson::db::Backend backend) {
    auto db = boson::db::Database::open(db_options(backend, base));
    if (!db) {
        std::cerr << db.error().message << '\n';
        return 2;
    }
    boson::db::Table<Counter> table(*db, "counters");
    if (!table.create()) return 3;
    auto tx = db->transaction([&](boson::db::Connection& c) -> boson::db::DbResult<void> {
        for (std::int64_t i = 0; i < 100; ++i) {
            if (auto r = table.upsert(c, Counter{.id = i, .value = i * 2}); !r) return r;
        }
        return {};
    });
    if (!tx) {
        std::cerr << tx.error().message << '\n';
        return 4;
    }
    write_all(base / "committed.marker", "1");
    for (std::int64_t i = 100;; ++i) {
        (void)table.upsert(Counter{.id = i, .value = i});
        if (i == 150) write_all(base / "async.marker", "1");
    }
}

void test_db(boson::db::Backend backend) {
    const std::string bname(boson::db::to_string(backend));
    step("db[" + bname + "]: open, Table<T> CRUD, JSON / optional columns");
    const fs::path base = g_root / ("db-" + bname);
    fs::create_directories(base);
    {
        auto db = boson::db::Database::open(db_options(backend, base));
        NS_CHECK(db.has_value());
        if (!db) {
            std::cerr << "  open failed: " << db.error().message << '\n';
            return;
        }
        NS_CHECK(db->backend() == backend);
        boson::db::Table<AddonRecord> addons(*db, "addons");
        auto created = addons.create();
        NS_CHECK(created.has_value());
        if (!created) std::cerr << "  create: " << created.error().message << '\n';
        NS_CHECK(addons.create().has_value()); // idempotent
        AddonRecord a{.uid = "uid-1", .name = "LibAddonMenu", .version = 34, .rating = 4.5, .enabled = true, .note = std::nullopt, .tags = {"lib", "menu"}};
        auto up = addons.upsert(a);
        NS_CHECK(up.has_value());
        if (!up) std::cerr << "  upsert: " << up.error().message << '\n';
        auto got = addons.get(std::string("uid-1"));
        NS_CHECK(got && got->has_value());
        if (got && *got) {
            NS_CHECK((*got)->name == "LibAddonMenu" && (*got)->version == 34 && (*got)->rating == 4.5 && (*got)->enabled);
            NS_CHECK(!(*got)->note && (*got)->tags.size() == 2 && (*got)->tags[1] == "menu");
        } else if (!got) {
            std::cerr << "  get: " << got.error().message << '\n';
        }
        a.version = 35;
        a.note = "updated";
        NS_CHECK(addons.upsert(a).has_value());
        got = addons.get(std::string("uid-1"));
        NS_CHECK(got && *got && (*got)->version == 35 && (*got)->note == "updated");
        NS_CHECK(addons.count() == 1);
        std::vector<AddonRecord> many;
        for (int i = 0; i < 50; ++i) {
            many.push_back(AddonRecord{.uid = "bulk-" + std::to_string(100 + i), .name = "Addon " + std::to_string(i), .version = i, .rating = i / 10.0,
                                       .enabled = i % 2 == 0});
        }
        auto all_up = addons.upsert_all(many);
        NS_CHECK(all_up.has_value());
        if (!all_up) std::cerr << "  upsert_all: " << all_up.error().message << '\n';
        NS_CHECK(addons.count() == 51);
        auto top = addons.where("version >= $1 ORDER BY version DESC LIMIT 5", std::int64_t{40});
        NS_CHECK(top && top->size() == 5 && top->front().version == 49);
        if (!top) std::cerr << "  where: " << top.error().message << '\n';
        auto enabled = addons.where("enabled = $1", true);
        NS_CHECK(enabled && enabled->size() == 26);
        if (!enabled) std::cerr << "  where bool: " << enabled.error().message << '\n';
        NS_CHECK(addons.remove(std::string("bulk-100")) == true);
        NS_CHECK(addons.remove(std::string("bulk-100")) == false);
        NS_CHECK(addons.count() == 50);
        NS_CHECK(addons.create_index("version").has_value());
        auto raw = db->query_value<std::int64_t>("SELECT version FROM addons WHERE uid = $1", std::string("uid-1"));
        NS_CHECK(raw && *raw == 35);
        auto bad = db->execute("SELEKT nonsense");
        NS_CHECK(!bad.has_value());
    }

    step("db[" + bname + "]: transactions (commit, rollback on error, exception)");
    {
        auto db = boson::db::Database::open(db_options(backend, base));
        NS_CHECK(db.has_value());
        if (!db) return;
        boson::db::Table<AddonRecord> addons(*db, "addons");
        NS_CHECK(addons.count() == 50); // reopened: data persisted
        auto failed = db->transaction([&](boson::db::Connection& c) -> boson::db::DbResult<void> {
            if (auto r = addons.upsert(c, AddonRecord{.uid = "tx-1", .name = "rolled back"}); !r) return r;
            return std::unexpected(boson::db::DbError{.code = 0, .extended_code = 0, .message = "abort"});
        });
        NS_CHECK(!failed);
        NS_CHECK(addons.get(std::string("tx-1")) && !addons.get(std::string("tx-1"))->has_value());
        bool caught = false;
        try {
            (void)db->transaction([&](boson::db::Connection& c) {
                (void)addons.upsert(c, AddonRecord{.uid = "tx-2", .name = "thrown"});
                throw std::runtime_error("handler failed");
            });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        NS_CHECK(caught);
        NS_CHECK(addons.get(std::string("tx-2")) && !addons.get(std::string("tx-2"))->has_value());
        auto ok = db->transaction([&](boson::db::Connection& c) -> boson::db::DbResult<int> {
            if (auto r = addons.upsert(c, AddonRecord{.uid = "tx-3", .name = "kept"}); !r) return std::unexpected(r.error());
            return 7;
        });
        NS_CHECK(ok && *ok == 7);
        NS_CHECK(addons.get(std::string("tx-3")) && addons.get(std::string("tx-3"))->has_value());
        auto nested = db->transaction([&](boson::db::Connection& c) -> boson::db::DbResult<void> {
            (void)addons.upsert(c, AddonRecord{.uid = "tx-4", .name = "outer"});
            (void)c.transaction([&](boson::db::Connection& inner) -> boson::db::DbResult<void> {
                (void)addons.upsert(inner, AddonRecord{.uid = "tx-5", .name = "inner"});
                return std::unexpected(boson::db::DbError{.code = 0, .extended_code = 0, .message = "inner fails"});
            });
            return {};
        });
        if (backend == boson::db::Backend::Sqlite) {
            NS_CHECK(nested.has_value()); // savepoint: only the inner part is undone
            NS_CHECK(addons.get(std::string("tx-4"))->has_value() && !addons.get(std::string("tx-5"))->has_value());
        } else {
            NS_CHECK(!nested.has_value()); // no savepoints: the whole transaction rolls back
            NS_CHECK(!addons.get(std::string("tx-4"))->has_value() && !addons.get(std::string("tx-5"))->has_value());
        }
    }

    step("db[" + bname + "]: KvStore, concurrency, sender, statement cache");
    {
        auto db = boson::db::Database::open(db_options(backend, base));
        if (!db) return;
        boson::db::KvStore kv(*db);
        auto kc = kv.create();
        NS_CHECK(kc.has_value());
        if (!kc) std::cerr << "  kv create: " << kc.error().message << '\n';
        NS_CHECK(kv.set("settings", "theme", "\"dark\"").has_value());
        NS_CHECK(kv.set("settings", "theme", "\"light\"").has_value());
        NS_CHECK(kv.get("settings", "theme") && *kv.get("settings", "theme") == "\"light\"");
        NS_CHECK(kv.get("settings", "missing") && !kv.get("settings", "missing")->has_value());
        for (int i = 0; i < 20; ++i) (void)kv.set("cache", "page/" + std::to_string(i), std::to_string(i));
        (void)kv.set("cache", "pagf", "x");
        (void)kv.set("other", "page/1", "y");
        auto keys = kv.keys("cache", "page/");
        NS_CHECK(keys && keys->size() == 20);
        if (!keys) std::cerr << "  kv keys: " << keys.error().message << '\n';
        auto limited = kv.entries("cache", "page/", 5);
        NS_CHECK(limited && limited->size() == 5 && limited->front().key == "page/0");
        NS_CHECK(kv.count("cache") == 21);
        NS_CHECK(kv.remove("cache", "pagf") == true);
        NS_CHECK(kv.clear("cache") == 20);
        NS_CHECK(kv.count("other") == 1);
        NS_CHECK(kv.set_as("prefs", "main", Prefs{.volume = 7, .lang = "pt-BR"}).has_value());
        auto prefs = kv.get_as<Prefs>("prefs", "main");
        NS_CHECK(prefs && *prefs && (*prefs)->volume == 7 && (*prefs)->lang == "pt-BR");
        NS_CHECK(!kv.set("bad\x1fns", "k", "v").has_value());

        boson::db::Table<Counter> counters(*db, "counters_mt");
        NS_CHECK(counters.create().has_value());
        constexpr int kThreads = 4;
        constexpr int kPer = 250;
        std::atomic<int> errors{0};
        {
            std::vector<std::jthread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t]() {
                    for (int i = 0; i < kPer; ++i) {
                        if (!counters.upsert(Counter{.id = t * 10'000 + i, .value = i})) ++errors;
                        if (!counters.get(std::int64_t{t * 10'000})) ++errors;
                    }
                });
            }
        }
        NS_CHECK(errors == 0);
        NS_CHECK(counters.count() == kThreads * kPer);
        exec::static_thread_pool pool(2);
        auto r = stdexec::sync_wait(db->run_on(pool.get_scheduler(), [](boson::db::Connection& c) { return c.query_value<std::int64_t>("SELECT COUNT(*) FROM counters_mt"); }));
        NS_CHECK(r && std::get<0>(*r) && *std::get<0>(*r) == kThreads * kPer);
        NS_CHECK(db->with([](boson::db::Connection& c) { return c.cached_statements(); }) > 0);
        NS_CHECK(db->checkpoint().has_value());
        NS_CHECK(db->integrity_check() == true);
    }
    if (backend == boson::db::Backend::Zephyr) {
        auto again = boson::db::Database::open(db_options(backend, base));
        NS_CHECK(again.has_value());
        if (again) {
            auto twice = boson::db::Database::open(db_options(backend, base));
            NS_CHECK(!twice); // one engine per directory
        }
        boson::db::DatabaseOptions enc;
        enc.backend = backend;
        enc.path = base / "zephyr-enc";
        const std::string secret = "correct horse battery staple";
        enc.encryption_key.assign(reinterpret_cast<const std::byte*>(secret.data()), reinterpret_cast<const std::byte*>(secret.data()) + secret.size());
        {
            auto e = boson::db::Database::open(enc);
            NS_CHECK(e.has_value());
            if (e) {
                boson::db::KvStore ekv(*e);
                NS_CHECK(ekv.create().has_value());
                NS_CHECK(ekv.set("s", "token", "secret-value").has_value());
            }
        }
        const std::string snap = read_all(enc.path / "zephyr.snap") + read_all(enc.path / "zephyr.wal");
        NS_CHECK(snap.find("secret-value") == std::string::npos);
        auto wrong = enc;
        wrong.encryption_key.assign(4, std::byte{1});
        NS_CHECK(!boson::db::Database::open(wrong));
        auto right = boson::db::Database::open(enc);
        NS_CHECK(right.has_value());
        if (right) {
            boson::db::KvStore ekv(*right);
            NS_CHECK(ekv.get("s", "token") && *ekv.get("s", "token") == "secret-value");
        }
        boson::db::DatabaseOptions full = db_options(backend, base / "full");
        full.synchronous = boson::db::Synchronous::Full;
        NS_CHECK(!boson::db::Database::open(full));
    } else {
        boson::db::DatabaseOptions enc = db_options(backend, base / "enc");
        enc.encryption_key.assign(4, std::byte{1});
        NS_CHECK(!boson::db::Database::open(enc));
    }

    step("db[" + bname + "]: crash recovery (child process killed during writes)");
    {
        const fs::path cbase = base / "crash";
        (void)boson::fs::remove_tree(cbase);
        fs::create_directories(cbase);
        auto child = spawn_self(L"--db-crash-child=\"" + cbase.wstring() + L"\" --backend=" + std::wstring(bname.begin(), bname.end()));
        NS_CHECK(child != nullptr);
        const bool committed = wait_for_file(cbase / "async.marker", 30s);
        NS_CHECK(committed);
        // ZephyrDB acknowledges autocommit writes before its WAL guardian writes them (group commit every
        // zephyr_wal_idle_flush = 1 s), so a kill loses the last window; wait past it before killing.
        std::this_thread::sleep_for(backend == boson::db::Backend::Zephyr ? 1500ms : 50ms);
        TerminateProcess(child->process, 9);
        WaitForSingleObject(child->process, 10000);
        auto db = boson::db::Database::open(db_options(backend, cbase));
        NS_CHECK(db.has_value());
        if (db) {
            boson::db::Table<Counter> table(*db, "counters");
            std::int64_t committed_rows = 0;
            for (std::int64_t i = 0; i < 100; ++i) {
                auto g = table.get(i);
                committed_rows += (g && *g && (*g)->value == i * 2) ? 1 : 0;
            }
            NS_CHECK(committed_rows == 100);
            const auto total = table.count();
            NS_CHECK(total.has_value() && *total >= 150);
            if (!total) std::cerr << "  count after crash: " << total.error().message << '\n';
            std::cout << "  committed transaction rows after kill: " << committed_rows << "/100; autocommit rows: "
                      << (total ? *total - 100 : -1) << " (child had written >= 51)\n";
            // Writable after recovery.
            NS_CHECK(table.upsert(Counter{.id = 999'999, .value = 1}).has_value());
        }
    }
}

void test_controllers() {
    step("controllers: Http (allow-list, redirect hop, download to ScopedFs, cancel), Db (namespaces, JSON, read-only)");
    HttpTestServer server;
    server.route("/feed.json", [](const TestRequest&) { return TestResponse{.status = 200, .headers = {{"Content-Type", "application/json"}}, .body = R"({"addons":[1,2]})"}; });
    server.route("/escape", [](const TestRequest&) { return TestResponse{.status = 302, .headers = {{"Location", "http://localhost:1/blocked"}}}; });
    server.route("/bin", [](const TestRequest&) { return TestResponse{.status = 200, .body = std::string("\x00\x01\x02", 3)}; });
    server.route("/slow", [](const TestRequest&) { return TestResponse{.status = 200, .body = std::string(1u << 20, 's'), .chunk_size = 4096, .delay_between_chunks = 50ms}; });
    const std::string payload(300'000, 'p');
    server.route("/pkg.zip", [&payload](const TestRequest&) { return TestResponse{.status = 200, .body = payload}; });

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([]() {});
    boson::exec::TaskRunner runner(scheduler, marshal);
    boson::ipc::Bridge bridge(runner, marshal, [](std::string_view, boson::ipc::Bridge::ReplyFn) {});
    auto scope = std::make_shared<boson::security::FsScope>();
    const fs::path downloads = g_root / "ctl-downloads";
    fs::create_directories(downloads);
    NS_CHECK(scope->allow(downloads.wstring(), false, true, "test").has_value());
    auto scoped = std::make_shared<boson::security::ScopedFs>(scope);
    boson::ipc::HttpControllerOptions hopts;
    hopts.allowed_hosts = {"127.0.0.1"};
    hopts.client.retry = boson::net::RetryPolicy::none();
    boson::ipc::HttpController http(bridge, runner, scoped, hopts);
    auto database = boson::db::Database::open(boson::db::DatabaseOptions{});
    NS_CHECK(database.has_value());
    auto shared_db = std::make_shared<boson::db::Database>(std::move(*database));
    boson::ipc::DbController dbc(shared_db, boson::ipc::DbControllerOptions{.namespaces = {"settings"}, .read_only_namespaces = {"catalog"}});
    boson::ipc::Router<boson::ipc::HttpController, boson::ipc::DbController> router(http, dbc);
    const auto call = [&](std::string_view method, std::string_view params) {
        return router.dispatch_sync(std::string(R"({"id":1,"method":")") + std::string(method) + R"(","params":)" + std::string(params) + "}");
    };
    const auto ok = [](const std::string& reply) { return reply.find("\"error\"") == std::string::npos; };

    std::string r = call("Http.fetch", R"({"url":")" + server.url("/feed.json") + R"("})");
    NS_CHECK(ok(r) && r.find(R"(\"addons\")") != std::string::npos);
    r = call("Http.fetch", R"({"url":"http://localhost:)" + std::to_string(server.port()) + R"(/feed.json"})");
    NS_CHECK(!ok(r)); // localhost is not on the list
    r = call("Http.fetch", R"({"url":")" + server.url("/escape") + R"("})");
    NS_CHECK(!ok(r) && r.find("refused") != std::string::npos);
    r = call("Http.fetch", R"({"url":")" + server.url("/bin") + R"(","base64":true})");
    NS_CHECK(ok(r) && r.find(R"("body":"AAEC")") != std::string::npos);
    r = call("Http.fetch", R"({"method":"BREW","url":")" + server.url("/feed.json") + R"("})");
    NS_CHECK(!ok(r));

    const std::string target = (downloads / "pkg.zip").string();
    const std::string sha = boson::crypto::hash(boson::crypto::HashAlgorithm::Sha256, payload)->hex();
    std::string escaped;
    for (const char c : target) {
        if (c == '\\') escaped += '\\';
        escaped += c;
    }
    r = call("Http.download", R"({"url":")" + server.url("/pkg.zip") + R"(","path":")" + escaped + R"(","expected_hash":")" + sha + R"("})");
    NS_CHECK(ok(r) && r.find(R"("id":)") != std::string::npos);
    NS_CHECK(wait_for_file(downloads / "pkg.zip", 10s));
    r = call("Http.download", R"({"url":")" + server.url("/pkg.zip") + R"(","path":"C:\\Windows\\evil.zip"})");
    NS_CHECK(!ok(r)); // outside the scope
    const std::string slow_target = escaped.substr(0, escaped.size() - 7) + "slow.bin";
    r = call("Http.download", R"({"url":")" + server.url("/slow") + R"(","path":")" + slow_target + R"("})");
    NS_CHECK(ok(r));
    const auto id_pos = r.find(R"("id":)", r.find(R"("data")"));
    const std::string id = id_pos == std::string::npos ? "0" : r.substr(id_pos + 5, r.find_first_of(",}", id_pos + 5) - id_pos - 5);
    std::this_thread::sleep_for(100ms);
    r = call("Http.cancel", R"({"id":)" + id + "}");
    NS_CHECK(ok(r) && r.find(R"("value":true)") != std::string::npos);
    for (int i = 0; i < 100 && call("Http.cancel", R"({"id":)" + id + "}").find(R"("value":true)") != std::string::npos; ++i) {
        std::this_thread::sleep_for(20ms); // the task is still unwinding
    }
    NS_CHECK(!fs::exists(downloads / "slow.bin"));
    NS_CHECK(read_all(downloads / "pkg.zip") == payload);

    NS_CHECK(ok(call("Db.set", R"({"ns":"settings","key":"theme","value":"{\"dark\":true}"})")));
    r = call("Db.get", R"({"ns":"settings","key":"theme"})");
    NS_CHECK(ok(r) && r.find(R"("found":true)") != std::string::npos);
    NS_CHECK(!ok(call("Db.set", R"({"ns":"settings","key":"bad","value":"{not json"})")));
    NS_CHECK(!ok(call("Db.get", R"({"ns":"secrets","key":"x"})")));
    NS_CHECK(ok(call("Db.get", R"({"ns":"catalog","key":"x"})")));
    NS_CHECK(!ok(call("Db.set", R"({"ns":"catalog","key":"x","value":"1"})")));
    r = call("Db.keys", R"({"ns":"settings","prefix":""})");
    NS_CHECK(ok(r) && r.find("theme") != std::string::npos);
    r = call("Db.info", "null");
    NS_CHECK(ok(r) && r.find("sqlite") != std::string::npos);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::optional<fs::path> install_child;
    std::optional<fs::path> db_child;
    boson::db::Backend child_backend = boson::db::default_backend();
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg.starts_with(L"--install-crash-child=")) install_child = fs::path(arg.substr(22));
        if (arg.starts_with(L"--db-crash-child=")) db_child = fs::path(arg.substr(17));
        if (arg == L"--backend=zephyr") child_backend = boson::db::Backend::Zephyr;
        if (arg == L"--backend=sqlite") child_backend = boson::db::Backend::Sqlite;
    }
    if (install_child) return run_install_crash_child(*install_child);
    if (db_child) return run_db_crash_child(*db_child, child_backend);

    g_root = fs::temp_directory_path() / (L"boson-native-test-" + std::to_wstring(GetCurrentProcessId()));
    (void)boson::fs::remove_tree(g_root);
    fs::create_directories(g_root);

    test_hashing();
    test_zip();
    test_install();
    test_proxy_parsing();
    test_http();
    test_controllers();
    for (const auto backend : {boson::db::Backend::Sqlite, boson::db::Backend::Zephyr}) {
        if (boson::db::backend_available(backend)) {
            test_db(backend);
        }
    }

    (void)boson::fs::remove_tree(g_root);
    std::cout << "\n" << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
