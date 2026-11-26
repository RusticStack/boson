// Transfer workstream tests: base64, pool sizing, Range parsing, safe path mapping, StreamServer, FileDrop
// (headless), then BinaryTransport / FileDrop / StreamServer against the installed WebView2 runtime plus a
// JSON-base64 vs shared-buffer throughput benchmark. The WebView2 part skips when no runtime is installed.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

namespace tt {

struct EchoRequest {
    std::string text;
};

struct EchoResult {
    std::string text;
};

struct GenParams {
    std::uint64_t n = 0;
};

class EchoController {
public:
    BOSON_CONTROLLER("Test");
    [[nodiscard]] EchoResult echo(EchoRequest req) const { return EchoResult{.text = req.text + "!"}; }
};

} // namespace tt

template <>
struct glz::meta<tt::EchoController> {
    using T = tt::EchoController;
    static constexpr auto value = glz::object("echo", &T::echo);
};

namespace {

using boson::transfer::BinaryTransport;
using boson::transfer::FileDrop;
using boson::transfer::RangeKind;
using boson::transfer::StreamRequest;
using boson::transfer::StreamServer;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_transfer.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define TT_CHECK(...) check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

std::span<const std::byte> bytes_of(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::byte pattern(std::uint64_t i) {
    return static_cast<std::byte>((i * 31 + 7) % 251);
}

std::vector<std::byte> make_pattern(std::size_t n) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = pattern(i);
    }
    return v;
}

std::vector<std::byte> read_all(IStream* stream) {
    std::vector<std::byte> out;
    if (stream == nullptr) {
        return out;
    }
    std::byte chunk[8192];
    for (;;) {
        ULONG got = 0;
        const HRESULT hr = stream->Read(chunk, sizeof(chunk), &got);
        out.insert(out.end(), chunk, chunk + got);
        if (FAILED(hr) || got == 0 || hr == S_FALSE) {
            break;
        }
    }
    return out;
}

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
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

std::optional<std::string> eval(boson::core::WebView& webview, std::string_view js, std::chrono::milliseconds timeout = std::chrono::seconds{30}) {
    bool done = false;
    std::optional<std::string> result;
    if (!webview.execute_script(js, [&](HRESULT hr, std::string value) {
            if (SUCCEEDED(hr)) {
                result = std::move(value);
            }
            done = true;
        })) {
        return std::nullopt;
    }
    pump_until([&]() { return done; }, timeout);
    return result;
}

bool eval_true(boson::core::WebView& webview, std::string_view js) {
    return eval(webview, js) == std::optional<std::string>{"true"};
}

bool wait_js(boson::core::WebView& webview, std::string_view js, std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
    return pump_until([&]() { return eval_true(webview, js); }, timeout);
}

std::filesystem::path temp_dir(std::wstring_view tag) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    auto dir = std::filesystem::path(temp) / (std::wstring(tag) + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    return dir;
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

HDROP make_hdrop(const std::vector<std::wstring>& paths, POINT pt) {
    std::size_t chars = 1;
    for (const auto& p : paths) {
        chars += p.size() + 1;
    }
    const std::size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GHND, bytes);
    if (mem == nullptr) {
        return nullptr;
    }
    auto* df = static_cast<DROPFILES*>(GlobalLock(mem));
    df->pFiles = sizeof(DROPFILES);
    df->pt = pt;
    df->fNC = FALSE;
    df->fWide = TRUE;
    auto* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<std::byte*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths) {
        std::memcpy(dst, p.c_str(), p.size() * sizeof(wchar_t));
        dst += p.size() + 1;
    }
    GlobalUnlock(mem);
    return static_cast<HDROP>(mem);
}

// ---- headless ------------------------------------------------------------------------------------------

void test_base64() {
    step("base64");
    using boson::transfer::base64_decode;
    using boson::transfer::base64_encode;
    const std::pair<std::string_view, std::string_view> vectors[] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
    };
    for (const auto& [plain, encoded] : vectors) {
        TT_CHECK(base64_encode(bytes_of(plain)) == encoded);
        const auto decoded = base64_decode(encoded);
        TT_CHECK(decoded.has_value() && decoded->size() == plain.size() &&
                 std::memcmp(decoded->data(), plain.data(), plain.size()) == 0);
    }
    TT_CHECK(base64_decode("Zm9vYg").has_value()); // unpadded
    TT_CHECK(!base64_decode("Zg=a").has_value());
    TT_CHECK(!base64_decode("@@@@").has_value());
    TT_CHECK(!base64_decode("Z").has_value());
    TT_CHECK(!base64_decode("Zg===").has_value());
    TT_CHECK(!base64_decode("Zm9v Yg==").has_value());
    bool all = true;
    for (std::size_t n = 0; n < 300; ++n) {
        const auto data = make_pattern(n);
        const auto round = base64_decode(base64_encode(data));
        all = all && round.has_value() && *round == data;
    }
    TT_CHECK(all);
}

void test_buckets() {
    step("shared-buffer pool size classes");
    TT_CHECK(BinaryTransport::bucket_size(0) == 64 * 1024);
    TT_CHECK(BinaryTransport::bucket_size(1) == 64 * 1024);
    TT_CHECK(BinaryTransport::bucket_size(64 * 1024 + 1) == 128 * 1024);
    TT_CHECK(BinaryTransport::bucket_size(1'000'000) == 1024 * 1024);
    TT_CHECK(BinaryTransport::bucket_size(1024 * 1024) == 1024 * 1024);
    TT_CHECK(BinaryTransport::bucket_size(16ULL * 1024 * 1024) == 16ULL * 1024 * 1024);
    TT_CHECK(BinaryTransport::bucket_size(16ULL * 1024 * 1024 + 1) == 20ULL * 1024 * 1024);
    TT_CHECK(BinaryTransport::bucket_size(64ULL * 1024 * 1024) == 64ULL * 1024 * 1024);
}

void test_ranges() {
    step("HTTP Range parsing");
    using R = boson::transfer::ByteRange;
    auto p = [](std::string_view h, std::uint64_t size, std::uint64_t cap = 0) { return StreamServer::parse_range(h, size, cap); };
    TT_CHECK(p("bytes=0-99", 1000).kind == RangeKind::Satisfiable && p("bytes=0-99", 1000).range == R{0, 99});
    TT_CHECK(p("bytes=500-", 1000).range == R{500, 999});
    TT_CHECK(p("bytes=-100", 1000).range == R{900, 999});
    TT_CHECK(p("bytes=-5000", 1000).range == R{0, 999});
    TT_CHECK(p("bytes=900-5000", 1000).range == R{900, 999});
    TT_CHECK(p(" Bytes=1-2 ", 1000).range == R{1, 2});
    TT_CHECK(p("bytes=1000-", 1000).kind == RangeKind::Unsatisfiable);
    TT_CHECK(p("bytes=1000-1001", 1000).kind == RangeKind::Unsatisfiable);
    TT_CHECK(p("bytes=-0", 1000).kind == RangeKind::Unsatisfiable);
    TT_CHECK(p("bytes=0-", 0).kind == RangeKind::Unsatisfiable);
    TT_CHECK(p("bytes=5-1", 1000).kind == RangeKind::None);
    TT_CHECK(p("bytes=0-1,5-6", 1000).kind == RangeKind::None);
    TT_CHECK(p("items=0-1", 1000).kind == RangeKind::None);
    TT_CHECK(p("bytes=abc", 1000).kind == RangeKind::None);
    TT_CHECK(p("bytes=0-", 10'000'000, 1'000'000).range == R{0, 999'999});
    TT_CHECK(p("bytes=0-1999999", 10'000'000, 1'000'000).range == R{0, 1'999'999}); // explicit end is honoured
}

void test_paths() {
    step("percent decoding and traversal-safe path mapping");
    TT_CHECK(StreamServer::percent_decode("a%20b%2Fc") == std::optional<std::string>{"a b/c"});
    TT_CHECK(!StreamServer::percent_decode("%zz").has_value());
    TT_CHECK(!StreamServer::percent_decode("%2").has_value());
    TT_CHECK(!StreamServer::percent_decode("a%00b").has_value());
    const std::filesystem::path root = L"C:\\root";
    TT_CHECK(StreamServer::safe_join(root, "/a/b.txt") == std::optional{root / L"a" / L"b.txt"});
    TT_CHECK(StreamServer::safe_join(root, "//a/./b") == std::optional{root / L"a" / L"b"});
    TT_CHECK(StreamServer::safe_join(root, "/%E6%97%A5.mp4") == std::optional{root / L"\u65E5.mp4"});
    TT_CHECK(!StreamServer::safe_join(root, "/../x").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/a/%2e%2e/x").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/a/..%2f..%2fx").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/a%5c..%5cx").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/C:/Windows/win.ini").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/file.txt:stream").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/con").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/aux.txt").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/com1.mp4").has_value());
    TT_CHECK(StreamServer::safe_join(root, "/console.log").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/a./b").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/a /b").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/a%01b").has_value());
    TT_CHECK(!StreamServer::safe_join(root, "/%FF%FE").has_value());
    TT_CHECK(StreamServer::is_within(L"\\\\?\\C:\\root", L"\\\\?\\C:\\root\\a\\b"));
    TT_CHECK(StreamServer::is_within(L"\\\\?\\C:\\root", L"\\\\?\\c:\\ROOT\\a"));
    TT_CHECK(StreamServer::is_within(L"\\\\?\\C:\\root\\", L"\\\\?\\C:\\root\\a"));
    TT_CHECK(StreamServer::is_within(L"\\\\?\\C:\\", L"\\\\?\\C:\\x"));
    TT_CHECK(!StreamServer::is_within(L"\\\\?\\C:\\root", L"\\\\?\\C:\\rootx\\a"));
    TT_CHECK(!StreamServer::is_within(L"\\\\?\\C:\\root", L"\\\\?\\C:\\ro"));
    TT_CHECK(StreamServer::mime_type_for("a/b.MP4") == "video/mp4");
    TT_CHECK(StreamServer::mime_type_for("x.webm") == "video/webm");
    TT_CHECK(StreamServer::mime_type_for("x.mp3") == "audio/mpeg");
    TT_CHECK(StreamServer::mime_type_for("x.wav") == "audio/wav");
    TT_CHECK(StreamServer::mime_type_for("index.html") == "text/html; charset=utf-8");
    TT_CHECK(StreamServer::mime_type_for("noext") == "application/octet-stream");
}

void test_stream_server_headless(boson::exec::TaskRunner& runner, boson::exec::UiMarshal& marshal) {
    step("StreamServer::resolve (files, ranges, generators, junction escape)");
    const auto dir = temp_dir(L"boson-tt-media-");
    const auto outside = temp_dir(L"boson-tt-outside-");
    const auto data = make_pattern(100'000);
    write_file(dir / L"data.bin", data);
    std::filesystem::create_directories(dir / L"sub");
    write_file(dir / L"sub" / L"clip.mp4", bytes_of("not really a video"));
    write_file(outside / L"secret.txt", bytes_of("secret"));

    boson::transfer::StreamServerOptions opts;
    opts.max_open_range_bytes = 40'000;
    StreamServer server(runner, marshal, opts);
    TT_CHECK(server.mount_directory("/files", dir));
    TT_CHECK(!server.mount_directory("/bad", dir / L"missing"));
    TT_CHECK(server.base_url() == "boson://media");

    auto get = [&](std::string path, std::optional<std::string> range = std::nullopt, std::string method = "GET") {
        StreamRequest r;
        r.method = std::move(method);
        r.path = std::move(path);
        r.range_header = std::move(range);
        return server.resolve(r);
    };

    {
        const auto r = get("/files/data.bin");
        TT_CHECK(r.status == 200);
        TT_CHECK(r.header("Content-Length") == std::optional<std::string>{"100000"});
        TT_CHECK(r.header("accept-ranges") == std::optional<std::string>{"bytes"});
        TT_CHECK(r.header("Content-Type") == std::optional<std::string>{"application/octet-stream"});
        TT_CHECK(read_all(r.body.Get()) == data);
    }
    {
        const auto r = get("/files/data.bin", "bytes=10-19");
        TT_CHECK(r.status == 206);
        TT_CHECK(r.header("Content-Range") == std::optional<std::string>{"bytes 10-19/100000"});
        TT_CHECK(r.header("Content-Length") == std::optional<std::string>{"10"});
        const auto body = read_all(r.body.Get());
        TT_CHECK(body.size() == 10 && std::equal(body.begin(), body.end(), data.begin() + 10));
    }
    {
        const auto r = get("/files/data.bin", "bytes=-5");
        const auto body = read_all(r.body.Get());
        TT_CHECK(r.status == 206 && body.size() == 5 && std::equal(body.begin(), body.end(), data.end() - 5));
    }
    {
        const auto r = get("/files/data.bin", "bytes=0-");
        TT_CHECK(r.status == 206);
        TT_CHECK(r.header("Content-Range") == std::optional<std::string>{"bytes 0-39999/100000"});
        TT_CHECK(read_all(r.body.Get()).size() == 40'000);
    }
    {
        // Seek + Stat on the file stream.
        const auto r = get("/files/data.bin", "bytes=1000-1999");
        STATSTG st{};
        TT_CHECK(r.body && SUCCEEDED(r.body->Stat(&st, STATFLAG_NONAME)) && st.cbSize.QuadPart == 1000);
        LARGE_INTEGER move{};
        move.QuadPart = 500;
        ULARGE_INTEGER pos{};
        TT_CHECK(SUCCEEDED(r.body->Seek(move, STREAM_SEEK_SET, &pos)) && pos.QuadPart == 500);
        const auto body = read_all(r.body.Get());
        TT_CHECK(body.size() == 500 && body.front() == data[1500]);
    }
    TT_CHECK(get("/files/data.bin", "bytes=200000-").status == 416);
    TT_CHECK(get("/files/data.bin", "bytes=200000-").header("Content-Range") == std::optional<std::string>{"bytes */100000"});
    TT_CHECK(get("/files/data.bin", "bytes=0-1,4-5").status == 200);
    {
        const auto r = get("/files/data.bin", std::nullopt, "HEAD");
        TT_CHECK(r.status == 200 && !r.body && r.header("Content-Length") == std::optional<std::string>{"100000"});
    }
    TT_CHECK(get("/files/sub/clip.mp4").header("Content-Type") == std::optional<std::string>{"video/mp4"});
    TT_CHECK(get("/files/missing.bin").status == 404);
    TT_CHECK(get("/files/sub").status == 404);
    TT_CHECK(get("/files/../outside/secret.txt").status == 403);
    TT_CHECK(get("/files/%2e%2e/x").status == 403);
    TT_CHECK(get("/other/data.bin").status == 404);
    TT_CHECK(get("/files/data.bin", std::nullopt, "POST").status == 405);
    TT_CHECK(get("/files/data.bin", std::nullopt, "OPTIONS").status == 204);

    // A junction inside the root that points outside it must not be followed.
    const auto junction = dir / L"escape";
    const std::wstring cmd = L"cmd /c mklink /J \"" + junction.wstring() + L"\" \"" + outside.wstring() + L"\" >nul 2>&1";
    if (_wsystem(cmd.c_str()) == 0 && std::filesystem::exists(junction / L"secret.txt")) {
        TT_CHECK(get("/files/escape/secret.txt").status == 403);
        RemoveDirectoryW(junction.c_str()); // removes the junction only
    } else {
        std::cout << "  (mklink /J unavailable; junction escape check skipped)\n";
    }

    // Generators: sized (Range-capable) and unsized (streamed).
    server.mount_generator("/gen", [](const StreamRequest& req) -> std::optional<boson::transfer::GeneratedResource> {
        if (req.path == "/sized.bin") {
            return boson::transfer::GeneratedResource{
                .mime_type = "application/x-test",
                .size = 5000,
                .read = [](std::uint64_t offset, std::span<std::byte> out) {
                    for (std::size_t i = 0; i < out.size(); ++i) out[i] = pattern(offset + i);
                    return out.size();
                },
                .extra_headers = {{"X-Gen", "1"}},
            };
        }
        if (req.path == "/stream.txt") {
            auto counter = std::make_shared<std::atomic<int>>(0);
            return boson::transfer::GeneratedResource{
                .mime_type = "text/plain",
                .size = std::nullopt,
                .read = [counter](std::uint64_t, std::span<std::byte> out) -> std::size_t {
                    if (counter->fetch_add(1) >= 3 || out.size() < 5) return 0;
                    std::memcpy(out.data(), "chunk", 5);
                    return 5;
                },
            };
        }
        return std::nullopt;
    });
    {
        const auto r = get("/gen/sized.bin", "bytes=4990-");
        const auto body = read_all(r.body.Get());
        TT_CHECK(r.status == 206 && body.size() == 10 && body[0] == pattern(4990));
        TT_CHECK(r.header("X-Gen") == std::optional<std::string>{"1"});
        TT_CHECK(r.header("Content-Type") == std::optional<std::string>{"application/x-test"});
    }
    {
        const auto r = get("/gen/stream.txt", "bytes=0-1");
        const auto body = read_all(r.body.Get());
        TT_CHECK(r.status == 200 && r.header("Accept-Ranges") == std::optional<std::string>{"none"});
        TT_CHECK(body.size() == 15);
        TT_CHECK(!r.header("Content-Length").has_value());
    }
    TT_CHECK(get("/gen/nothing").status == 404);
    TT_CHECK(server.unmount("/gen"));
    TT_CHECK(get("/gen/sized.bin").status == 404);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove_all(outside, ec);
}

void test_file_drop_headless() {
    step("FileDrop: HDROP parsing, WM_DROPFILES, drop message parsing");
    const std::vector<std::wstring> paths = {L"C:\\data\\a.txt", L"D:\\\u65E5\u672C\\b c.mp4"};
    HDROP drop = make_hdrop(paths, POINT{12, 34});
    TT_CHECK(drop != nullptr);
    const auto parsed = FileDrop::paths_from_hdrop(drop);
    TT_CHECK(parsed.size() == 2 && parsed[0] == "C:\\data\\a.txt" && parsed[1] == boson::core::wide_to_utf8(paths[1]));
    GlobalFree(drop);
    TT_CHECK(FileDrop::paths_from_hdrop(nullptr).empty());
    TT_CHECK(FileDrop::paths_from_objects(nullptr).empty());

    const auto msg = FileDrop::parse_drop_message(R"({"__boson_transfer":"drop","zone":"z1","x":10.5,"y":20,"meta":{"k":[1,2]}})");
    TT_CHECK(msg.has_value() && msg->zone == "z1" && msg->x == 10.5 && msg->y == 20.0 && msg->metadata_json == R"({"k":[1,2]})");
    TT_CHECK(!FileDrop::parse_drop_message(R"({"id":1,"method":"x"})").has_value());
    TT_CHECK(!FileDrop::parse_drop_message(R"({"__boson_transfer":"ack","tid":1})").has_value());
    TT_CHECK(FileDrop::page_script().find(L"postMessageWithAdditionalObjects") != std::wstring::npos);

    boson::core::WindowConfig cfg;
    cfg.title = L"Boson transfer drop test";
    cfg.width = 300;
    cfg.height = 200;
    cfg.quit_on_destroy = false;
    boson::core::Window window(cfg);
    TT_CHECK(window.create(nullptr, SW_HIDE));
    std::vector<boson::transfer::FileDropEvent> events;
    {
        FileDrop file_drop(window, [&](const boson::transfer::FileDropEvent& e) { events.push_back(e); });
        TT_CHECK(file_drop.accepts_window_drops());
        TT_CHECK((GetWindowLongPtrW(window.hwnd(), GWL_EXSTYLE) & WS_EX_ACCEPTFILES) != 0);
        SendMessageW(window.hwnd(), WM_DROPFILES, reinterpret_cast<WPARAM>(make_hdrop(paths, POINT{40, 50})), 0);
        TT_CHECK(events.size() == 1);
        if (!events.empty()) {
            TT_CHECK(events[0].target == boson::transfer::DropTarget::Window);
            TT_CHECK(events[0].paths.size() == 2 && events[0].paths[0] == "C:\\data\\a.txt");
            TT_CHECK(events[0].client_px.x == 40 && events[0].client_px.y == 50);
            TT_CHECK(events[0].webview_id == 0);
        }
        file_drop.set_accept_window_drops(false);
        TT_CHECK((GetWindowLongPtrW(window.hwnd(), GWL_EXSTYLE) & WS_EX_ACCEPTFILES) == 0);
        SendMessageW(window.hwnd(), WM_DROPFILES, reinterpret_cast<WPARAM>(make_hdrop(paths, POINT{1, 1})), 0);
        TT_CHECK(events.size() == 1);
        file_drop.set_accept_window_drops(true);
    }
    TT_CHECK((GetWindowLongPtrW(window.hwnd(), GWL_EXSTYLE) & WS_EX_ACCEPTFILES) == 0);
    window.destroy();
}

// ---- WebView2 ------------------------------------------------------------------------------------------

constexpr std::string_view kPage =
    "<!doctype html><html><head><title>Transfer</title><script>"
    "window.__responses=[];"
    "chrome.webview.addEventListener('message',e=>{const d=e.data;if(d&&typeof d.id==='number')window.__responses.push(d);});"
    "</script></head><body style='margin:0'><div id=zone style='width:200px;height:100px'>zone</div></body></html>";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/index.html") {
        return bytes_of(kPage);
    }
    return std::nullopt;
}

/// 16-bit mono PCM WAV of a quiet tone, generated on demand (header + samples) for the <audio> seek test.
struct WavSource {
    static constexpr std::uint32_t kRate = 44'100;
    static constexpr std::uint32_t kSeconds = 60;
    static constexpr std::uint64_t kDataBytes = static_cast<std::uint64_t>(kRate) * kSeconds * 2;
    static constexpr std::uint64_t kSize = 44 + kDataBytes;

    static std::byte at(std::uint64_t offset) {
        if (offset < 44) {
            std::byte header[44]{};
            const auto put32 = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF); };
            const auto put16 = [&](std::size_t at, std::uint16_t v) { header[at] = static_cast<std::byte>(v & 0xFF); header[at + 1] = static_cast<std::byte>(v >> 8); };
            std::memcpy(header, "RIFF", 4);
            put32(4, static_cast<std::uint32_t>(36 + kDataBytes));
            std::memcpy(header + 8, "WAVEfmt ", 8);
            put32(16, 16);
            put16(20, 1);
            put16(22, 1);
            put32(24, kRate);
            put32(28, kRate * 2);
            put16(32, 2);
            put16(34, 16);
            std::memcpy(header + 36, "data", 4);
            put32(40, static_cast<std::uint32_t>(kDataBytes));
            return header[offset];
        }
        const std::uint64_t sample_index = (offset - 44) / 2;
        const auto sample = static_cast<std::int16_t>(800.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * static_cast<double>(sample_index) / kRate));
        const auto u = static_cast<std::uint16_t>(sample);
        return static_cast<std::byte>(((offset - 44) % 2 == 0) ? (u & 0xFF) : (u >> 8));
    }
};

struct BenchResult {
    double ms_per_op = 0.0;
    double mb_per_s = 0.0;
};

void print_bench(std::string_view label, std::size_t bytes, int iterations, double total_ms) {
    const double per = total_ms / iterations;
    const double mbps = (static_cast<double>(bytes) / (1024.0 * 1024.0)) / (per / 1000.0);
    std::cout << "  [bench] " << std::left << std::setw(34) << label << std::right << std::fixed << std::setprecision(2) << std::setw(9)
              << per << " ms/op " << std::setw(9) << mbps << " MiB/s  (" << iterations << " iterations)\n";
}

int run_webview_tests() {
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
        std::cout << "SKIP WebView2 part: no WebView2 runtime installed\n";
        return 0;
    }
    std::wcout << L"WebView2 runtime " << version << L'\n';
    CoTaskMemFree(version);

    const auto user_data = temp_dir(L"boson-tt-udf-");
    const auto media_dir = temp_dir(L"boson-tt-wvmedia-");
    const auto media_data = make_pattern(300'000);
    write_file(media_dir / L"data.bin", media_data);

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson transfer test";
    window_cfg.width = 640;
    window_cfg.height = 420;
    window_cfg.quit_on_destroy = false;
    auto window = std::make_unique<boson::core::Window>(window_cfg);
    TT_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

    boson::exec::Scheduler scheduler(4);
    boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
    window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    auto runner = std::make_unique<boson::exec::TaskRunner>(scheduler, marshal);

    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = user_data.wstring();
    const auto env = boson::core::Environment::create(env_cfg);
    auto host = std::make_unique<boson::core::WebViewHost>(*window, env);
    host->set_default_asset_lookup(&lookup);
    bool nav = false;
    boson::core::WebView& wv = host->create({.initial_url = "boson://app/index.html"});
    wv.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { nav = e.is_success; };
    TT_CHECK(pump_until([&]() { return wv.is_ready() && nav; }, std::chrono::seconds{30}));
    if (!wv.is_ready()) {
        std::cerr << "WebView did not become ready\n";
        return 1;
    }

    tt::EchoController echo;
    boson::ipc::Router<tt::EchoController> router(echo);
    auto bridge = std::make_unique<boson::ipc::Bridge>(*runner, marshal, router);
    bridge->attach(wv);

    boson::transfer::BinaryTransportOptions topts;
    topts.ack_timeout = std::chrono::milliseconds{1500};
    auto transport = std::make_unique<BinaryTransport>(*runner, marshal, topts);
    transport->attach(wv);

    std::vector<boson::transfer::FileDropEvent> drops;
    auto file_drop = std::make_unique<FileDrop>(*window, [&](const boson::transfer::FileDropEvent& e) { drops.push_back(e); });
    file_drop->attach(wv);

    boson::transfer::StreamServerOptions sopts;
    auto streams = std::make_unique<StreamServer>(*runner, marshal, sopts);
    TT_CHECK(streams->mount_directory("/files", media_dir));
    std::mutex range_mutex;
    std::vector<std::string> wav_ranges;
    streams->mount_generator("/gen", [&](const StreamRequest& req) -> std::optional<boson::transfer::GeneratedResource> {
        if (req.path != "/tone.wav") {
            return std::nullopt;
        }
        {
            std::scoped_lock lock(range_mutex);
            wav_ranges.push_back(req.range_header.value_or("(none)"));
        }
        return boson::transfer::GeneratedResource{
            .mime_type = "audio/wav",
            .size = WavSource::kSize,
            .read = [](std::uint64_t offset, std::span<std::byte> out) {
                const std::uint64_t n = std::min<std::uint64_t>(out.size(), WavSource::kSize > offset ? WavSource::kSize - offset : 0);
                for (std::uint64_t i = 0; i < n; ++i) out[i] = WavSource::at(offset + i);
                return static_cast<std::size_t>(n);
            },
        };
    });
    streams->attach(wv);

    // Test-only observer for benchmark completions ({"bench_done":...}).
    std::uint64_t bench_done = 0;
    const auto bench_hook = wv.add_web_message_observer([&](const boson::core::WebView::WebMessage& m) {
        if (m.json.starts_with("{\"bench_done\"")) {
            ++bench_done;
            return true;
        }
        return false;
    });

    // ---- binary transport ----------------------------------------------------------------------------
    step("BinaryTransport: feature detection and page runtime");
    TT_CHECK(BinaryTransport::supports_shared_buffers(wv));
    TT_CHECK(transport->mode(wv.id()) == boson::transfer::TransferMode::SharedBuffer);
    TT_CHECK(transport->is_attached(wv.id()));
    TT_CHECK(wait_js(wv, "typeof window.__bosonTransfer === 'object' && window.__bosonTransfer.version === 1"));
    TT_CHECK(eval_true(wv,
        "window.__got=[];window.__bosonTransfer.onBinary('t',m=>{const d=m.data;let s=0;for(let i=0;i<d.length;i+=97)s=(s+d[i])|0;"
        "window.__got.push({len:d.length,mode:m.mode,first:d[0],last:d[d.length-1],sum:s,meta:m.meta});});true"));
    const auto js_sum = [](const std::vector<std::byte>& d) {
        std::int32_t s = 0;
        for (std::size_t i = 0; i < d.size(); i += 97) s += static_cast<std::int32_t>(d[i]);
        return s;
    };

    // JSON RPC still flows through the Bridge with observers installed.
    TT_CHECK(eval(wv, "chrome.webview.postMessage({id:1,method:'Test.echo',params:{text:'hi'}}); 1").has_value());
    TT_CHECK(wait_js(wv, "window.__responses.length === 1 && window.__responses[0].data.text === 'hi!'"));

    step("BinaryTransport: native -> page over shared buffers, pooling");
    {
        const auto data = make_pattern(1000);
        TT_CHECK(transport->send_binary(wv.id(), "t", data, R"({"n":1})"));
        TT_CHECK(wait_js(wv, "window.__got.length === 1"));
        TT_CHECK(eval(wv, "JSON.stringify(window.__got[0])") ==
                 std::optional<std::string>{"\"{\\\"len\\\":1000,\\\"mode\\\":\\\"shared\\\",\\\"first\\\":" + std::to_string(static_cast<int>(data[0])) +
                                            ",\\\"last\\\":" + std::to_string(static_cast<int>(data[999])) + ",\\\"sum\\\":" + std::to_string(js_sum(data)) +
                                            ",\\\"meta\\\":{\\\"n\\\":1}}\""});
        TT_CHECK(pump_until([&]() { return transport->stats().buffers_in_flight == 0; }));
        const auto s1 = transport->stats();
        TT_CHECK(s1.shared_sends == 1 && s1.buffers_created == 1 && s1.idle_buffers == 1 && s1.idle_bytes == 64 * 1024);

        TT_CHECK(transport->send_binary(wv.id(), "t", data));
        TT_CHECK(wait_js(wv, "window.__got.length === 2 && window.__got[1].meta === null"));
        TT_CHECK(pump_until([&]() { return transport->stats().buffers_in_flight == 0; }));
        TT_CHECK(transport->stats().buffers_reused == 1 && transport->stats().buffers_created == 1);

        // From a worker thread: needs a new 512 KiB buffer, created through UiMarshal::invoke_sync.
        const auto big = make_pattern(300'000);
        std::atomic<bool> sent{false};
        runner->spawn([&]() { sent = transport->send_binary(wv.id(), "t", big); });
        TT_CHECK(pump_until([&]() { return sent.load(); }));
        TT_CHECK(wait_js(wv, "window.__got.length === 3 && window.__got[2].len === 300000 && window.__got[2].sum === " + std::to_string(js_sum(big))));

        transport->post_binary(wv.id(), "t", make_pattern(5000), R"("posted")");
        TT_CHECK(wait_js(wv, "window.__got.length === 4 && window.__got[3].meta === 'posted' && window.__got[3].len === 5000"));

        TT_CHECK(transport->send_binary_with(wv.id(), "t", 4096, [](std::span<std::byte> out) {
            for (std::size_t i = 0; i < out.size(); ++i) out[i] = pattern(i);
        }));
        TT_CHECK(wait_js(wv, "window.__got.length === 5 && window.__got[4].len === 4096 && window.__got[4].mode === 'shared'"));
        TT_CHECK(!transport->send_binary(wv.id(), "t", data, "{not json"));
        TT_CHECK(!transport->send_binary(987654, "t", data));
    }

    step("BinaryTransport: base64 fallback");
    {
        transport->set_force_base64(true);
        TT_CHECK(transport->mode(wv.id()) == boson::transfer::TransferMode::Base64);
        const auto data = make_pattern(777);
        TT_CHECK(transport->send_binary(wv.id(), "t", data, "[1,2]"));
        TT_CHECK(wait_js(wv, "window.__got.length === 6 && window.__got[5].mode === 'base64' && window.__got[5].len === 777 && window.__got[5].sum === " +
                                 std::to_string(js_sum(data)) + " && window.__got[5].meta[1] === 2"));
        TT_CHECK(transport->send_binary(wv.id(), "t", std::span<const std::byte>{}));
        TT_CHECK(wait_js(wv, "window.__got.length === 7 && window.__got[6].len === 0"));
        transport->set_force_base64(false);
    }

    step("BinaryTransport: page requests (requestBinary)");
    {
        transport->on_request("gen", [](const boson::transfer::BinaryRequest& req) {
            tt::GenParams p;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(p, req.params_json)) {
                throw std::runtime_error("bad params");
            }
            if (p.n == 13) {
                throw std::runtime_error("unlucky");
            }
            if (p.n > 100'000) {
                boson::transfer::BinaryReply reply;
                reply.size = p.n;
                reply.fill = [](std::span<std::byte> out) { for (std::size_t i = 0; i < out.size(); ++i) out[i] = pattern(i); };
                reply.metadata_json = R"({"filled":true})";
                return reply;
            }
            return boson::transfer::BinaryReply{.data = make_pattern(p.n), .metadata_json = R"({"n":)" + std::to_string(p.n) + "}"};
        });
        TT_CHECK(eval_true(wv, "window.__req=null;window.__bosonTransfer.request('gen',{n:5000}).then(r=>{window.__req={len:r.data.length,mode:r.mode,meta:r.meta,b:r.data[4999]};});true"));
        TT_CHECK(wait_js(wv, "window.__req !== null && window.__req.len === 5000 && window.__req.mode === 'shared' && window.__req.meta.n === 5000 && window.__req.b === " +
                                 std::to_string(static_cast<int>(pattern(4999)))));
        TT_CHECK(eval_true(wv, "window.__req=null;window.__bosonTransfer.request('gen',{n:200000},{zeroCopy:true}).then(r=>{window.__req={len:r.data.length,meta:r.meta,b:r.data[199999]};r.release();});true"));
        TT_CHECK(wait_js(wv, "window.__req !== null && window.__req.len === 200000 && window.__req.meta.filled === true && window.__req.b === " +
                                 std::to_string(static_cast<int>(pattern(199999)))));
        TT_CHECK(eval_true(wv, "window.__err=null;window.__bosonTransfer.request('gen',{n:13}).catch(e=>{window.__err=e.name+':'+e.message;});true"));
        TT_CHECK(wait_js(wv, "window.__err === 'BosonTransferError:unlucky'"));
        TT_CHECK(eval_true(wv, "window.__err=null;window.__bosonTransfer.request('nope',{}).catch(e=>{window.__err=e.message;});true"));
        TT_CHECK(wait_js(wv, "window.__err !== null && window.__err.indexOf('no request handler') >= 0"));
        transport->set_force_base64(true);
        TT_CHECK(eval_true(wv, "window.__req=null;window.__bosonTransfer.request('gen',{n:3000}).then(r=>{window.__req={len:r.data.length,mode:r.mode};});true"));
        TT_CHECK(wait_js(wv, "window.__req !== null && window.__req.len === 3000 && window.__req.mode === 'base64'"));
        transport->set_force_base64(false);
    }

    step("BinaryTransport: page -> native uploads (shared buffer and base64)");
    {
        std::atomic<bool> last_zero_copy{false};
        transport->on_upload("up", [&](const boson::transfer::BinaryUpload& up) {
            if (up.metadata_json == R"({"fail":true})") {
                throw std::runtime_error("upload refused");
            }
            last_zero_copy = up.zero_copy;
            std::uint64_t sum = 0;
            bool ok = true;
            for (std::size_t i = 0; i < up.data.size(); ++i) {
                ok = ok && up.data[i] == pattern(i);
                sum += static_cast<std::uint64_t>(up.data[i]);
            }
            return R"({"len":)" + std::to_string(up.data.size()) + R"(,"ok":)" + (ok ? "true" : "false") + R"(,"sum":)" + std::to_string(sum) +
                   R"(,"channel":")" + up.channel + R"(","meta":)" + up.metadata_json + "}";
        });
        const char* make_js = "window.__mk=(n)=>{const a=new Uint8Array(n);for(let i=0;i<n;i++)a[i]=(i*31+7)%251;return a;};true";
        TT_CHECK(eval_true(wv, make_js));
        TT_CHECK(eval_true(wv, "window.__up=null;window.__bosonTransfer.upload('up',window.__mk(3000000),{tag:'x'}).then(r=>{window.__up=r;});true"));
        TT_CHECK(wait_js(wv, "window.__up !== null && window.__up.len === 3000000 && window.__up.ok === true && window.__up.meta.tag === 'x'"));
        TT_CHECK(last_zero_copy.load());
        TT_CHECK(pump_until([&]() { return transport->stats().buffers_leased == 0; }));
        TT_CHECK(transport->stats().uploads >= 1);

        TT_CHECK(eval_true(wv, "window.__up=null;window.__bosonTransfer.upload('up',new Uint8Array(0)).then(r=>{window.__up=r;});true"));
        TT_CHECK(wait_js(wv, "window.__up !== null && window.__up.len === 0"));

        transport->set_force_base64(true);
        TT_CHECK(eval_true(wv, "window.__up=null;window.__bosonTransfer.upload('up',window.__mk(100000).buffer).then(r=>{window.__up=r;});true"));
        TT_CHECK(wait_js(wv, "window.__up !== null && window.__up.len === 100000 && window.__up.ok === true"));
        TT_CHECK(!last_zero_copy.load());
        transport->set_force_base64(false);

        TT_CHECK(eval_true(wv, "window.__err=null;window.__bosonTransfer.upload('up',window.__mk(10),{fail:true}).catch(e=>{window.__err=e.message;});true"));
        TT_CHECK(wait_js(wv, "window.__err === 'upload refused'"));
        TT_CHECK(eval_true(wv, "window.__err=null;window.__bosonTransfer.upload('missing',window.__mk(10)).catch(e=>{window.__err=e.message;});true"));
        TT_CHECK(wait_js(wv, "window.__err !== null && window.__err.indexOf('no upload handler') >= 0"));
        TT_CHECK(eval_true(wv, "window.__err=null;try{window.__bosonTransfer.upload('up',{}).catch(e=>{window.__err=e.name;});}catch(e){window.__err='sync';}true"));
        TT_CHECK(wait_js(wv, "window.__err === 'TypeError'"));
    }

    step("BinaryTransport: un-acknowledged buffers expire, trim, origin filtering");
    {
        TT_CHECK(eval_true(wv, "window.__bosonTransfer.onBinary('hold',()=>new Promise(()=>{}));true"));
        const auto before = transport->stats().ack_timeouts;
        TT_CHECK(transport->send_binary(wv.id(), "hold", make_pattern(10)));
        TT_CHECK(pump_until([&]() { return transport->stats().buffers_in_flight == 1; }, std::chrono::seconds{5}));
        TT_CHECK(pump_until([&]() { return transport->stats().ack_timeouts == before + 1; }, std::chrono::seconds{10}));
        TT_CHECK(transport->stats().buffers_in_flight == 0);
        transport->trim();
        TT_CHECK(transport->stats().idle_buffers == 0 && transport->stats().idle_bytes == 0);

        transport->attach(wv, {.allowed_origins = {"https://only.example"}, .inject_page_runtime = false});
        TT_CHECK(eval_true(wv, "window.__req='pending';window.__bosonTransfer.request('gen',{n:10}).then(()=>{window.__req='resolved';},()=>{window.__req='rejected';});true"));
        pump_for(std::chrono::milliseconds{600});
        TT_CHECK(eval(wv, "window.__req") == std::optional<std::string>{"\"pending\""});
        transport->attach(wv, {.inject_page_runtime = false});
    }

    step("TransferController");
    {
        boson::ipc::TransferController controller(*transport, wv, streams.get());
        const auto caps = controller.capabilities();
        TT_CHECK(caps.shared_buffer && caps.mode == "shared_buffer" && caps.stream_base_url == "boson://media");
        TT_CHECK(controller.stats().shared_sends >= 5);
    }

    // ---- file drop ---------------------------------------------------------------------------------
    step("FileDrop: page drop message with additional objects");
    {
        TT_CHECK(wait_js(wv, "window.__bosonFileDrop === true"));
        const std::size_t responses_before = 1;
        const auto posted = eval(wv,
            "(()=>{try{chrome.webview.postMessageWithAdditionalObjects({__boson_transfer:'drop',zone:'z',x:10,y:20,meta:{a:1}},"
            "[new File(['hello'],'x.txt')]);return 'ok';}catch(e){return 'threw:'+e.message;}})()");
        std::cout << "  postMessageWithAdditionalObjects(script File): " << posted.value_or("(no result)") << '\n';
        // Without a user gesture a page cannot get a File backed by disk, so route an empty drop instead: it must
        // be consumed by FileDrop (never reach the Bridge) and produce no event.
        TT_CHECK(eval_true(wv, "chrome.webview.postMessageWithAdditionalObjects({__boson_transfer:'drop',zone:'z',x:1,y:2,meta:null},[]);true"));
        pump_for(std::chrono::milliseconds{300});
        TT_CHECK(drops.empty());
        if (posted == std::optional<std::string>{"\"ok\""}) {
            TT_CHECK(pump_until([&]() { return !drops.empty(); }, std::chrono::seconds{5}));
            if (!drops.empty()) {
                const auto& d = drops.back();
                TT_CHECK(d.target == boson::transfer::DropTarget::WebView);
                TT_CHECK(d.webview_id == wv.id());
                TT_CHECK(d.zone == "z" && d.css_x == 10.0 && d.css_y == 20.0 && d.metadata_json == R"({"a":1})");
                TT_CHECK(d.source_uri.starts_with("boson://app"));
                std::cout << "  drop: " << d.paths.size() << " path(s), " << d.unresolved_objects << " object(s) without a path\n";
                TT_CHECK(d.paths.size() + d.unresolved_objects == 1);
                const RECT b = wv.bounds();
                TT_CHECK(d.client_px.x >= b.left + 10 && d.client_px.y >= b.top + 20);
            }
        }
        pump_for(std::chrono::milliseconds{200});
        TT_CHECK(eval(wv, "window.__responses.length") == std::optional<std::string>{std::to_string(responses_before)});
    }

    // ---- streaming ---------------------------------------------------------------------------------
    step("StreamServer through WebView2: fetch with Range, traversal, <audio> seeking");
    {
        TT_CHECK(eval_true(wv,
            "window.__f=null;fetch('boson://media/files/data.bin',{headers:{Range:'bytes=100-199'}}).then(async r=>{const b=new Uint8Array(await r.arrayBuffer());"
            "window.__f={s:r.status,cr:r.headers.get('content-range'),ar:r.headers.get('accept-ranges'),len:b.length,b0:b[0]};}).catch(e=>{window.__f={err:String(e)};});true"));
        TT_CHECK(wait_js(wv, "window.__f !== null"));
        std::cout << "  range fetch: " << eval(wv, "JSON.stringify(window.__f)").value_or("?") << '\n';
        TT_CHECK(eval_true(wv, "window.__f.s === 206 && window.__f.cr === 'bytes 100-199/300000' && window.__f.len === 100 && window.__f.b0 === " +
                                   std::to_string(static_cast<int>(media_data[100]))));
        TT_CHECK(eval_true(wv,
            "window.__f=null;fetch('boson://media/files/data.bin').then(async r=>{const b=new Uint8Array(await r.arrayBuffer());"
            "window.__f={s:r.status,len:b.length,last:b[b.length-1],ct:r.headers.get('content-type')};}).catch(e=>{window.__f={err:String(e)};});true"));
        TT_CHECK(wait_js(wv, "window.__f !== null"));
        TT_CHECK(eval_true(wv, "window.__f.s === 200 && window.__f.len === 300000 && window.__f.ct === 'application/octet-stream' && window.__f.last === " +
                                   std::to_string(static_cast<int>(media_data.back()))));
        TT_CHECK(eval_true(wv,
            "window.__f=null;fetch('boson://media/files/..%2f..%2fwin.ini').then(r=>{window.__f={s:r.status};}).catch(e=>{window.__f={err:String(e)};});true"));
        TT_CHECK(wait_js(wv, "window.__f !== null"));
        TT_CHECK(eval_true(wv, "window.__f.s === 403"));
        TT_CHECK(eval_true(wv,
            "window.__f=null;fetch('boson://media/files/missing.bin').then(r=>{window.__f={s:r.status};}).catch(e=>{window.__f={err:String(e)};});true"));
        TT_CHECK(wait_js(wv, "window.__f !== null"));
        TT_CHECK(eval_true(wv, "window.__f.s === 404"));

        TT_CHECK(eval_true(wv,
            "window.__a={};const a=document.createElement('audio');a.muted=true;a.preload='auto';window.__audio=a;"
            "a.addEventListener('loadedmetadata',()=>{window.__a.dur=a.duration;});"
            "a.addEventListener('seeked',()=>{window.__a.seeked=a.currentTime;});"
            "a.addEventListener('error',()=>{window.__a.err=a.error&&a.error.code;});"
            "a.src='boson://media/gen/tone.wav';document.body.appendChild(a);true"));
        const bool loaded = wait_js(wv, "window.__a.dur !== undefined || window.__a.err !== undefined", std::chrono::seconds{20});
        TT_CHECK(loaded);
        std::cout << "  audio: " << eval(wv, "JSON.stringify(window.__a)").value_or("?") << '\n';
        TT_CHECK(eval_true(wv, "Math.abs(window.__a.dur - 60) < 0.5"));
        TT_CHECK(eval_true(wv, "window.__audio.currentTime = 50; true"));
        TT_CHECK(wait_js(wv, "window.__a.seeked !== undefined", std::chrono::seconds{20}));
        TT_CHECK(eval_true(wv, "Math.abs(window.__a.seeked - 50) < 0.5"));
        std::vector<std::string> ranges;
        {
            std::scoped_lock lock(range_mutex);
            ranges = wav_ranges;
        }
        std::cout << "  <audio> requests:";
        bool mid_file_range = false;
        for (const auto& r : ranges) {
            std::cout << ' ' << r;
            if (r.starts_with("bytes=") && r.size() > 6 && r[6] != '0') {
                mid_file_range = true;
            }
        }
        std::cout << '\n';
        TT_CHECK(!ranges.empty());
        std::cout << "  mid-file Range request after seeking: " << (mid_file_range ? "yes" : "no") << '\n';
    }

    // ---- benchmark ---------------------------------------------------------------------------------
    step("benchmark: native -> page, JSON-base64 vs shared buffer");
    {
        TT_CHECK(eval_true(wv,
            "window.__bosonTransfer.onBinary('bench',m=>{const d=m.data;let s=0;for(let i=0;i<d.length;i+=4096)s+=d[i];"
            "chrome.webview.postMessage({bench_done:d.length,s});});true"));
        struct Case {
            std::size_t bytes;
            int iterations;
        };
        for (const Case c : {Case{1024 * 1024, 20}, Case{64ULL * 1024 * 1024, 3}}) {
            const auto payload = make_pattern(c.bytes);
            for (const bool b64 : {true, false}) {
                transport->set_force_base64(b64);
                // warm-up (creates the pooled buffer / JIT)
                bench_done = 0;
                transport->send_binary(wv.id(), "bench", payload);
                TT_CHECK(pump_until([&]() { return bench_done == 1; }, std::chrono::seconds{120}));
                pump_until([&]() { return transport->stats().buffers_in_flight == 0; }, std::chrono::seconds{10});
                bench_done = 0;
                const auto t0 = std::chrono::steady_clock::now();
                bool ok = true;
                for (int i = 0; i < c.iterations && ok; ++i) {
                    ok = transport->send_binary(wv.id(), "bench", payload) &&
                         pump_until([&]() { return bench_done == static_cast<std::uint64_t>(i + 1); }, std::chrono::seconds{120});
                    if (!b64) {
                        pump_until([&]() { return transport->stats().buffers_in_flight == 0; }, std::chrono::seconds{10});
                    }
                }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                TT_CHECK(ok);
                print_bench(std::string(b64 ? "native->page base64 " : "native->page shared ") + (c.bytes > 2 * 1024 * 1024 ? "64 MiB" : "1 MiB"),
                            c.bytes, c.iterations, ms);
            }
        }
        transport->set_force_base64(false);
    }

    step("benchmark: page -> native uploads, JSON-base64 vs shared buffer");
    {
        transport->on_upload("bench_up", [](const boson::transfer::BinaryUpload& up) {
            std::uint64_t s = 0;
            for (std::size_t i = 0; i < up.data.size(); i += 4096) s += static_cast<std::uint64_t>(up.data[i]);
            return R"({"len":)" + std::to_string(up.data.size()) + R"(,"s":)" + std::to_string(s) + "}";
        });
        struct Case {
            std::size_t bytes;
            int iterations;
        };
        for (const Case c : {Case{1024 * 1024, 20}, Case{64ULL * 1024 * 1024, 3}}) {
            for (const bool b64 : {true, false}) {
                transport->set_force_base64(b64);
                const std::string js = "window.__bench=null;(async()=>{const d=window.__mk(" + std::to_string(c.bytes) +
                                       ");await window.__bosonTransfer.upload('bench_up',d);const t0=performance.now();for(let i=0;i<" +
                                       std::to_string(c.iterations) + ";i++){const r=await window.__bosonTransfer.upload('bench_up',d);if(r.len!==d.length)throw new Error('len');}"
                                       "window.__bench=performance.now()-t0;})().catch(e=>{window.__bench=-1;console.error(e);});true";
                TT_CHECK(eval_true(wv, js));
                TT_CHECK(wait_js(wv, "window.__bench !== null", std::chrono::seconds{240}));
                const auto ms_text = eval(wv, "window.__bench");
                const double ms = ms_text.has_value() ? std::atof(ms_text->c_str()) : -1.0;
                TT_CHECK(ms > 0.0);
                print_bench(std::string(b64 ? "page->native base64 " : "page->native shared ") + (c.bytes > 2 * 1024 * 1024 ? "64 MiB" : "1 MiB"),
                            c.bytes, c.iterations, ms);
            }
        }
        transport->set_force_base64(false);
        const auto s = transport->stats();
        std::cout << "  pool: created " << s.buffers_created << ", reused " << s.buffers_reused << ", closed " << s.buffers_closed << ", idle "
                  << s.idle_buffers << " (" << s.idle_bytes / (1024 * 1024) << " MiB)\n";
        TT_CHECK(s.buffers_reused > s.buffers_created);
    }

    // ---- teardown ----------------------------------------------------------------------------------
    wv.remove_web_message_observer(bench_hook);
    bool shut_down = false;
    streams.reset();
    file_drop.reset();
    transport.reset();
    bridge.reset();
    env->shutdown([&]() { shut_down = true; });
    TT_CHECK(pump_until([&]() { return shut_down; }, std::chrono::seconds{15}));
    host.reset();
    runner.reset();
    marshal.drain_on_ui_thread();
    window.reset();
    std::error_code ec;
    std::filesystem::remove_all(user_data, ec);
    std::filesystem::remove_all(media_dir, ec);
    return 0;
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    test_base64();
    test_buckets();
    test_ranges();
    test_paths();
    {
        boson::exec::Scheduler scheduler(2);
        boson::exec::UiMarshal marshal([]() {});
        boson::exec::TaskRunner runner(scheduler, marshal);
        test_stream_server_headless(runner, marshal);
    }
    test_file_drop_headless();
    const int webview_rc = run_webview_tests();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " transfer checks passed\n";
    CoUninitialize();
    return g_failures == 0 && webview_rc == 0 ? 0 : 1;
}
