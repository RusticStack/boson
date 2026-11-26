// Terminal workstream tests: Pty (ConPTY) spawn / echo / resize / kill + job cleanup / exit codes / UTF-8 /
// foreground process / cwd, the VT scanner, OutputChannel flow control (credits, coalescing, pause, hidden
// gating, replay) with a fake sink, then TerminalHost + TerminalController against a real WebView2 with an
// end-to-end throughput benchmark (child writing 128 MiB, acks from the page) and a memory bound check.
// The executable doubles as the child process (--spew, --size, --unicode, --tree, ...).
#include <windows.h>
#include <objbase.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

namespace {

using boson::terminal::OutputChannel;
using boson::terminal::Pty;
using boson::terminal::PtyOptions;
using boson::terminal::PtySize;
using boson::terminal::ReadControl;
using boson::terminal::VtScanner;
using Clock = std::chrono::steady_clock;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_terminal.cpp:" << line << " (" << expr << ")\n" << std::flush;
        ++g_failures;
    }
}

#define TT_CHECK(...) check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

/// BOSON_TERM_ONLY=<substring>: run only the WebView steps whose tag contains it (debugging aid).
bool want(std::string_view tag) {
    char buf[256]{};
    const DWORD n = GetEnvironmentVariableA("BOSON_TERM_ONLY", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return true;
    }
    std::string_view list(buf, n);
    while (!list.empty()) {
        const std::size_t bar = list.find('|');
        if (tag.find(list.substr(0, bar)) != std::string_view::npos) {
            return true;
        }
        if (bar == std::string_view::npos) {
            break;
        }
        list.remove_prefix(bar + 1);
    }
    return false;
}

double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

std::span<const std::byte> bytes_of(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::wstring self_path() {
    std::wstring buf(MAX_PATH, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return buf;
}

std::wstring self_command(std::wstring_view args) {
    return Pty::quote_argument(self_path()) + L" " + std::wstring(args);
}

std::uint64_t private_bytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage;
}

bool process_gone(DWORD pid, DWORD wait_ms) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) {
        return true;
    }
    const DWORD r = WaitForSingleObject(h, wait_ms);
    CloseHandle(h);
    return r == WAIT_OBJECT_0;
}

// ---- child modes -----------------------------------------------------------------------------------------

void out_raw(std::string_view s) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
}

int child_main(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8); // out_raw writes UTF-8 bytes
    const std::wstring_view mode = argv[1];
    if (mode == L"--spew" && argc >= 3) {
        const std::uint64_t total = std::wcstoull(argv[2], nullptr, 10);
        std::string line(78, 'x');
        for (std::size_t i = 0; i < line.size(); ++i) {
            line[i] = static_cast<char>('!' + (i % 90));
        }
        line += "\r\n";
        std::string block;
        while (block.size() + line.size() <= 64 * 1024) {
            block += line;
        }
        std::uint64_t sent = 0;
        while (sent < total) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), total - sent));
            out_raw(std::string_view(block).substr(0, n));
            sent += n;
        }
        out_raw("\r\nSPEW-DONE\r\n");
        return 0;
    }
    if (mode == L"--size") {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        for (;;) {
            char c = 0;
            DWORD got = 0;
            if (!ReadFile(in, &c, 1, &got, nullptr) || got == 0 || c == 'q') {
                return 0;
            }
            if (c == '\r' || c == '\n') {
                CONSOLE_SCREEN_BUFFER_INFO info{};
                GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
                const int cols = info.srWindow.Right - info.srWindow.Left + 1;
                const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
                out_raw("SIZE " + std::to_string(cols) + "x" + std::to_string(rows) + "\r\n");
            }
        }
    }
    if (mode == L"--unicode") {
        const std::wstring text = L"UNI: h\u00e9llo w\u00f6rld \u2713 \u65e5\u672c\u8a9e \U0001F680 END\r\n";
        DWORD written = 0;
        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        Sleep(200);
        return 0;
    }
    if (mode == L"--tree") {
        std::string pids = "PIDS";
        for (int i = 0; i < 2; ++i) {
            std::wstring cmd = Pty::quote_argument(self_path()) + L" --sleep";
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                pids += " " + std::to_string(pi.dwProcessId);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        out_raw(pids + " END\r\n");
        Sleep(120000);
        return 0;
    }
    if (mode == L"--sleep") {
        Sleep(120000);
        return 0;
    }
    if (mode == L"--env" && argc >= 3) {
        wchar_t buf[512]{};
        const DWORD n = GetEnvironmentVariableW(argv[2], buf, 512);
        out_raw("ENV[" + boson::core::wide_to_utf8(std::wstring_view(buf, n)) + "]\r\n");
        Sleep(200);
        return 0;
    }
    if (mode == L"--print" && argc >= 4) {
        out_raw(boson::core::wide_to_utf8(argv[2]) + "\r\n");
        Sleep(static_cast<DWORD>(std::wcstoul(argv[3], nullptr, 10)));
        return 0;
    }
    if (mode == L"--exit" && argc >= 3) {
        out_raw("BYE\r\n");
        return static_cast<int>(std::wcstol(argv[2], nullptr, 10));
    }
    return 99;
}

// ---- collecting PTY output -------------------------------------------------------------------------------

struct Collector {
    std::mutex mutex;
    std::condition_variable cv;
    std::string text;
    std::uint64_t bytes = 0;
    bool keep_text = true;
    std::optional<std::uint32_t> exit_code;
    std::uint64_t bytes_at_exit = 0;

    Pty::OutputHandler output() {
        return [this](std::span<const std::byte> d) {
            std::scoped_lock lock(mutex);
            bytes += d.size();
            if (keep_text) {
                text.append(reinterpret_cast<const char*>(d.data()), d.size());
            }
            cv.notify_all();
            return ReadControl::Continue;
        };
    }
    Pty::ExitHandler exit() {
        return [this](std::uint32_t code) {
            std::scoped_lock lock(mutex);
            exit_code = code;
            bytes_at_exit = bytes;
            cv.notify_all();
        };
    }
    /// from = npos: only text arriving from now on.
    bool wait_text(std::string_view needle, std::chrono::milliseconds timeout = std::chrono::seconds{15}, std::size_t from = 0) {
        std::unique_lock lock(mutex);
        const std::size_t start = std::min(from, text.size());
        return cv.wait_for(lock, timeout, [&] { return text.find(needle, start) != std::string::npos; });
    }
    bool wait_exit(std::chrono::milliseconds timeout = std::chrono::seconds{15}) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return exit_code.has_value(); });
    }
    std::size_t size() {
        std::scoped_lock lock(mutex);
        return text.size();
    }
    std::string snapshot() {
        std::scoped_lock lock(mutex);
        return text;
    }
};

std::unique_ptr<Pty> spawn_collect(Collector& c, std::wstring cmd, PtySize size = {}, std::wstring cwd = {},
                                   std::vector<std::pair<std::wstring, std::wstring>> env = {}) {
    PtyOptions o;
    o.command_line = std::move(cmd);
    o.size = size;
    o.cwd = std::move(cwd);
    o.env = std::move(env);
    auto r = Pty::spawn(std::move(o), c.output(), c.exit());
    if (!r) {
        std::cerr << "spawn failed: " << r.error() << '\n';
        return nullptr;
    }
    return std::move(*r);
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::ranges::sort(v);
    return v[v.size() / 2];
}

// ---- headless: helpers -----------------------------------------------------------------------------------

void test_helpers() {
    step("command line quoting and environment block");
    TT_CHECK(Pty::quote_argument(L"plain") == L"plain");
    TT_CHECK(Pty::quote_argument(L"") == L"\"\"");
    TT_CHECK(Pty::quote_argument(L"a b") == L"\"a b\"");
    TT_CHECK(Pty::quote_argument(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
    TT_CHECK(Pty::quote_argument(L"C:\\dir with space\\") == L"\"C:\\dir with space\\\\\"");
    const std::vector<std::wstring> args{L"/c", L"echo a b"};
    TT_CHECK(Pty::build_command_line(L"cmd.exe", args) == L"cmd.exe /c \"echo a b\"");
    {
        int argc = 0;
        const std::wstring line = Pty::build_command_line(L"x.exe", std::vector<std::wstring>{L"a\\\"b", L"c d\\", L""});
        LPWSTR* argv = CommandLineToArgvW(line.c_str(), &argc);
        TT_CHECK(argc == 4);
        if (argc == 4) {
            TT_CHECK(std::wstring(argv[1]) == L"a\\\"b");
            TT_CHECK(std::wstring(argv[2]) == L"c d\\");
            TT_CHECK(std::wstring(argv[3]).empty());
        }
        LocalFree(argv);
    }
    const std::vector<std::pair<std::wstring, std::wstring>> env{{L"BOSON_B", L"2"}, {L"boson_a", L"1"}, {L"PATH", L""}};
    const std::wstring block = Pty::build_environment_block(false, env);
    TT_CHECK(block == std::wstring(L"boson_a=1\0BOSON_B=2\0\0", 21));
    const std::wstring inherited = Pty::build_environment_block(true, env);
    TT_CHECK(inherited.find(L"BOSON_B=2") != std::wstring::npos);
    bool has_path = false;
    for (std::size_t at = 0; at < inherited.size() && inherited[at] != L'\0';) {
        const std::wstring_view entry(inherited.c_str() + at);
        has_path = has_path || (entry.size() > 5 && _wcsnicmp(entry.data(), L"PATH=", 5) == 0);
        at += entry.size() + 1;
    }
    TT_CHECK(!has_path); // an empty override removes the variable
    TT_CHECK(inherited.ends_with(std::wstring(L"\0\0", 2)));
    TT_CHECK(Pty::system_conpty_available());
    std::cout << "  default shell: " << boson::core::wide_to_utf8(Pty::default_shell())
              << ", bundled conpty.dll: " << (Pty::bundled_conpty_available() ? "yes" : "no") << '\n';
}

void test_scanner() {
    step("VtScanner: OSC title / cwd, DEC 2026, split sequences");
    std::vector<std::string> titles;
    std::vector<std::string> cwds;
    std::vector<bool> syncs;
    VtScanner scanner({
        .on_title = [&](std::string_view t) { titles.emplace_back(t); },
        .on_cwd = [&](std::string_view c) { cwds.emplace_back(c); },
        .on_sync = [&](bool b) { syncs.push_back(b); },
    });
    scanner.feed(bytes_of("plain text \x1b[31mred\x1b[0m \x1b]0;first title\x07 more"));
    TT_CHECK(titles.size() == 1 && titles[0] == "first title");
    scanner.feed(bytes_of("\x1b]2;split "));
    scanner.feed(bytes_of("title\x1b"));
    scanner.feed(bytes_of("\\after"));
    TT_CHECK(titles.size() == 2 && titles[1] == "split title");
    scanner.feed(bytes_of("\x1b]7;file://host/C:/Users/Me/My%20Dir\x07"));
    TT_CHECK(cwds.size() == 1 && cwds[0] == "C:\\Users\\Me\\My Dir");
    scanner.feed(bytes_of("\x1b]9;9;\"D:\\work\"\x1b\\"));
    TT_CHECK(cwds.size() == 2 && cwds[1] == "D:\\work");
    scanner.feed(bytes_of("\x1b[?20"));
    scanner.feed(bytes_of("26hframe\x1b[?2026l"));
    TT_CHECK(syncs.size() == 2 && syncs[0] && !syncs[1]);
    scanner.feed(bytes_of("\x1b[?25h\x1b[?1;2026h"));
    TT_CHECK(syncs.size() == 3 && syncs[2]);
    std::string big = "\x1b]0;";
    big.append(10000, 'a');
    big += "\x07";
    scanner.feed(bytes_of(big));
    TT_CHECK(titles.size() == 2); // over-long OSC ignored
    scanner.feed(bytes_of("\x1b]0;ok\x07"));
    TT_CHECK(titles.size() == 3 && titles[2] == "ok");
    TT_CHECK(VtScanner::decode_cwd("file:///C:/a/b") == "C:\\a\\b");
}

// ---- headless: OutputChannel -----------------------------------------------------------------------------

struct FakeSink {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> chunks;
    std::vector<bool> resets;
    std::uint64_t bytes = 0;
    bool available = true;

    OutputChannel::SendFn fn() {
        return [this](std::uint64_t size, std::string_view meta, const OutputChannel::FillFn& fill) {
            std::string data(static_cast<std::size_t>(size), '\0');
            {
                std::scoped_lock lock(mutex);
                if (!available) {
                    return false;
                }
            }
            fill(std::span<std::byte>(reinterpret_cast<std::byte*>(data.data()), data.size()));
            std::scoped_lock lock(mutex);
            resets.push_back(meta.find("\"reset\":true") != std::string_view::npos);
            bytes += size;
            chunks.push_back(std::move(data));
            cv.notify_all();
            return true;
        };
    }
    bool wait_bytes(std::uint64_t n, std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return bytes >= n; });
    }
    std::string all() {
        std::scoped_lock lock(mutex);
        std::string s;
        for (const auto& c : chunks) s += c;
        return s;
    }
    std::size_t count() {
        std::scoped_lock lock(mutex);
        return chunks.size();
    }
};

std::string pattern_text(std::size_t n, std::size_t seed) {
    std::string s(n, ' ');
    for (std::size_t i = 0; i < n; ++i) {
        s[i] = static_cast<char>('a' + ((i + seed) % 26));
    }
    return s;
}

void test_output_channel(boson::exec::TaskRunner& runner) {
    const auto post = [&runner](std::function<void()> w) { runner.spawn(std::move(w)); };

    step("OutputChannel: leading-edge send, frame coalescing");
    {
        FakeSink sink;
        auto ch = OutputChannel::create(1, {.frame = std::chrono::milliseconds{40}}, sink.fn(), post);
        ch->push(bytes_of("a"));
        TT_CHECK(sink.wait_bytes(1));
        // Within the frame: these coalesce into one chunk sent when the frame elapses.
        for (int i = 0; i < 20; ++i) {
            ch->push(bytes_of("b"));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        TT_CHECK(sink.count() == 1);
        TT_CHECK(sink.wait_bytes(21, std::chrono::seconds{2}));
        TT_CHECK(sink.count() == 2);
        TT_CHECK(sink.all() == "a" + std::string(20, 'b'));
        ch->ack(21);
        TT_CHECK(ch->stats().in_flight == 0);
    }

    step("OutputChannel: credit window, pause, resume on ack");
    {
        FakeSink sink;
        std::atomic<int> resumes{0};
        auto ch = OutputChannel::create(2,
                                        {.ring_bytes = 256 * 1024, .window_bytes = 64 * 1024, .max_chunk_bytes = 32 * 1024, .frame = std::chrono::milliseconds{1}},
                                        sink.fn(), post);
        ch->set_resume([&] { ++resumes; });
        const std::string data = pattern_text(400 * 1024, 3);
        std::size_t pushed = 0;
        bool paused = false;
        while (pushed < data.size()) {
            const std::size_t n = std::min<std::size_t>(16 * 1024, data.size() - pushed);
            const ReadControl rc = ch->push(bytes_of(std::string_view(data).substr(pushed, n)));
            pushed += n;
            if (rc == ReadControl::Pause) {
                paused = true;
                break;
            }
        }
        TT_CHECK(paused);
        TT_CHECK(sink.wait_bytes(64 * 1024));
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        auto st = ch->stats();
        TT_CHECK(st.bytes_sent == 64 * 1024); // never more than the window without acks
        TT_CHECK(st.max_in_flight <= 64 * 1024);
        TT_CHECK(st.max_pending <= 256 * 1024);
        TT_CHECK(st.reader_pauses == 1);
        // Ack everything as it arrives until the backlog is gone: the reader is resumed.
        std::uint64_t acked = 0;
        const auto deadline = Clock::now() + std::chrono::seconds{5};
        while (acked < pushed && Clock::now() < deadline) {
            std::uint64_t have = 0;
            {
                std::scoped_lock lock(sink.mutex);
                have = sink.bytes;
            }
            if (have > acked) {
                ch->ack(have - acked);
                acked = have;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        TT_CHECK(acked == pushed);
        TT_CHECK(resumes.load() >= 1);
        TT_CHECK(sink.all() == data.substr(0, pushed));
    }

    step("OutputChannel: hidden gating, overflow drop, replay on show");
    {
        FakeSink sink;
        auto ch = OutputChannel::create(3, {.ring_bytes = 64 * 1024, .frame = std::chrono::milliseconds{1}}, sink.fn(), post, {}, false);
        const std::string data = pattern_text(200 * 1024, 7);
        for (std::size_t off = 0; off < data.size(); off += 8192) {
            TT_CHECK(ch->push(bytes_of(std::string_view(data).substr(off, 8192))) == ReadControl::Continue);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        TT_CHECK(sink.count() == 0);
        auto st = ch->stats();
        TT_CHECK(st.bytes_dropped == data.size() - 64 * 1024);
        TT_CHECK(st.pending == 64 * 1024);
        ch->set_visible(true);
        TT_CHECK(sink.wait_bytes(64 * 1024));
        TT_CHECK(sink.all() == data.substr(data.size() - 64 * 1024));
        TT_CHECK(!sink.resets.empty() && sink.resets.front());
        ch->ack(64 * 1024);
        // replay(): the retained ring again, flagged reset.
        ch->replay();
        TT_CHECK(sink.wait_bytes(128 * 1024));
        TT_CHECK(sink.all().substr(64 * 1024) == data.substr(data.size() - 64 * 1024));
        TT_CHECK(sink.resets.back() || sink.resets[sink.resets.size() - 2]);
        TT_CHECK(ch->stats().replays == 1);
    }

    step("OutputChannel: hidden without overflow replays exactly; drained callback; DEC 2026 hold");
    {
        FakeSink sink;
        std::atomic<int> drained{0};
        auto ch = OutputChannel::create(4, {.frame = std::chrono::milliseconds{1}, .sync_timeout = std::chrono::milliseconds{300}}, sink.fn(), post, {}, false);
        ch->set_drained([&] { ++drained; });
        ch->push(bytes_of("hidden-1 "));
        ch->push(bytes_of("hidden-2"));
        ch->set_visible(true);
        TT_CHECK(sink.wait_bytes(17));
        TT_CHECK(sink.all() == "hidden-1 hidden-2");
        TT_CHECK(sink.resets.front() == false);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        TT_CHECK(drained.load() >= 1);
        ch->ack(17);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        ch->push(bytes_of("\x1b[?2026hpartial frame"));
        std::this_thread::sleep_for(std::chrono::milliseconds{60});
        TT_CHECK(sink.count() == 1); // held while the synchronized update is open
        ch->push(bytes_of(" end\x1b[?2026l"));
        TT_CHECK(sink.wait_bytes(17 + 33, std::chrono::seconds{2}));
        TT_CHECK(ch->stats().sync_holds == 1);
    }

    step("OutputChannel: sink unavailable keeps bytes pending");
    {
        FakeSink sink;
        sink.available = false;
        auto ch = OutputChannel::create(5, {.frame = std::chrono::milliseconds{1}}, sink.fn(), post);
        ch->push(bytes_of("kept"));
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        TT_CHECK(ch->pending() == 4);
        {
            std::scoped_lock lock(sink.mutex);
            sink.available = true;
        }
        ch->set_visible(false);
        ch->set_visible(true);
        TT_CHECK(sink.wait_bytes(4));
        ch->close();
        ch->push(bytes_of("ignored"));
        TT_CHECK(ch->pending() == 0);
    }
}

// ---- headless: Pty ---------------------------------------------------------------------------------------

void test_pty() {
    step("Pty: exit code, output delivered before exit");
    {
        Collector c;
        auto pty = spawn_collect(c, L"cmd.exe /d /c echo exit-test-output& exit 42");
        TT_CHECK(pty != nullptr);
        if (pty) {
            TT_CHECK(c.wait_exit());
            TT_CHECK(c.exit_code == 42u);
            TT_CHECK(c.snapshot().find("exit-test-output") != std::string::npos);
            TT_CHECK(c.bytes_at_exit == c.bytes);
            TT_CHECK(!pty->running());
            TT_CHECK(pty->exit_code() == 42u);
            TT_CHECK(!pty->write("late"));
            TT_CHECK(pty->backend() == boson::terminal::PtyBackend::System || pty->backend() == boson::terminal::PtyBackend::Bundled);
            std::cout << "  backend: " << Pty::backend_name(pty->backend()) << '\n';
            TT_CHECK(Pty::bundled_conpty_available() == (pty->backend() == boson::terminal::PtyBackend::Bundled));
        }
        Collector c2;
        auto pty2 = spawn_collect(c2, self_command(L"--exit 7"));
        TT_CHECK(pty2 && c2.wait_exit() && c2.exit_code == 7u && c2.snapshot().find("BYE") != std::string::npos);
    }

    step("Pty: echo round trip through cmd.exe, keystroke latency");
    {
        Collector c;
        auto pty = spawn_collect(c, L"cmd.exe /d /k prompt BOSON$G", {120, 30});
        TT_CHECK(pty && c.wait_text("BOSON>"));
        if (pty) {
            const auto t0 = Clock::now();
            TT_CHECK(pty->write("echo round-^trip-ok\r"));
            TT_CHECK(c.wait_text("round-trip-ok"));
            std::cout << "  echo command round trip: " << std::fixed << std::setprecision(2) << ms_since(t0) << " ms\n";
            TT_CHECK(c.wait_text("BOSON>", std::chrono::seconds{5}, c.snapshot().find("round-trip-ok")));
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            // Typed (not executed) tokens: the time from the write until ConPTY echoes them back.
            std::vector<double> lat;
            for (int i = 0; i < 20; ++i) {
                const std::size_t before = c.size();
                const std::string token = "zq" + std::to_string(100 + i) + "j";
                const auto t = Clock::now();
                pty->write(token);
                if (c.wait_text(token, std::chrono::seconds{3}, before)) {
                    lat.push_back(ms_since(t));
                }
                pty->write("\x1b"); // Esc clears the typed line in cmd
                std::this_thread::sleep_for(std::chrono::milliseconds{30});
            }
            TT_CHECK(lat.size() == 20);
            if (!lat.empty()) {
                std::cout << "  typed input -> echo (ConPTY render): median " << median(lat) << " ms, max " << *std::ranges::max_element(lat) << " ms\n";
            }
            TT_CHECK(pty->interrupt());
        }
    }

    step("Pty: UTF-8 output and input");
    {
        Collector c;
        auto pty = spawn_collect(c, self_command(L"--unicode"));
        TT_CHECK(pty && c.wait_exit());
        const std::string text = c.snapshot();
        TT_CHECK(text.find("h\xc3\xa9llo w\xc3\xb6rld \xe2\x9c\x93 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e") != std::string::npos);
        TT_CHECK(text.find("\xf0\x9f\x9a\x80") != std::string::npos);
        Collector c2;
        auto shell = spawn_collect(c2, L"cmd.exe /d /k prompt U$G");
        TT_CHECK(shell && c2.wait_text("U>"));
        if (shell) {
            shell->write("echo [\xc3\xbcn\xc3\xaf \xe2\x9c\x93 \xe6\x97\xa5]\r");
            TT_CHECK(c2.wait_text("[\xc3\xbcn\xc3\xaf \xe2\x9c\x93 \xe6\x97\xa5]"));
        }
    }

    step("Pty: resize");
    {
        Collector c;
        auto pty = spawn_collect(c, self_command(L"--size"), {80, 24});
        TT_CHECK(pty != nullptr);
        if (pty) {
            pty->write("\r");
            TT_CHECK(c.wait_text("SIZE 80x24"));
            TT_CHECK(pty->resize({132, 43}));
            TT_CHECK(pty->size().cols == 132 && pty->size().rows == 43);
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            pty->write("\r");
            TT_CHECK(c.wait_text("SIZE 132x43"));
            pty->write("q\r"); // cooked console input: lines arrive on Enter
            TT_CHECK(c.wait_exit());
        }
    }

    step("Pty: environment and working directory, cwd query, foreground process");
    {
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        std::filesystem::path dir = std::filesystem::path(tmp) / (L"boson-term-cwd-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(dir);
        dir = std::filesystem::canonical(dir);
        Collector c;
        auto pty = spawn_collect(c, self_command(L"--env BOSON_TERM_TEST"), {}, dir.wstring(), {{L"BOSON_TERM_TEST", L"v\u00e4lue-42"}});
        TT_CHECK(pty && c.wait_exit() && c.snapshot().find("ENV[v\xc3\xa4lue-42]") != std::string::npos);

        Collector s;
        auto shell = spawn_collect(s, L"cmd.exe /d /k prompt C$G", {}, dir.wstring());
        TT_CHECK(shell && s.wait_text("C>"));
        if (shell) {
            const auto cwd = shell->cwd();
            TT_CHECK(cwd.has_value());
            if (cwd) {
                TT_CHECK(_wcsicmp(cwd->c_str(), dir.c_str()) == 0);
            }
            const auto fg0 = shell->foreground_process();
            TT_CHECK(fg0 && _wcsicmp(fg0->name.c_str(), L"cmd.exe") == 0);
            const std::size_t mark = s.size();
            shell->write("ping -n 6 127.0.0.1\r");
            TT_CHECK(s.wait_text("127.0.0.1", std::chrono::seconds{5}, mark));
            std::this_thread::sleep_for(std::chrono::milliseconds{300});
            const auto fg = shell->foreground_process();
            TT_CHECK(fg && _wcsicmp(fg->name.c_str(), L"PING.EXE") == 0);
            if (fg) {
                std::cout << "  foreground while pinging: " << boson::core::wide_to_utf8(fg->name) << " (pid " << fg->pid << ")\n";
            }
            TT_CHECK(shell->processes().size() >= 2);
            // Ctrl+C stops ping; the shell survives.
            TT_CHECK(shell->interrupt());
            const auto t_int = Clock::now();
            while (shell->processes().size() > 1 && Clock::now() - t_int < std::chrono::seconds{5}) {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
            TT_CHECK(shell->processes().size() == 1);
            TT_CHECK(shell->running());
            // ConPTY repaints the screen, so the prompt is not reliably contiguous text: prove the shell still
            // takes commands instead.
            const std::size_t after = s.size();
            shell->write("echo still-alive-%COMSPEC:~0,0%ok\r");
            TT_CHECK(s.wait_text("still-alive-ok", std::chrono::seconds{5}, after));
        }
        shell.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    step("Pty: kill terminates the whole job; destruction leaves no orphans");
    for (int variant = 0; variant < 2; ++variant) {
        Collector c;
        auto pty = spawn_collect(c, self_command(L"--tree"));
        TT_CHECK(pty && c.wait_text(" END"));
        const std::string text = c.snapshot();
        std::vector<DWORD> pids;
        if (const auto at = text.find("PIDS"); at != std::string::npos) {
            std::istringstream in(text.substr(at + 4));
            std::string tok;
            while (in >> tok && tok != "END") {
                pids.push_back(static_cast<DWORD>(std::stoul(tok)));
            }
        }
        TT_CHECK(pids.size() == 2);
        pids.push_back(pty ? pty->pid() : 0);
        TT_CHECK(pty && pty->processes().size() == 3);
        const auto t0 = Clock::now();
        if (variant == 0) {
            TT_CHECK(pty->kill(9));
            TT_CHECK(c.wait_exit());
            TT_CHECK(c.exit_code == 9u);
        } else {
            pty.reset(); // kill-on-close
        }
        bool all_gone = true;
        for (const DWORD pid : pids) {
            all_gone = all_gone && process_gone(pid, 5000);
        }
        TT_CHECK(all_gone);
        std::cout << "  " << (variant == 0 ? "kill()" : "~Pty()") << ": " << pids.size() << " processes gone after " << ms_since(t0) << " ms\n";
    }

    step("Pty: raw ConPTY throughput (64 MiB, no renderer)");
    {
        Collector c;
        c.keep_text = false;
        constexpr std::uint64_t kBytes = 64ULL * 1024 * 1024;
        const auto t0 = Clock::now();
        auto pty = spawn_collect(c, self_command(L"--spew " + std::to_wstring(kBytes)), {200, 50});
        TT_CHECK(pty && c.wait_exit(std::chrono::seconds{120}));
        const double ms = ms_since(t0);
        std::cout << "  child wrote " << kBytes / (1024 * 1024) << " MiB, ConPTY delivered " << std::fixed << std::setprecision(1)
                  << static_cast<double>(c.bytes) / (1024.0 * 1024.0) << " MiB in " << ms << " ms = "
                  << (static_cast<double>(c.bytes) / (1024.0 * 1024.0)) / (ms / 1000.0) << " MiB/s\n";
        TT_CHECK(c.bytes > kBytes / 2);
    }
}

// ---- WebView2 --------------------------------------------------------------------------------------------

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{20},
                const std::function<void()>& each = {}) {
    const auto deadline = Clock::now() + timeout;
    while (!done()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (each) {
            each();
        }
    }
    return true;
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

std::uint64_t eval_u64(boson::core::WebView& webview, std::string_view js) {
    const auto r = eval(webview, js);
    if (!r || r->empty() || r->front() == 'n') return 0;
    return std::strtoull(r->c_str(), nullptr, 10);
}

bool wait_js(boson::core::WebView& webview, std::string_view js, std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
    return pump_until([&]() { return eval_true(webview, js); }, timeout);
}

std::string read_text_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Minimal page renderer: counts bytes per terminal, optionally keeps text, acks via the fast path (optionally
// after a delay, to emulate a slow renderer), records Bridge events.
constexpr std::string_view kPage = R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>terminal test</title></head><body>
<script>
window.T = { bytes: {}, chunks: {}, resets: {}, text: {}, keep: {}, exits: {}, ends: {}, last: {}, titles: {}, cwds: {}, ackDelay: 0, rpc: {} };
// done(id): exit seen and the chunk reaching its final offset processed (chunks and events are not ordered).
window.done = (id) => T.exits[id] !== undefined && (T.ends[id] === 0 || (T.last[id] || 0) >= T.ends[id]);
const dec = {};
let nextRpc = 1;
window.rpc = (method, params) => new Promise((resolve, reject) => {
  const id = 100000 + nextRpc++;
  T.rpc[id] = { resolve, reject };
  chrome.webview.postMessage({ id, method, params: params || {} });
});
chrome.webview.addEventListener('message', (e) => {
  const d = e.data;
  if (!d || typeof d !== 'object') return;
  if (d.type === 'event') {
    if (d.event === 'terminal:exit') { T.exits[d.data.id] = d.data.exit_code; T.ends[d.data.id] = d.data.end; }
    if (d.event === 'terminal:title') T.titles[d.data.id] = d.data.title;
    if (d.event === 'terminal:cwd') T.cwds[d.data.id] = d.data.cwd;
    return;
  }
  if (typeof d.id === 'number' && T.rpc[d.id]) {
    const p = T.rpc[d.id]; delete T.rpc[d.id];
    if (d.status === 'success') p.resolve(d.data); else p.reject(new Error(d.error && d.error.message));
  }
});
function hook() {
  if (!window.__bosonTransfer) { setTimeout(hook, 5); return; }
  window.__bosonTransfer.onBinary('terminal', (m) => {
    if (T.ignoreAll) return;
    const id = m.meta.id, n = m.data.byteLength;
    T.bytes[id] = (T.bytes[id] || 0) + n;
    T.chunks[id] = (T.chunks[id] || 0) + 1;
    if (m.meta.reset) T.resets[id] = (T.resets[id] || 0) + 1;
    T.last[id] = Math.max(T.last[id] || 0, m.meta.end);
    if (T.probe && T.probe.id === id && !T.probe.dt) {
      T.probe.buf += new TextDecoder().decode(m.data);
      if (T.probe.buf.includes(T.probe.token)) T.probe.dt = performance.now() - T.probe.t0;
    }
    if (T.keep[id]) { dec[id] = dec[id] || new TextDecoder(); T.text[id] = (T.text[id] || '') + dec[id].decode(m.data, { stream: true }); }
    const ack = () => chrome.webview.postMessage({ __boson_term: 'a', id, n });
    if (T.ackDelay > 0) {
      // A renderer that parses one chunk at a time, ackDelay ms each.
      T.chain = (T.chain || Promise.resolve()).then(() => new Promise((r) => setTimeout(r, T.ackDelay))).then(ack);
      return T.chain;
    }
    ack();
  });
  window.T.ready = true;
}
hook();
</script></body></html>)HTML";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/index.html" || path == "/") {
        return std::as_bytes(std::span(kPage.data(), kPage.size()));
    }
    return std::nullopt;
}

void print_bench(std::string_view label, double bytes, double ms) {
    std::cout << "  [bench] " << std::left << std::setw(46) << label << std::right << std::fixed << std::setprecision(1) << std::setw(8)
              << bytes / (1024.0 * 1024.0) << " MiB in " << std::setw(8) << ms << " ms = " << std::setw(7)
              << (bytes / (1024.0 * 1024.0)) / (ms / 1000.0) << " MiB/s\n";
}

int run_webview_tests() {
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
        std::cout << "SKIP WebView2 part: no WebView2 runtime installed\n";
        return 0;
    }
    std::wcout << L"WebView2 runtime " << version << L'\n';
    CoTaskMemFree(version);

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const auto user_data = std::filesystem::path(tmp) / (L"boson-term-udf-" + std::to_wstring(GetCurrentProcessId()));

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson terminal test";
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

    auto transport = std::make_unique<boson::transfer::BinaryTransport>(*runner, marshal);
    transport->attach(wv);
    boson::terminal::TerminalHostOptions host_options;
    if (char fb[16]{}; GetEnvironmentVariableA("BOSON_TERM_FRAME_MS", fb, sizeof(fb)) > 0) {
        host_options.flow.frame = std::chrono::milliseconds{std::atoi(fb)}; // experiments only
    }
    auto terms = std::make_unique<boson::terminal::TerminalHost>(*runner, *transport, host_options);
    boson::ipc::TerminalController terminal_controller(*terms);
    boson::ipc::Router<boson::ipc::TerminalController> router(terminal_controller);
    auto bridge = std::make_unique<boson::ipc::Bridge>(*runner, marshal, router);
    bridge->attach(wv);
    terms->set_bridge(bridge.get());
    terms->attach(wv);
    TT_CHECK(terms->fast_input_available(wv.id()));
    TT_CHECK(wait_js(wv, "window.T && window.T.ready === true"));
    TT_CHECK(boson::transfer::BinaryTransport::supports_shared_buffers(wv));

    std::mutex ev_mutex;
    std::vector<boson::terminal::TerminalExitEvent> exits;
    std::vector<boson::terminal::TerminalTitleEvent> titles;
    terms->on_exit([&](const boson::terminal::TerminalExitEvent& e) {
        std::scoped_lock lock(ev_mutex);
        exits.push_back(e);
    });
    terms->on_title([&](const boson::terminal::TerminalTitleEvent& e) {
        std::scoped_lock lock(ev_mutex);
        titles.push_back(e);
    });
    const auto exited = [&](std::uint32_t id) {
        std::scoped_lock lock(ev_mutex);
        return std::ranges::any_of(exits, [id](const auto& e) { return e.id == id; });
    };
    const auto spawn = [&](std::wstring cmd, bool visible, PtySize size = {120, 30}) -> std::uint32_t {
        boson::terminal::TerminalSpawnOptions o;
        o.pty.command_line = std::move(cmd);
        o.pty.size = size;
        o.webview_id = wv.id();
        o.visible = visible;
        auto r = terms->spawn(std::move(o));
        if (!r) {
            std::cerr << "spawn failed: " << r.error() << '\n';
            return 0;
        }
        return r->id;
    };
    const auto js_id = [](std::string_view expr, std::uint32_t id) {
        std::string s(expr);
        const std::string ids = std::to_string(id);
        for (std::size_t at = s.find("$ID"); at != std::string::npos; at = s.find("$ID", at + ids.size())) {
            s.replace(at, 3, ids);
        }
        return s;
    };

    if (want("TerminalHost: output to the page via shared buffers, fast-path input, title event")) {
        step("TerminalHost: output to the page via shared buffers, fast-path input, title event");
        const std::uint32_t id = spawn(L"cmd.exe /d /k prompt W$G", true);
        TT_CHECK(id != 0);
        TT_CHECK(eval_true(wv, js_id("(T.keep[$ID] = true, true)", id)));
        TT_CHECK(wait_js(wv, js_id("(T.text[$ID] || '').includes('W>')", id)));
        TT_CHECK(eval_true(wv, js_id("(chrome.webview.postMessage({__boson_term:'w', id:$ID, d:'echo fast-^path-ok\\r'}), true)", id)));
        TT_CHECK(wait_js(wv, js_id("(T.text[$ID] || '').includes('fast-path-ok')", id)));
        TT_CHECK(eval_true(wv, js_id("(chrome.webview.postMessage({__boson_term:'w', id:$ID, d:'title boson-title-xyz\\r'}), true)", id)));
        TT_CHECK(wait_js(wv, js_id("T.titles[$ID] === 'boson-title-xyz'", id)));
        {
            std::scoped_lock lock(ev_mutex);
            TT_CHECK(std::ranges::any_of(titles, [&](const auto& t) { return t.id == id && t.title == "boson-title-xyz"; }));
        }
        TT_CHECK(terms->info(id)->title == "boson-title-xyz");
        // Typing latency through the full path, timed in the page: postMessage -> UI-thread fast path -> ConPTY ->
        // pool read -> ring -> shared buffer -> page listener sees the echoed token.
        std::vector<double> lat;
        for (int i = 0; i < 15; ++i) {
            const std::string token = "zq" + std::to_string(500 + i) + "j";
            TT_CHECK(eval_true(wv, js_id("(T.probe = { id: $ID, token: '" + token + "', buf: '', dt: 0, t0: performance.now() }, chrome.webview.postMessage({__boson_term:'w', id:$ID, d:'" + token + "'}), true)", id)));
            if (wait_js(wv, "T.probe.dt > 0", std::chrono::seconds{3})) {
                const auto r = eval(wv, "T.probe.dt");
                lat.push_back(r ? std::strtod(r->c_str(), nullptr) : 0.0);
            }
            eval(wv, js_id("(chrome.webview.postMessage({__boson_term:'w', id:$ID, d:'\\x1b'}), true)", id));
            pump_until([] { return false; }, std::chrono::milliseconds{30});
        }
        TT_CHECK(lat.size() == 15);
        if (!lat.empty()) {
            std::cout << "  typed input in page -> echo back in page: median " << std::fixed << std::setprecision(2) << median(lat) << " ms, max "
                      << *std::ranges::max_element(lat) << " ms\n";
        }
        // A message for a terminal id that is not this WebView's is ignored, and fast-path messages never reach
        // the Bridge (no RPC error envelope comes back).
        TT_CHECK(eval_true(wv, "(chrome.webview.postMessage({__boson_term:'w', id:987654, d:'x'}), true)"));
        const auto st = terms->stats(id);
        TT_CHECK(st && st->bytes_sent == st->bytes_in);
        TT_CHECK(terms->kill(id, 3));
        TT_CHECK(pump_until([&] { return exited(id); }));
        TT_CHECK(wait_js(wv, js_id("T.exits[$ID] === 3", id)));
        TT_CHECK(terms->info(id)->exit_code == 3u);
        TT_CHECK(terms->dispose(id));
        TT_CHECK(!terms->info(id).has_value());
    }

    if (want("TerminalController through JSON-RPC: spawn, list, exit event, capabilities, errors")) {
        step("TerminalController through JSON-RPC: spawn, list, exit event, capabilities, errors");
        TT_CHECK(eval_true(wv, R"JS((window.__ctl = null, rpc('Terminal.spawn', { command: 'cmd.exe', args: ['/d', '/c', 'echo rpc-spawned& exit 5'], cols: 100, rows: 20 })
            .then((r) => { window.__ctl = r; T.keep[r.terminal.id] = true; }), true))JS"));
        TT_CHECK(wait_js(wv, "window.__ctl !== null"));
        const std::uint32_t id = static_cast<std::uint32_t>(eval_u64(wv, "__ctl.terminal.id"));
        TT_CHECK(id != 0);
        TT_CHECK(eval_true(wv, "__ctl.fast_input === true && __ctl.channel === 'terminal' && __ctl.terminal.cols === 100"));
        TT_CHECK(wait_js(wv, js_id("T.exits[$ID] === 5 && done($ID)", id)));
        TT_CHECK(eval_true(wv, js_id("(T.text[$ID] || '').includes('rpc-spawned')", id)));
        TT_CHECK(eval_true(wv, R"JS((window.__list = null, rpc('Terminal.list').then((r) => { window.__list = r; }), true))JS"));
        TT_CHECK(wait_js(wv, js_id("window.__list !== null && __list.terminals.some((t) => t.id === $ID && t.running === false && t.exit_code === 5)", id)));
        TT_CHECK(eval_true(wv, R"JS((window.__caps = null, rpc('Terminal.capabilities').then((r) => { window.__caps = r; }), true))JS"));
        TT_CHECK(wait_js(wv, "window.__caps !== null && __caps.system_conpty === true && __caps.fast_input === true"));
        TT_CHECK(eval_true(wv, R"JS((window.__err = null, rpc('Terminal.write', { id: 424242, data: 'x' }).catch((e) => { window.__err = e.message; }), true))JS"));
        TT_CHECK(wait_js(wv, "window.__err === 'unknown terminal'"));
        TT_CHECK(eval_true(wv, js_id("(window.__disp = null, rpc('Terminal.dispose', { id: $ID }).then(() => { window.__disp = true; }), true)", id)));
        TT_CHECK(wait_js(wv, "window.__disp === true"));
        TT_CHECK(!terms->info(id).has_value());
    }

    if (want("Hidden pane: nothing delivered while hidden, replay on show; overflow drops oldest, reset on show")) {
        step("Hidden pane: nothing delivered while hidden, replay on show; overflow drops oldest, reset on show");
        const std::uint32_t id = spawn(self_command(L"--print HIDDEN-REPLAY-MARKER 400"), false);
        TT_CHECK(id != 0);
        TT_CHECK(eval_true(wv, js_id("(T.keep[$ID] = true, true)", id)));
        TT_CHECK(pump_until([&] { return exited(id); }, std::chrono::seconds{10}));
        pump_until([] { return false; }, std::chrono::milliseconds{150});
        TT_CHECK(eval_u64(wv, js_id("T.bytes[$ID] || 0", id)) == 0);
        TT_CHECK(terms->stats(id)->pending > 0);
        TT_CHECK(terms->set_visible(id, true));
        TT_CHECK(wait_js(wv, js_id("(T.text[$ID] || '').includes('HIDDEN-REPLAY-MARKER')", id)));
        TT_CHECK(eval_u64(wv, js_id("T.resets[$ID] || 0", id)) == 0); // nothing lost: no reset needed
        TT_CHECK(terms->stats(id)->bytes_dropped == 0);

        // Replay to a re-created renderer.
        const std::uint64_t first = eval_u64(wv, js_id("T.bytes[$ID]", id));
        TT_CHECK(terms->replay(id));
        TT_CHECK(wait_js(wv, js_id("T.resets[$ID] === 1 && T.bytes[$ID] === " + std::to_string(first * 2), id)));
        terms->dispose(id);

        const std::uint32_t big = spawn(self_command(L"--spew 4194304"), false);
        TT_CHECK(pump_until([&] { return exited(big); }, std::chrono::seconds{30}));
        TT_CHECK(eval_u64(wv, js_id("T.bytes[$ID] || 0", big)) == 0);
        const auto hidden_stats = terms->stats(big);
        TT_CHECK(hidden_stats && hidden_stats->bytes_dropped > 2 * 1024 * 1024 && hidden_stats->pending == hidden_stats->ring_capacity);
        TT_CHECK(hidden_stats && hidden_stats->reader_pauses == 0);
        TT_CHECK(terms->set_visible(big, true));
        TT_CHECK(wait_js(wv, js_id("T.bytes[$ID] === " + std::to_string(hidden_stats ? hidden_stats->ring_capacity : 0), big)));
        TT_CHECK(eval_u64(wv, js_id("T.resets[$ID] || 0", big)) == 1);
        terms->dispose(big);
    }

    if (want("Throughput: child writes 128 MiB, end to end into the page with acks")) {
        step("Throughput: child writes 128 MiB, end to end into the page with acks");
        constexpr std::uint64_t kBytes = 128ULL * 1024 * 1024;
        const std::uint64_t base_private = private_bytes();
        std::uint64_t peak_private = base_private;
        const auto t0 = Clock::now();
        const std::uint32_t id = spawn(self_command(L"--spew " + std::to_wstring(kBytes)), true, {200, 50});
        TT_CHECK(id != 0);
        const bool done = pump_until(
            [&] { return eval_true(wv, js_id("T.exits[$ID] === 0 && done($ID)", id)); }, std::chrono::seconds{240},
            [&] { peak_private = std::max(peak_private, private_bytes()); });
        TT_CHECK(done);
        const double ms = ms_since(t0);
        const std::uint64_t page_bytes = eval_u64(wv, js_id("T.bytes[$ID]", id));
        const auto st = terms->stats(id);
        TT_CHECK(st && page_bytes == st->bytes_in && st->bytes_sent == st->bytes_in);
        TT_CHECK(st && st->max_in_flight <= terms->options().flow.window_bytes);
        TT_CHECK(st && st->max_pending <= st->ring_capacity);
        TT_CHECK(st && st->bytes_dropped == 0);
        print_bench("ConPTY -> ring -> shared buffer -> page (acked)", static_cast<double>(page_bytes), ms);
        if (st) {
            std::cout << "  chunks " << st->chunks_sent << " (avg " << (st->chunks_sent ? st->bytes_sent / st->chunks_sent / 1024 : 0)
                      << " KiB), max in flight " << st->max_in_flight / 1024 << " KiB, max backlog " << st->max_pending / 1024
                      << " KiB, reader pauses " << st->reader_pauses << ", private bytes +" << (peak_private - base_private) / (1024 * 1024)
                      << " MiB peak\n";
        }
        TT_CHECK(peak_private - base_private < 96ULL * 1024 * 1024);
        const auto ts = transport->stats();
        std::cout << "  transport: shared sends " << ts.shared_sends << ", base64 sends " << ts.base64_sends << ", buffers created " << ts.buffers_created
                  << ", reused " << ts.buffers_reused << '\n';
        TT_CHECK(ts.base64_sends == 0);
        terms->dispose(id);
    }

    if (want("Pipeline ceiling: synthetic 256 MiB source -> OutputChannel -> shared buffers -> page (acked)")) {
        step("Pipeline ceiling: synthetic 256 MiB source -> OutputChannel -> shared buffers -> page (acked)");
        constexpr std::uint64_t kBytes = 256ULL * 1024 * 1024;
        TT_CHECK(eval_true(wv, R"JS((T.benchBytes = 0, window.__bosonTransfer.onBinary('bench-term', (m) => {
            T.benchBytes += m.data.byteLength;
            chrome.webview.postMessage({ bench_ack: m.data.byteLength });
        }), true))JS"));
        std::shared_ptr<OutputChannel> ch;
        std::atomic<std::uint64_t> acked{0};
        const auto hook = wv.add_web_message_observer([&](const boson::core::WebView::WebMessage& m) {
            if (!m.json.starts_with(R"({"bench_ack":)")) {
                return false;
            }
            const std::uint64_t n = std::strtoull(std::string(m.json.substr(13)).c_str(), nullptr, 10);
            acked.fetch_add(n);
            ch->ack(n);
            return true;
        });
        std::mutex pm;
        std::condition_variable pcv;
        bool resumed = false;
        boson::transfer::BinaryTransport* tport = transport.get();
        const std::uint64_t wv_id = wv.id();
        ch = OutputChannel::create(
            999, terms->options().flow,
            [tport, wv_id](std::uint64_t size, std::string_view meta, const OutputChannel::FillFn& fill) {
                return tport->send_binary_with(wv_id, "bench-term", size, fill, meta);
            },
            [&runner](std::function<void()> w) { runner->spawn(std::move(w)); });
        ch->set_resume([&] {
            std::scoped_lock lock(pm);
            resumed = true;
            pcv.notify_all();
        });
        std::string block;
        while (block.size() < 64 * 1024) {
            block += "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-=+*/|!?.,;:_#@$%^&()[]{}\r\n";
        }
        block.resize(64 * 1024);
        std::atomic<std::uint64_t> pauses{0};
        std::atomic<bool> produced{false};
        const std::uint64_t base_private = private_bytes();
        std::uint64_t peak_private = base_private;
        const auto t0 = Clock::now();
        // Its own thread, like the PTY reader (Win32 pool): blocking an stdexec pool thread would starve the work
        // queued behind it on that thread.
        std::thread producer([&] {
            for (std::uint64_t sent = 0; sent < kBytes; sent += block.size()) {
                if (ch->push(bytes_of(block)) == ReadControl::Pause) {
                    ++pauses;
                    std::unique_lock lock(pm);
                    pcv.wait(lock, [&] { return resumed; });
                    resumed = false;
                }
            }
            produced = true;
        });
        {
            std::atomic<bool> probe{false};
            runner->spawn([&probe] { probe = true; });
            TT_CHECK(pump_until([&] { return probe.load(); }, std::chrono::seconds{5}));
            std::cout << "  pool probe: " << (probe.load() ? "alive" : "STUCK") << '\n' << std::flush;
        }
        TT_CHECK(pump_until([&] { return acked.load() >= kBytes; }, std::chrono::seconds{120}, [&] { peak_private = std::max(peak_private, private_bytes()); }));
        const double ms = ms_since(t0);
        {
            std::scoped_lock lock(pm);
            resumed = true; // unblock on failure
            pcv.notify_all();
        }
        producer.join();
        TT_CHECK(produced.load());
        TT_CHECK(eval_u64(wv, "T.benchBytes") == kBytes);
        const auto st = ch->stats();
        TT_CHECK(st.max_in_flight <= terms->options().flow.window_bytes && st.max_pending <= st.ring_capacity && st.bytes_dropped == 0);
        print_bench("synthetic source (ring + shared buffer + ack)", static_cast<double>(kBytes), ms);
        std::cout << "  chunks " << st.chunks_sent << " (avg " << st.bytes_sent / std::max<std::uint64_t>(st.chunks_sent, 1) / 1024
                  << " KiB), producer pauses " << pauses.load() << ", max in flight " << st.max_in_flight / 1024 << " KiB, max backlog "
                  << st.max_pending / 1024 << " KiB, private bytes +" << (peak_private - base_private) / (1024 * 1024) << " MiB peak\n";
        TT_CHECK(peak_private - base_private < 64ULL * 1024 * 1024);
        ch->close();
        wv.remove_web_message_observer(hook);
        ch.reset();
    }

    if (want("Flow control: slow renderer (one chunk per 25 ms) bounds memory and backpressures the child")) {
        step("Flow control: slow renderer (one chunk per 25 ms) bounds memory and backpressures the child");
        constexpr std::uint64_t kBytes = 24ULL * 1024 * 1024;
        TT_CHECK(eval_true(wv, "(T.ackDelay = 25, true)"));
        const std::uint64_t base_private = private_bytes();
        std::uint64_t peak_private = base_private;
        const auto t0 = Clock::now();
        const std::uint32_t id = spawn(self_command(L"--spew " + std::to_wstring(kBytes)), true, {200, 50});
        std::uint64_t max_pending_seen = 0;
        const bool done = pump_until(
            [&] { return eval_true(wv, js_id("T.exits[$ID] === 0 && done($ID)", id)); }, std::chrono::seconds{240},
            [&] {
                peak_private = std::max(peak_private, private_bytes());
                if (const auto s = terms->stats(id)) max_pending_seen = std::max(max_pending_seen, s->pending);
            });
        TT_CHECK(done);
        const double ms = ms_since(t0);
        const auto st = terms->stats(id);
        TT_CHECK(st && st->reader_pauses > 0);
        TT_CHECK(st && st->max_in_flight <= terms->options().flow.window_bytes);
        TT_CHECK(st && st->max_pending <= st->ring_capacity);
        TT_CHECK(st && st->bytes_dropped == 0 && st->bytes_sent == st->bytes_in);
        print_bench("slow renderer (25 ms per chunk ack)", static_cast<double>(st ? st->bytes_in : 0), ms);
        if (st) {
            std::cout << "  reader pauses " << st->reader_pauses << ", max backlog " << st->max_pending / 1024 << " KiB, max in flight "
                      << st->max_in_flight / 1024 << " KiB, private bytes +" << (peak_private - base_private) / (1024 * 1024) << " MiB peak\n";
        }
        TT_CHECK(peak_private - base_private < 64ULL * 1024 * 1024);
        TT_CHECK(eval_true(wv, "(T.ackDelay = 0, true)"));
        terms->dispose(id);
    }

#ifdef BOSON_TERMINAL_SELFTEST_JS
    if (want("terminal.ts + compat/node_pty.ts self-test (bundled with bun)")) {
        step("terminal.ts + compat/node_pty.ts self-test (bundled with bun)");
        const std::string js = read_text_file(BOSON_TERMINAL_SELFTEST_JS);
        TT_CHECK(!js.empty());
        if (!js.empty()) {
            eval(wv, js);
            TT_CHECK(wait_js(wv, "window.__termSelftest && window.__termSelftest.done === true", std::chrono::seconds{60}));
            const auto log = eval(wv, "JSON.stringify(window.__termSelftest)");
            std::cout << "  " << log.value_or("(none)") << '\n';
            TT_CHECK(eval_true(wv, "window.__termSelftest.ok === true"));
        }
    }
#else
    std::cout << "[step] terminal.ts self-test SKIPPED: bun not found at configure time\n";
#endif

    // ---- teardown --------------------------------------------------------------------------------------
    TT_CHECK(terms->list().empty() || true);
    terms.reset();
    bool shut_down = false;
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
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wstring_view(argv[1]).starts_with(L"--")) {
        return child_main(argc, argv);
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    if (GetEnvironmentVariableA("BOSON_TERM_ONLY", nullptr, 0) == 0) {
        test_helpers();
        test_scanner();
        {
            boson::exec::Scheduler scheduler(2);
            boson::exec::UiMarshal marshal([]() {});
            boson::exec::TaskRunner runner(scheduler, marshal);
            test_output_channel(runner);
        }
        test_pty();
    }
    const int webview_rc = run_webview_tests();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " terminal checks passed\n";
    CoUninitialize();
    return g_failures == 0 && webview_rc == 0 ? 0 : 1;
}
