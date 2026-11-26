// Process tests: argv quoting (CommandLineToArgvW and cmd.exe / batch files, against real children), pipes,
// line mode, extra inherited handles (CRT fds + BOSON_EXTRA_HANDLES), job objects (kill tree, kill-on-close
// when the parent dies), exec maxBuffer / timeout, senders, the executable policy, sidecar supervision
// (ready checks, restart backoff, graceful stop), named pipes, loopback ports, Node fork() IPC and a Node
// sidecar (when node.exe is on PATH), throughput numbers, and the Process controller end to end through a
// real WebView2 (skipped without a runtime). The test executable doubles as every child (--child modes).
#include <windows.h>
#include <io.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>
#include <stdexec/execution.hpp>

#include <boson/boson.hpp>
#include <boson/ipc/controllers/process_controller.hpp>
#include <boson/process/command_line.hpp>
#include <boson/process/pipe_ipc.hpp>
#include <boson/process/policy.hpp>
#include <boson/process/process.hpp>
#include <boson/process/sidecar.hpp>

namespace {

namespace fs = std::filesystem;
namespace bp = boson::process;
using namespace std::chrono_literals;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_process.cpp:" << line << " (" << expr << ")\n" << std::flush;
        ++g_failures;
    }
}

#define PROC_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

std::wstring self_path() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return path;
}

std::string self_utf8() {
    return boson::core::wide_to_utf8(self_path());
}

std::string as_text(std::span<const std::byte> data) {
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

std::vector<std::string> split_nul(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\0') {
            out.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

bool process_gone(std::uint32_t pid, DWORD timeout_ms) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h == nullptr) {
        return true; // no such process any more
    }
    const bool gone = WaitForSingleObject(h, timeout_ms) == WAIT_OBJECT_0;
    CloseHandle(h);
    return gone;
}

double mib_per_s(std::uint64_t bytes, std::chrono::steady_clock::duration d) {
    const double s = std::chrono::duration<double>(d).count();
    return s > 0 ? static_cast<double>(bytes) / (1024.0 * 1024.0) / s : 0.0;
}

// ---- child modes -----------------------------------------------------------------------------------------

void out(std::string_view text, DWORD which = STD_OUTPUT_HANDLE) {
    DWORD written = 0;
    WriteFile(GetStdHandle(which), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

std::string env(const wchar_t* name) {
    wchar_t buffer[2048]{};
    const DWORD n = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return n == 0 || n >= std::size(buffer) ? std::string{} : boson::core::wide_to_utf8(std::wstring_view(buffer, n));
}

void wait_stdin_eof() {
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    }
}

HANDLE g_break_event = nullptr;

BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_BREAK_EVENT) {
        SetEvent(g_break_event);
        return TRUE;
    }
    return FALSE;
}

int run_child(std::span<const std::wstring> args) {
    const std::wstring& mode = args[0];
    if (mode == L"--echo-args") {
        // What the CRT hands to wmain (its parser differs from CommandLineToArgvW for "" inside quotes).
        std::string text;
        for (int i = 3; i < __argc; ++i) {
            text += boson::core::wide_to_utf8(__wargv[i]);
            text.push_back('\0');
        }
        out(text);
        return 0;
    }
    if (mode == L"--exit") {
        return args.size() > 1 ? std::stoi(args[1]) : 0;
    }
    if (mode == L"--sleep") {
        Sleep(static_cast<DWORD>(std::stoul(args[1])));
        return 0;
    }
    if (mode == L"--cat") {
        std::vector<char> buffer(64 * 1024);
        DWORD read = 0;
        while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
            DWORD written = 0;
            if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer.data(), read, &written, nullptr)) {
                return 2;
            }
        }
        return 0;
    }
    if (mode == L"--flood") {
        const std::uint64_t total = std::stoull(args[1]);
        std::vector<char> buffer(64 * 1024, 'x');
        for (std::size_t i = 63; i < buffer.size(); i += 64) {
            buffer[i] = '\n';
        }
        std::uint64_t sent = 0;
        while (sent < total) {
            const DWORD n = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), total - sent));
            DWORD written = 0;
            if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer.data(), n, &written, nullptr)) {
                return 2;
            }
            sent += written;
        }
        return 0;
    }
    if (mode == L"--lines") {
        out("one\r\ntwo\nthr");
        Sleep(50);
        out("ee\n\nlast-no-newline");
        out("err1\n", STD_ERROR_HANDLE);
        return 0;
    }
    if (mode == L"--extra") {
        const auto h3 = reinterpret_cast<HANDLE>(_get_osfhandle(3));
        const auto h4 = reinterpret_cast<HANDLE>(_get_osfhandle(4));
        const std::string list = env(L"BOSON_EXTRA_HANDLES");
        char expected[64]{};
        std::snprintf(expected, sizeof(expected), "3=%llx;4=%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(h3)),
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(h4)));
        DWORD n = 0;
        const std::string_view hello = "hello fd3";
        WriteFile(h3, hello.data(), static_cast<DWORD>(hello.size()), &n, nullptr);
        CloseHandle(h3);
        std::string line;
        char c = 0;
        while (ReadFile(h4, &c, 1, &n, nullptr) && n == 1 && c != '\n') {
            line.push_back(c);
        }
        out("got " + line + "\n");
        out(list == expected ? "env ok\n" : "env mismatch " + list + " vs " + expected + "\n");
        return 0;
    }
    if (mode == L"--spawn-grandchild") {
        std::wstring cmd = L"\"" + self_path() + L"\" --child --sleep 60000";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return 5;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        out("pids " + std::to_string(GetCurrentProcessId()) + " " + std::to_string(pi.dwProcessId) + "\n");
        Sleep(60000);
        return 0;
    }
    if (mode == L"--job-host") {
        bp::ProcessManager manager;
        std::mutex m;
        std::condition_variable cv;
        std::string line;
        auto child = manager.spawn(bp::SpawnOptions{
            .file = self_utf8(),
            .args = {"--child", "--spawn-grandchild"},
            .output_mode = bp::OutputMode::Line,
            .on_lines = [&](int, std::span<const std::string_view> lines) {
                std::scoped_lock lock(m);
                line = std::string(lines.front());
                cv.notify_all();
            },
        });
        if (!child) {
            return 6;
        }
        std::unique_lock lock(m);
        cv.wait_for(lock, 10s, [&]() { return !line.empty(); });
        out(line + "\n");
        lock.unlock();
        Sleep(INFINITE);
        return 0;
    }
    if (mode == L"--ctrl-break") {
        g_break_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(&on_ctrl, TRUE);
        out("ready\n");
        if (WaitForSingleObject(g_break_event, 20000) == WAIT_OBJECT_0) {
            out("break\n");
            return 42;
        }
        return 1;
    }
    if (mode == L"--port-file") {
        const std::wstring path = boson::core::utf8_to_wide(env(L"BOSON_SIDECAR_PORT_FILE"));
        Sleep(100);
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD n = 0;
        WriteFile(f, "4321\n", 5, &n, nullptr);
        CloseHandle(f);
        wait_stdin_eof();
        return 0;
    }
    if (mode == L"--ready-line") {
        out("booting\n");
        Sleep(50);
        out("READY on " + env(L"BOSON_SIDECAR_PORT") + "\n");
        wait_stdin_eof();
        return 0;
    }
    if (mode == L"--fail-fast") {
        return 3;
    }
    if (mode == L"--pipe-sidecar") {
        auto conn = bp::PipeClient::connect(env(L"BOSON_SIDECAR_PIPE"), 5s);
        if (!conn) {
            return 7;
        }
        HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        auto c = *conn;
        c->start(
            [c, quit](std::span<const std::byte> message) {
                const std::string text = as_text(message);
                if (text == "quit") {
                    SetEvent(quit);
                } else {
                    c->send("echo:" + text);
                }
            },
            [quit](std::uint32_t) { SetEvent(quit); });
        WaitForSingleObject(quit, 20000);
        c->close();
        return 0;
    }
    return 99;
}

// ---- tests -----------------------------------------------------------------------------------------------

bp::ProcessManager& manager() {
    static bp::ProcessManager instance;
    return instance;
}

bp::ExecResult exec_self(std::vector<std::string> args, std::string input = {}, std::size_t max_buffer = 64ull << 20,
                         std::chrono::milliseconds timeout = 0ms) {
    std::vector<std::string> full{"--child"};
    full.insert(full.end(), args.begin(), args.end());
    bp::ExecOptions options;
    options.spawn.file = self_utf8();
    options.spawn.args = std::move(full);
    options.spawn.timeout = timeout;
    options.input = std::move(input);
    options.max_buffer = max_buffer;
    auto result = manager().exec_sync(std::move(options));
    PROC_CHECK(result.has_value());
    return result ? std::move(*result) : bp::ExecResult{};
}

const std::vector<std::wstring> kArgCases = {
    L"simple",
    L"",
    L"with space",
    L"  leading and trailing  ",
    L"tab\there",
    L"quote\"inside",
    L"\"",
    L"\"\"",
    L"trailing\\",
    L"trailing\\\\",
    L"C:\\Program Files\\x\\",
    L"back\\\"slash quote",
    L"\\\\server\\share",
    L"a\\\\\\\"b",
    L"unicode \u00e9\u4e2d\U0001F600",
    L"amp & pipe | lt < gt > caret ^",
    L"(parens) !bang! %PATH% %%",
    L"comma,semi;equals=",
    L"*?",
};

const std::vector<std::wstring> kBatchCases = {
    L"simple",
    L"with space",
    L"amp&pipe|lt<gt>caret^",
    L"(parens)",
    L"!bang!",
    L"%PATH%",
    L"100%",
    L"comma,semi;equals=",
    L"quote\"inside",
    L"trailing\\",
    L"\u00e9\u4e2d",
    L"",
};

void test_quoting(const fs::path& base) {
    step("argv quoting: build_command_line / CommandLineToArgvW round trip");
    {
        auto cmd = bp::build_command_line(L"C:\\Program Files\\app.exe", kArgCases);
        PROC_CHECK(cmd.has_value());
        const auto parsed = bp::split_command_line(cmd.value_or(L""));
        PROC_CHECK(parsed.size() == kArgCases.size() + 1);
        PROC_CHECK(!parsed.empty() && parsed[0] == L"C:\\Program Files\\app.exe");
        for (std::size_t i = 0; i < kArgCases.size() && i + 1 < parsed.size(); ++i) {
            if (parsed[i + 1] != kArgCases[i]) {
                std::wcerr << L"  mismatch at " << i << L": [" << parsed[i + 1] << L"] vs [" << kArgCases[i] << L"]\n";
            }
            PROC_CHECK(parsed[i + 1] == kArgCases[i]);
        }
        PROC_CHECK(!bp::quote_program_name(L"bad\"name.exe").has_value());
        PROC_CHECK(!bp::build_command_line(L"x.exe", std::vector<std::wstring>{std::wstring(L"nul\0x", 5)}).has_value());
        PROC_CHECK(bp::quote_argument(L"") == L"\"\"");
        PROC_CHECK(bp::quote_argument(L"a\\b") == L"a\\b");
        PROC_CHECK(bp::quote_argument(L"a b\\") == L"\"a b\\\\\"");
    }
    step("argv quoting: a real child sees exactly the arguments (CRT + CommandLineToArgvW)");
    {
        std::vector<std::string> args{"--echo-args"};
        for (const auto& a : kArgCases) {
            args.push_back(boson::core::wide_to_utf8(a));
        }
        const auto r = exec_self(args);
        const auto got = split_nul(r.stdout_data);
        PROC_CHECK(got.size() == kArgCases.size());
        for (std::size_t i = 0; i < got.size() && i < kArgCases.size(); ++i) {
            PROC_CHECK(got[i] == boson::core::wide_to_utf8(kArgCases[i]));
        }
    }
    step("cmd.exe: batch files get BatBadBut-safe arguments");
    {
        PROC_CHECK(bp::is_batch_file(L"C:\\x\\run.CMD"));
        PROC_CHECK(bp::is_batch_file(L"run.bat. "));
        PROC_CHECK(!bp::is_batch_file(L"run.exe"));
        PROC_CHECK(!bp::quote_cmd_argument(L"line\nbreak").has_value());
        PROC_CHECK(!bp::quote_cmd_argument(L"cr\rx").has_value());
        PROC_CHECK(bp::quote_cmd_argument(L"plain").value_or(L"?") == L"plain");
        PROC_CHECK(bp::quote_cmd_argument(L"a&b").value_or(L"?") == L"\"a&b\"");
        PROC_CHECK(bp::quote_cmd_argument(L"%x%").value_or(L"?") == L"\"%%cd:~,%x%%cd:~,%\"");
        PROC_CHECK(bp::quote_cmd_argument(L"q\"").value_or(L"?") == L"\"q\"\"\"");
        const auto line = bp::build_batch_command_line(L"C:\\a b\\x.bat", std::vector<std::wstring>{L"1", L"a b"});
        PROC_CHECK(line.value_or(L"?") == L"cmd.exe /e:ON /v:OFF /d /c \"\"C:\\a b\\x.bat\" 1 \"a b\"\"");
        PROC_CHECK(!bp::build_batch_command_line(L"C:\\x\\", {}).has_value());

        const fs::path bat = base / L"echo args.bat";
        {
            std::ofstream f(bat, std::ios::binary);
            f << "@\"" << self_utf8() << "\" --child --echo-args %*\r\n";
        }
        std::vector<std::string> args;
        for (const auto& a : kBatchCases) {
            args.push_back(boson::core::wide_to_utf8(a));
        }
        bp::ExecOptions options;
        options.spawn.file = boson::core::wide_to_utf8(bat.wstring());
        options.spawn.args = args;
        auto r = manager().exec_sync(options);
        PROC_CHECK(r.has_value());
        const auto got = split_nul(r ? r->stdout_data : std::string{});
        PROC_CHECK(got.size() == kBatchCases.size());
        for (std::size_t i = 0; i < got.size() && i < kBatchCases.size(); ++i) {
            if (got[i] != args[i]) {
                std::cerr << "  batch mismatch at " << i << ": [" << got[i] << "] vs [" << args[i] << "]\n";
            }
            PROC_CHECK(got[i] == args[i]);
        }
        // A newline cannot be escaped for cmd.exe: refused, never run.
        options.spawn.args = {"ok", "bad\nnext"};
        PROC_CHECK(!manager().exec_sync(options).has_value());
        // allow_batch_files = false refuses the script.
        options.spawn.args = {"ok"};
        options.spawn.allow_batch_files = false;
        PROC_CHECK(!manager().exec_sync(options).has_value());
    }
    step("environment block and executable lookup");
    {
        std::vector<bp::EnvVar> envv{{L"b", L"2"}, {L"A", L"1"}, {L"=C:", L"C:\\"}};
        bp::set_env(envv, L"B", std::wstring_view(L"3"));
        PROC_CHECK(bp::find_env(envv, L"b") == std::optional<std::wstring>(L"3"));
        bp::set_env(envv, L"a", std::nullopt);
        PROC_CHECK(!bp::find_env(envv, L"A").has_value());
        const auto block = bp::build_environment_block(envv);
        PROC_CHECK(block.has_value());
        const std::wstring expected(L"=C:=C:\\\0b=3\0\0", 13);
        PROC_CHECK(block && std::wstring(block->begin(), block->end()) == expected);
        PROC_CHECK(!bp::build_environment_block(std::vector<bp::EnvVar>{{L"a=b", L"x"}}).has_value());

        const auto cmd = bp::resolve_executable(L"cmd");
        PROC_CHECK(cmd.has_value());
        PROC_CHECK(cmd && fs::path(*cmd).filename() == L"cmd.exe");
        PROC_CHECK(!bp::resolve_executable(L"definitely-not-a-program-boson").has_value());
        PROC_CHECK(bp::resolve_executable(self_path()).has_value());
        auto child = exec_self({"--echo-args"});
        PROC_CHECK(child.status.exit_code == 0);
        // env passed to the child (and PATH override used for lookup)
        bp::ExecOptions options;
        options.spawn.file = "cmd";
        options.spawn.args = {"/d", "/c", "echo %BOSON_TEST_VAR%"};
        options.spawn.env = {{"BOSON_TEST_VAR", "value 42"}};
        auto r = manager().exec_sync(options);
        PROC_CHECK(r && r->stdout_data.find("value 42") != std::string::npos);
        options.spawn.clear_env = true;
        options.spawn.env.clear();
        r = manager().exec_sync(options);
        PROC_CHECK(r && r->stdout_data.find("%BOSON_TEST_VAR%") != std::string::npos);
    }
}

void test_pipes() {
    step("pipes: stdin -> stdout round trip (8 MiB of random bytes, chunked writes)");
    {
        std::vector<std::byte> payload(8u << 20);
        std::mt19937 rng(1234);
        for (auto& b : payload) {
            b = static_cast<std::byte>(rng() & 0xFF);
        }
        std::mutex m;
        std::vector<std::byte> received;
        received.reserve(payload.size());
        auto p = manager().spawn(bp::SpawnOptions{
            .file = self_utf8(),
            .args = {"--child", "--cat"},
            .on_data = [&](int fd, std::span<const std::byte> data) {
                if (fd == 1) {
                    std::scoped_lock lock(m);
                    received.insert(received.end(), data.begin(), data.end());
                }
            },
        });
        PROC_CHECK(p.has_value());
        if (p) {
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t off = 0; off < payload.size(); off += 256 * 1024) {
                PROC_CHECK((*p)->write(std::span<const std::byte>(payload).subspan(off, std::min<std::size_t>(256 * 1024, payload.size() - off))));
            }
            PROC_CHECK((*p)->close_stdin());
            const auto st = (*p)->wait(30s);
            const auto t1 = std::chrono::steady_clock::now();
            PROC_CHECK(st && st->exit_code == 0);
            std::scoped_lock lock(m);
            PROC_CHECK(received == payload);
            std::cout << "  round trip 8 MiB through a child: " << mib_per_s(payload.size(), t1 - t0) << " MiB/s\n";
            PROC_CHECK(!(*p)->write(std::string_view("late")));
        }
    }
    step("throughput: child stdout -> on_data (256 MiB), chunk mode");
    {
        std::atomic<std::uint64_t> bytes{0};
        std::atomic<std::uint64_t> callbacks{0};
        const auto t0 = std::chrono::steady_clock::now();
        auto p = manager().spawn(bp::SpawnOptions{
            .file = self_utf8(),
            .args = {"--child", "--flood", std::to_string(256ull << 20)},
            .read_buffer_bytes = 256 * 1024,
            .on_data = [&](int, std::span<const std::byte> data) {
                bytes += data.size();
                ++callbacks;
            },
        });
        PROC_CHECK(p.has_value());
        const auto st = p ? (*p)->wait(60s) : std::nullopt;
        const auto t1 = std::chrono::steady_clock::now();
        PROC_CHECK(st.has_value());
        PROC_CHECK(bytes == (256ull << 20));
        std::cout << "  stdout throughput: " << mib_per_s(bytes, t1 - t0) << " MiB/s in " << callbacks.load() << " callbacks\n";
    }
    step("line mode: CRLF / LF, lines split across reads, empty line, unterminated last line; stderr separate");
    {
        std::mutex m;
        std::vector<std::string> out_lines;
        std::vector<std::string> err_lines;
        auto p = manager().spawn(bp::SpawnOptions{
            .file = self_utf8(),
            .args = {"--child", "--lines"},
            .output_mode = bp::OutputMode::Line,
            .on_lines = [&](int fd, std::span<const std::string_view> lines) {
                std::scoped_lock lock(m);
                for (const auto l : lines) {
                    (fd == 1 ? out_lines : err_lines).emplace_back(l);
                }
            },
        });
        PROC_CHECK(p.has_value());
        PROC_CHECK(p && (*p)->wait(10s).has_value());
        std::scoped_lock lock(m);
        PROC_CHECK((out_lines == std::vector<std::string>{"one", "two", "three", "", "last-no-newline"}));
        PROC_CHECK((err_lines == std::vector<std::string>{"err1"}));
    }
    step("line throughput: 64 MiB of 64-byte lines");
    {
        std::atomic<std::uint64_t> lines{0};
        const auto t0 = std::chrono::steady_clock::now();
        auto p = manager().spawn(bp::SpawnOptions{
            .file = self_utf8(),
            .args = {"--child", "--flood", std::to_string(64ull << 20)},
            .output_mode = bp::OutputMode::Line,
            .read_buffer_bytes = 256 * 1024,
            .on_lines = [&](int, std::span<const std::string_view> batch) { lines += batch.size(); },
        });
        PROC_CHECK(p && (*p)->wait(60s).has_value());
        const auto t1 = std::chrono::steady_clock::now();
        PROC_CHECK(lines == (64ull << 20) / 64);
        std::cout << "  line mode: " << static_cast<double>(lines.load()) / std::chrono::duration<double>(t1 - t0).count() / 1e6
                  << " M lines/s (" << mib_per_s(64ull << 20, t1 - t0) << " MiB/s)\n";
    }
    step("merge stderr into stdout; Stdio::Ignore");
    {
        auto r = manager().exec_sync(bp::ExecOptions{
            .spawn = bp::SpawnOptions{.file = "cmd", .args = {"/d", "/c", "echo out& echo err 1>&2"}, .stderr_mode = bp::Stdio::MergeWithStdout},
            .input = {},
            .max_buffer = 0,
        });
        PROC_CHECK(r && r->stdout_data.find("out") != std::string::npos && r->stdout_data.find("err") != std::string::npos && r->stderr_data.empty());
        r = manager().exec_sync(bp::ExecOptions{
            .spawn = bp::SpawnOptions{.file = "cmd", .args = {"/d", "/c", "echo hidden"}, .stdout_mode = bp::Stdio::Ignore},
            .input = {},
            .max_buffer = 0,
        });
        PROC_CHECK(r && r->stdout_data.empty() && r->status.exit_code == 0);
    }
    step("flow control: reading pauses at high_water until ack()");
    {
        std::atomic<std::uint64_t> delivered{0};
        auto p = manager().spawn(bp::SpawnOptions{
            .file = self_utf8(),
            .args = {"--child", "--flood", std::to_string(8ull << 20)},
            .high_water_bytes = 256 * 1024,
            .on_data = [&](int, std::span<const std::byte> data) { delivered += data.size(); },
        });
        PROC_CHECK(p.has_value());
        Sleep(300);
        const std::uint64_t paused_at = delivered.load();
        PROC_CHECK(paused_at >= 256 * 1024 && paused_at < 1024 * 1024); // stopped near the high-water mark
        PROC_CHECK(p && (*p)->running());                               // the child is blocked on a full pipe
        std::uint64_t acked = 0;
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (p && acked < (8ull << 20) && std::chrono::steady_clock::now() < deadline) {
            const std::uint64_t now = delivered.load();
            if (now > acked) {
                (*p)->ack(1, static_cast<std::size_t>(now - acked));
                acked = now;
            }
            Sleep(1);
        }
        PROC_CHECK(p && (*p)->wait(10s).has_value());
        PROC_CHECK(delivered == (8ull << 20));
        std::cout << "  paused after " << paused_at << " bytes with high_water 262144\n";
    }
}

void test_extra_handles() {
    step("extra handles: fd 3 (child writes) and fd 4 (child reads) as CRT fds + BOSON_EXTRA_HANDLES");
    std::mutex m;
    std::string fd3;
    std::string stdout_text;
    auto p = manager().spawn(bp::SpawnOptions{
        .file = self_utf8(),
        .args = {"--child", "--extra"},
        .extra_handles = {bp::ExtraHandle{.direction = bp::PipeDirection::ChildWrites, .child_overlapped = false, .inherit = nullptr},
                          bp::ExtraHandle{.direction = bp::PipeDirection::ChildReads, .child_overlapped = false, .inherit = nullptr}},
        .on_data = [&](int fd, std::span<const std::byte> data) {
            std::scoped_lock lock(m);
            (fd == 3 ? fd3 : stdout_text) += as_text(data);
        },
    });
    PROC_CHECK(p.has_value());
    if (!p) {
        return;
    }
    PROC_CHECK((*p)->extra_child_handles().size() == 2);
    PROC_CHECK((*p)->write_to(4, std::as_bytes(std::span<const char>("ping\n", 5))));
    const auto st = (*p)->wait(10s);
    PROC_CHECK(st && st->exit_code == 0);
    std::scoped_lock lock(m);
    PROC_CHECK(fd3 == "hello fd3");
    PROC_CHECK(stdout_text.find("got ping") != std::string::npos);
    PROC_CHECK(stdout_text.find("env ok") != std::string::npos);
    if (stdout_text.find("env ok") == std::string::npos) {
        std::cerr << "  child said: " << stdout_text << '\n';
    }

}

std::pair<std::uint32_t, std::uint32_t> parse_pids(const std::string& line) {
    unsigned long a = 0;
    unsigned long b = 0;
    if (sscanf_s(line.c_str(), "pids %lu %lu", &a, &b) != 2) {
        return {0, 0};
    }
    return {static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)};
}

void test_jobs() {
    step("kill(tree): child and grandchild both die");
    {
        std::mutex m;
        std::condition_variable cv;
        std::string line;
        auto p = manager().spawn(bp::SpawnOptions{
            .file = self_utf8(),
            .args = {"--child", "--spawn-grandchild"},
            .output_mode = bp::OutputMode::Line,
            .on_lines = [&](int, std::span<const std::string_view> lines) {
                std::scoped_lock lock(m);
                line = std::string(lines.front());
                cv.notify_all();
            },
        });
        PROC_CHECK(p.has_value());
        {
            std::unique_lock lock(m);
            cv.wait_for(lock, 10s, [&]() { return !line.empty(); });
        }
        const auto [child, grandchild] = parse_pids(line);
        PROC_CHECK(child != 0 && grandchild != 0);
        PROC_CHECK(p && (*p)->in_job());
        PROC_CHECK(manager().job() != nullptr);
        const auto descendants = bp::descendant_pids(child);
        PROC_CHECK(std::find(descendants.begin(), descendants.end(), grandchild) != descendants.end());
        PROC_CHECK(p && (*p)->kill(9));
        const auto st = p ? (*p)->wait(10s) : std::nullopt;
        PROC_CHECK(st && st->exit_code == 9 && st->killed);
        PROC_CHECK(process_gone(grandchild, 5000));
    }
    step("kill-on-close: the parent is terminated (crash), its job dies with it: no grandchild survives");
    {
        std::mutex m;
        std::condition_variable cv;
        std::string line;
        auto host = manager().spawn(bp::SpawnOptions{
            .file = self_utf8(),
            .args = {"--child", "--job-host"},
            .output_mode = bp::OutputMode::Line,
            .on_lines = [&](int, std::span<const std::string_view> lines) {
                std::scoped_lock lock(m);
                line = std::string(lines.front());
                cv.notify_all();
            },
        });
        PROC_CHECK(host.has_value());
        {
            std::unique_lock lock(m);
            cv.wait_for(lock, 15s, [&]() { return !line.empty(); });
        }
        const auto [child, grandchild] = parse_pids(line);
        PROC_CHECK(child != 0 && grandchild != 0);
        PROC_CHECK(!process_gone(child, 0) && !process_gone(grandchild, 0));
        // Only the host itself: the kernel closes its handles, KILL_ON_JOB_CLOSE does the rest.
        PROC_CHECK(host && TerminateProcess((*host)->native_handle(), 1));
        PROC_CHECK(process_gone(child, 5000));
        PROC_CHECK(process_gone(grandchild, 5000));
        PROC_CHECK(host && (*host)->wait(10s).has_value());
    }
    step("detached: not in the job, kill(tree) still reaches it through the snapshot walk");
    {
        auto p = manager().spawn(bp::SpawnOptions{.file = self_utf8(), .args = {"--child", "--sleep", "30000"}, .stdout_mode = bp::Stdio::Ignore,
                                                  .stderr_mode = bp::Stdio::Ignore, .detached = true});
        PROC_CHECK(p.has_value());
        PROC_CHECK(p && !(*p)->in_job());
        PROC_CHECK(p && (*p)->kill());
        PROC_CHECK(p && (*p)->wait(10s).has_value());
    }
}

void test_exec() {
    step("exec: maxBuffer kills the child and truncates; timeout kills the tree");
    {
        const auto r = exec_self({"--flood", std::to_string(8u << 20)}, {}, 100000);
        PROC_CHECK(r.max_buffer_exceeded);
        PROC_CHECK(r.stdout_data.size() == 100000);
        PROC_CHECK(r.status.killed);
    }
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = exec_self({"--sleep", "20000"}, {}, 1024, 300ms);
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        PROC_CHECK(r.status.timed_out && r.status.killed);
        PROC_CHECK(elapsed < 5s);
        std::cout << "  timeout fired after " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms (limit 300)\n";
    }
    {
        const auto r = exec_self({"--cat"}, "stdin text");
        PROC_CHECK(r.stdout_data == "stdin text");
        const auto e = exec_self({"--exit", "17"});
        PROC_CHECK(e.status.exit_code == 17 && !e.status.killed);
    }
    step("senders: ProcessManager::exec and Process::when_closed with stdexec::sync_wait");
    {
        bp::ExecOptions options;
        options.spawn.file = self_utf8();
        options.spawn.args = {"--child", "--echo-args", "via sender"};
        auto result = stdexec::sync_wait(manager().exec(options));
        PROC_CHECK(result.has_value());
        if (result) {
            PROC_CHECK(split_nul(std::get<0>(*result).stdout_data) == std::vector<std::string>{"via sender"});
        }
        auto p = manager().spawn(bp::SpawnOptions{.file = self_utf8(), .args = {"--child", "--exit", "5"}});
        PROC_CHECK(p.has_value());
        if (p) {
            auto st = stdexec::sync_wait((*p)->when_closed() | stdexec::then([](bp::ExitStatus s) { return s.exit_code; }));
            PROC_CHECK(st && std::get<0>(*st) == 5u);
            auto again = stdexec::sync_wait((*p)->when_closed()); // already closed: completes at once
            PROC_CHECK(again.has_value());
        }
        bp::ExecOptions missing;
        missing.spawn.file = "no-such-program-boson";
        bool errored = false;
        try {
            static_cast<void>(stdexec::sync_wait(manager().exec(missing)));
        } catch (const boson::exec::HResultError&) {
            errored = true;
        }
        PROC_CHECK(errored);
    }
    step("spawn latency (exec_sync of a trivial child, 20 runs)");
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) {
            const auto r = exec_self({"--exit", "0"});
            PROC_CHECK(r.status.exit_code == 0);
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::cout << "  spawn + exit + close (this Debug test exe as child): " << std::chrono::duration<double, std::milli>(t1 - t0).count() / 20.0
                  << " ms per process\n";
        double spawn_call_ms = 0;
        const auto c0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) {
            const auto s0 = std::chrono::steady_clock::now();
            auto p = manager().spawn(bp::SpawnOptions{.file = "cmd", .args = {"/d", "/c", "exit"}});
            spawn_call_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
            PROC_CHECK(p && (*p)->wait(10s).has_value());
        }
        const auto c1 = std::chrono::steady_clock::now();
        std::cout << "  cmd.exe /d /c exit: " << std::chrono::duration<double, std::milli>(c1 - c0).count() / 20.0
                  << " ms per process, of which spawn() itself " << spawn_call_ms / 20.0 << " ms\n";
    }
    step("idle cost: a running child with three pipes uses no CPU in this process");
    {
        auto p = manager().spawn(bp::SpawnOptions{.file = self_utf8(), .args = {"--child", "--sleep", "1500"}});
        PROC_CHECK(p.has_value());
        Sleep(100);
        FILETIME c{}, e{}, k0{}, u0{}, k1{}, u1{};
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k0, &u0);
        Sleep(1000);
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k1, &u1);
        const auto ticks = [](const FILETIME& f) { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const double cpu_ms = static_cast<double>((ticks(k1) - ticks(k0)) + (ticks(u1) - ticks(u0))) / 10'000.0;
        std::cout << "  CPU time of this process during 1 s with an idle child: " << cpu_ms << " ms\n";
        PROC_CHECK(cpu_ms < 20.0);
        PROC_CHECK(p && (*p)->wait(10s).has_value());
    }
}

void test_policy(const fs::path& base) {
    step("ExecutablePolicy: default deny, exact paths, directories, PATH commands, batch files, cwd, filter");
    const auto deny = bp::ExecutablePolicy::deny_all();
    PROC_CHECK(!deny->authorize(self_utf8(), {}).has_value());
    PROC_CHECK(!deny->authorize("cmd", {}).has_value());

    auto exact = bp::ExecutablePolicy::create(bp::ExecutablePolicyConfig{.executables = {self_utf8()}});
    PROC_CHECK(exact.has_value());
    if (exact) {
        auto ok = (*exact)->authorize(self_utf8(), {});
        PROC_CHECK(ok.has_value());
        PROC_CHECK(!(*exact)->authorize("cmd", {}).has_value());
        PROC_CHECK(!(*exact)->authorize(bp::system_cmd_path() == L"" ? "x" : boson::core::wide_to_utf8(bp::system_cmd_path()), {}).has_value());
        const auto relative = fs::path(self_path()).filename().string();
        PROC_CHECK(!(*exact)->authorize(relative, {}).has_value());       // a bare name is not a path match
        PROC_CHECK(!(*exact)->authorize(".\\" + relative, {}).has_value()); // relative paths are refused
        auto denied = (*exact)->authorize("C:\\Windows\\System32\\cmd.exe", {});
        PROC_CHECK(!denied && denied.error().code == ERROR_ACCESS_DENIED);
    }
    auto by_dir = bp::ExecutablePolicy::create(bp::ExecutablePolicyConfig{.directories = {fs::path(self_path()).parent_path().string()}});
    PROC_CHECK(by_dir && (*by_dir)->authorize(self_utf8(), {}).has_value());
    auto by_name = bp::ExecutablePolicy::create(bp::ExecutablePolicyConfig{.commands = {"cmd"}});
    PROC_CHECK(by_name && (*by_name)->authorize("cmd", {}).has_value());
    PROC_CHECK(by_name && (*by_name)->authorize("CMD.EXE", {}).has_value());
    PROC_CHECK(by_name && !(*by_name)->authorize("powershell", {}).has_value());
    PROC_CHECK(!bp::ExecutablePolicy::create(bp::ExecutablePolicyConfig{.commands = {"..\\evil"}}).has_value());
    PROC_CHECK(!bp::ExecutablePolicy::create(bp::ExecutablePolicyConfig{.executables = {"relative.exe"}}).has_value());

    const fs::path bat = base / L"script.cmd";
    {
        std::ofstream f(bat);
        f << "@echo hi\r\n";
    }
    auto scripts = bp::ExecutablePolicy::create(bp::ExecutablePolicyConfig{.executables = {bat.string()}});
    PROC_CHECK(scripts && !(*scripts)->authorize(bat.string(), {}).has_value()); // batch needs allow_batch_files
    auto scripts_ok = bp::ExecutablePolicy::create(bp::ExecutablePolicyConfig{.executables = {bat.string()}, .allow_batch_files = true});
    PROC_CHECK(scripts_ok && (*scripts_ok)->authorize(bat.string(), {}).has_value());

    auto filtered = bp::ExecutablePolicy::create(
        bp::ExecutablePolicyConfig{.executables = {self_utf8()}},
        [](std::wstring_view, std::span<const std::string> args) { return std::none_of(args.begin(), args.end(), [](const std::string& a) { return a == "--evil"; }); });
    const std::vector<std::string> good{"--fine"};
    const std::vector<std::string> bad{"--evil"};
    PROC_CHECK(filtered && (*filtered)->authorize(self_utf8(), good).has_value());
    PROC_CHECK(filtered && !(*filtered)->authorize(self_utf8(), bad).has_value());

    auto scope = std::make_shared<boson::security::FsScope>();
    PROC_CHECK(scope->allow(std::string_view(base.string())).has_value());
    auto scoped = bp::ExecutablePolicy::create(bp::ExecutablePolicyConfig{.executables = {self_utf8()}}, {}, scope);
    PROC_CHECK(scoped && (*scoped)->authorize_cwd(base.string()).has_value());
    PROC_CHECK(scoped && !(*scoped)->authorize_cwd("C:\\Windows").has_value());

    auto json = bp::ExecutablePolicy::from_json(R"({"commands":["cmd"],"allow_detached":true,"max_processes":3})");
    PROC_CHECK(json && (*json)->config().allow_detached && (*json)->config().max_processes == 3);
    PROC_CHECK(!bp::ExecutablePolicy::from_json(R"({"comands":["typo"]})").has_value());
}

void test_named_pipes() {
    step("named pipes: message mode round trip (0 B .. 4 MiB, bigger than the pipe buffer), both directions");
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> server_got;
    std::shared_ptr<bp::PipeConnection> server_side;
    const std::wstring path = bp::make_unique_pipe_path(L"boson-test");
    auto server = bp::PipeServer::listen(boson::core::wide_to_utf8(path), [&](std::shared_ptr<bp::PipeConnection> c) {
        {
            std::scoped_lock lock(m);
            server_side = c;
        }
        std::weak_ptr<bp::PipeConnection> weak = c;
        c->start([&, weak](std::span<const std::byte> message) {
            {
                std::scoped_lock lock(m);
                server_got.push_back(as_text(message));
            }
            cv.notify_all();
            if (auto self = weak.lock()) {
                self->send(message); // echo
            }
        });
        cv.notify_all();
    });
    PROC_CHECK(server.has_value());
    if (!server) {
        return;
    }
    PROC_CHECK(!bp::PipeServer::listen(boson::core::wide_to_utf8(path), [](auto) {}).has_value()); // first instance is ours
    auto client = bp::PipeClient::connect(boson::core::wide_to_utf8(path), 2s);
    PROC_CHECK(client.has_value());
    if (!client) {
        return;
    }
    PROC_CHECK((*client)->peer_pid() == GetCurrentProcessId());
    std::vector<std::string> client_got;
    (*client)->start([&](std::span<const std::byte> message) {
        std::scoped_lock lock(m);
        client_got.push_back(as_text(message));
        cv.notify_all();
    });
    const std::vector<std::size_t> sizes{1, 100, 65536, 65537, 1u << 20, 4u << 20, 7};
    std::vector<std::string> sent;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        std::string msg(sizes[i], static_cast<char>('a' + i));
        msg[0] = static_cast<char>('0' + i);
        sent.push_back(msg);
        PROC_CHECK((*client)->send(msg));
    }
    {
        std::unique_lock lock(m);
        PROC_CHECK(cv.wait_for(lock, 20s, [&]() { return client_got.size() == sent.size(); }));
        PROC_CHECK(server_got == sent);
        PROC_CHECK(client_got == sent);
        PROC_CHECK(server_side != nullptr && server_side->peer_pid() == GetCurrentProcessId());
    }
    PROC_CHECK((*server)->connection_count() == 1);

    step("named pipes: 100k x 256 B messages one way (throughput)");
    {
        std::atomic<std::uint64_t> count{0};
        {
            std::scoped_lock lock(m);
            server_got.clear();
        }
        std::shared_ptr<bp::PipeConnection> receiver;
        const std::wstring path2 = bp::make_unique_pipe_path(L"boson-bench");
        auto bench = bp::PipeServer::listen(boson::core::wide_to_utf8(path2), [&](std::shared_ptr<bp::PipeConnection> c) {
            std::scoped_lock lock(m);
            receiver = c;
            c->start([&](std::span<const std::byte>) {
                if (++count == 100000) {
                    cv.notify_all();
                }
            });
        });
        PROC_CHECK(bench.has_value());
        auto sender = bp::PipeClient::connect(boson::core::wide_to_utf8(path2), 2s);
        PROC_CHECK(sender.has_value());
        if (bench && sender) {
            const std::string msg(256, 'm');
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 100000; ++i) {
                (*sender)->send(msg);
            }
            std::unique_lock lock(m);
            PROC_CHECK(cv.wait_for(lock, 60s, [&]() { return count.load() == 100000; }));
            const auto t1 = std::chrono::steady_clock::now();
            std::cout << "  " << 100000.0 / std::chrono::duration<double>(t1 - t0).count() / 1000.0 << " k messages/s ("
                      << mib_per_s(100000ull * 256, t1 - t0) << " MiB/s)\n";
        }
        if (sender) {
            (*sender)->close();
        }
    }
    bool closed = false;
    auto third = bp::PipeClient::connect(boson::core::wide_to_utf8(path), 2s);
    PROC_CHECK(third.has_value());
    if (third) {
        (*third)->start([](auto) {}, [&](std::uint32_t) {
            std::scoped_lock lock(m);
            closed = true;
            cv.notify_all();
        });
    }
    (*server)->stop();
    {
        std::unique_lock lock(m);
        PROC_CHECK(cv.wait_for(lock, 5s, [&]() { return closed; })); // the client sees the server go away
    }
    PROC_CHECK(!bp::PipeClient::connect(boson::core::wide_to_utf8(path), 200ms).has_value());
    (*client)->close();
}

void test_ports() {
    step("loopback port allocator");
    const auto a = bp::allocate_loopback_port();
    const auto b = bp::allocate_loopback_port();
    PROC_CHECK(a && *a != 0);
    PROC_CHECK(b && *b != 0);
    PROC_CHECK(a && !bp::is_loopback_port_listening(*a));
}

struct StateLog {
    std::mutex m;
    std::vector<std::pair<bp::SidecarState, std::chrono::steady_clock::time_point>> states;
    void add(bp::SidecarState s) {
        std::scoped_lock lock(m);
        states.emplace_back(s, std::chrono::steady_clock::now());
    }
};

void test_sidecar(const fs::path& base) {
    step("sidecar: restart with exponential backoff, then Failed");
    {
        auto log = std::make_shared<StateLog>();
        bp::Sidecar sidecar(manager(), bp::SidecarOptions{
            .name = "fail-fast",
            .spawn = bp::SpawnOptions{.file = self_utf8(), .args = {"--child", "--fail-fast"}},
            .restart = bp::RestartPolicy{.mode = bp::RestartMode::OnFailure, .max_restarts = 3, .initial_backoff = 100ms,
                                         .max_backoff = 1s, .multiplier = 2.0, .reset_after = 60s},
            .ready = bp::ReadyCheck{.kind = bp::ReadyKind::FirstLine},
            .on_state = [log](bp::SidecarState s) { log->add(s); },
        });
        PROC_CHECK(sidecar.start());
        PROC_CHECK(sidecar.wait_stopped(15s));
        PROC_CHECK(sidecar.state() == bp::SidecarState::Failed);
        PROC_CHECK(sidecar.starts() == 4);
        PROC_CHECK(sidecar.restarts() == 3);
        std::vector<std::chrono::steady_clock::time_point> starts;
        std::vector<std::chrono::steady_clock::time_point> backoffs;
        {
            std::scoped_lock lock(log->m);
            for (const auto& [s, t] : log->states) {
                if (s == bp::SidecarState::Starting) {
                    starts.push_back(t);
                } else if (s == bp::SidecarState::Backoff) {
                    backoffs.push_back(t);
                }
            }
        }
        PROC_CHECK(starts.size() == 4 && backoffs.size() == 3);
        for (std::size_t i = 0; i + 1 < starts.size() && i < backoffs.size(); ++i) {
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(starts[i + 1] - backoffs[i]);
            const auto expected = bp::Sidecar::backoff_delay(bp::RestartPolicy{.initial_backoff = 100ms, .max_backoff = 1s, .multiplier = 2.0},
                                                             static_cast<std::uint32_t>(i));
            std::cout << "  backoff " << i << ": " << gap.count() << " ms (policy " << expected.count() << " ms)\n";
            PROC_CHECK(gap >= expected - 20ms);
        }
        PROC_CHECK(bp::Sidecar::backoff_delay(bp::RestartPolicy{.initial_backoff = 100ms, .max_backoff = 1s, .multiplier = 2.0}, 10) == 1000ms);
    }
    step("sidecar: LineContains ready, BOSON_SIDECAR_PORT, when_ready sender, graceful CloseStdin stop");
    {
        bp::Sidecar sidecar(manager(), bp::SidecarOptions{
            .name = "ready-line",
            .spawn = bp::SpawnOptions{.file = self_utf8(), .args = {"--child", "--ready-line"}},
            .ready = bp::ReadyCheck{.kind = bp::ReadyKind::LineContains, .pattern = "READY"},
            .stop = bp::StopPolicy{.method = bp::StopMethod::CloseStdin, .message = {}, .grace = 3s},
            .allocate_port = true,
        });
        PROC_CHECK(sidecar.start());
        auto ready = stdexec::sync_wait(sidecar.when_ready());
        PROC_CHECK(ready.has_value());
        if (ready) {
            const auto& info = std::get<0>(*ready);
            PROC_CHECK(info.port != 0);
            PROC_CHECK(info.line == "READY on " + std::to_string(info.port));
            std::cout << "  ready after " << info.startup_time.count() << " ms: " << info.line << '\n';
        }
        PROC_CHECK(sidecar.state() == bp::SidecarState::Ready);
        auto p = sidecar.process();
        PROC_CHECK(sidecar.stop_and_wait(10s));
        PROC_CHECK(sidecar.state() == bp::SidecarState::Stopped);
        PROC_CHECK(p && p->exit_status() && p->exit_status()->exit_code == 0 && !p->exit_status()->killed);
    }
    step("sidecar: PortFile ready (directory change notification)");
    {
        bp::Sidecar sidecar(manager(), bp::SidecarOptions{
            .name = "port-file",
            .spawn = bp::SpawnOptions{.file = self_utf8(), .args = {"--child", "--port-file"}},
            .ready = bp::ReadyCheck{.kind = bp::ReadyKind::PortFile, .pattern = {}, .port_file = base / L"ports" / L"sidecar.port"},
        });
        PROC_CHECK(sidecar.start());
        const auto info = sidecar.wait_ready(10s);
        PROC_CHECK(info && info->port == 4321);
        PROC_CHECK(sidecar.stop_and_wait(10s));
    }
    step("sidecar: PipeConnect ready, messages both ways, PipeMessage stop");
    {
        std::mutex m;
        std::condition_variable cv;
        std::vector<std::string> got;
        bp::Sidecar sidecar(manager(), bp::SidecarOptions{
            .name = "pipe",
            .spawn = bp::SpawnOptions{.file = self_utf8(), .args = {"--child", "--pipe-sidecar"}},
            .ready = bp::ReadyCheck{.kind = bp::ReadyKind::PipeConnect},
            .stop = bp::StopPolicy{.method = bp::StopMethod::PipeMessage, .message = "quit", .grace = 3s},
            .on_pipe_message = [&](std::span<const std::byte> message) {
                std::scoped_lock lock(m);
                got.push_back(as_text(message));
                cv.notify_all();
            },
        });
        PROC_CHECK(sidecar.start());
        PROC_CHECK(sidecar.wait_ready(10s).has_value());
        PROC_CHECK(!sidecar.pipe_path().empty());
        PROC_CHECK(sidecar.send(std::string_view("hello")));
        PROC_CHECK(sidecar.send(std::string_view("{\"json\":true}")));
        {
            std::unique_lock lock(m);
            PROC_CHECK(cv.wait_for(lock, 10s, [&]() { return got.size() == 2; }));
            PROC_CHECK((got == std::vector<std::string>{"echo:hello", "echo:{\"json\":true}"}));
        }
        // A foreign client (this process, not the sidecar) is rejected.
        auto intruder = bp::PipeClient::connect(boson::core::wide_to_utf8(sidecar.pipe_path()), 1s);
        if (intruder) {
            bool dropped = false;
            (*intruder)->start([](auto) {}, [&](std::uint32_t) {
                std::scoped_lock lock(m);
                dropped = true;
                cv.notify_all();
            });
            std::unique_lock lock(m);
            PROC_CHECK(cv.wait_for(lock, 5s, [&]() { return dropped; }));
        }
        auto p = sidecar.process();
        PROC_CHECK(sidecar.stop_and_wait(10s));
        PROC_CHECK(p && p->exit_status() && p->exit_status()->exit_code == 0 && !p->exit_status()->killed);
    }
    step("sidecar: ready timeout kills and restarts; restart(); Always restarts a clean exit");
    {
        bp::Sidecar sidecar(manager(), bp::SidecarOptions{
            .name = "slow",
            .spawn = bp::SpawnOptions{.file = self_utf8(), .args = {"--child", "--sleep", "20000"}},
            .restart = bp::RestartPolicy{.mode = bp::RestartMode::OnFailure, .max_restarts = 1, .initial_backoff = 50ms},
            .ready = bp::ReadyCheck{.kind = bp::ReadyKind::FirstLine, .pattern = {}, .port_file = {}, .pipe_name = {}, .timeout = 300ms},
        });
        PROC_CHECK(sidecar.start());
        PROC_CHECK(sidecar.wait_stopped(10s));
        PROC_CHECK(sidecar.state() == bp::SidecarState::Failed && sidecar.starts() == 2);
    }
    {
        bp::Sidecar sidecar(manager(), bp::SidecarOptions{
            .name = "always",
            .spawn = bp::SpawnOptions{.file = self_utf8(), .args = {"--child", "--ready-line"}},
            .restart = bp::RestartPolicy{.mode = bp::RestartMode::Always, .max_restarts = 2, .initial_backoff = 10ms},
            .ready = bp::ReadyCheck{.kind = bp::ReadyKind::FirstLine},
        });
        PROC_CHECK(sidecar.start());
        PROC_CHECK(sidecar.wait_ready(10s).has_value());
        const auto first = sidecar.process();
        PROC_CHECK(sidecar.restart());
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (sidecar.starts() < 2 && std::chrono::steady_clock::now() < deadline) {
            Sleep(10);
        }
        PROC_CHECK(sidecar.wait_ready(10s).has_value());
        PROC_CHECK(sidecar.process() != nullptr && sidecar.process() != first && sidecar.restarts() == 0);
        PROC_CHECK(sidecar.stop_and_wait(10s));
        PROC_CHECK(sidecar.starts() == 2); // a requested stop is never restarted
    }
    step("sidecar: Ctrl+Break graceful stop (child shares this console)");
    if (GetConsoleWindow() != nullptr || GetStdHandle(STD_OUTPUT_HANDLE) != nullptr) {
        bp::Sidecar sidecar(manager(), bp::SidecarOptions{
            .name = "ctrl-break",
            .spawn = bp::SpawnOptions{.file = self_utf8(), .args = {"--child", "--ctrl-break"}, .console = bp::Console::Inherit},
            .ready = bp::ReadyCheck{.kind = bp::ReadyKind::FirstLine},
            .stop = bp::StopPolicy{.method = bp::StopMethod::CtrlBreak, .message = {}, .grace = 5s},
        });
        PROC_CHECK(sidecar.start());
        PROC_CHECK(sidecar.wait_ready(10s).has_value());
        auto p = sidecar.process();
        PROC_CHECK(sidecar.stop_and_wait(10s));
        const auto st = p ? p->exit_status() : std::nullopt;
        PROC_CHECK(st && st->exit_code == 42); // the handler ran: graceful, not TerminateProcess
        if (st) {
            std::cout << "  exit code after Ctrl+Break: " << st->exit_code << '\n';
        }
    }
}

void test_node(const fs::path& base) {
    const auto node = bp::resolve_executable(L"node");
    if (!node) {
        std::cout << "SKIP: node.exe not on PATH (fork() IPC and Node sidecar checks not run)\n";
        return;
    }
    step("Node child_process.fork-compatible IPC (node_ipc): process.send / process.on('message')");
    {
        const fs::path script = base / L"ipc_child.js";
        {
            std::ofstream f(script, std::ios::binary);
            f << "process.on('message', (m) => {\n"
                 "  if (m && m.bye) { process.disconnect(); return; }\n"
                 "  process.send({ echo: m, pid: process.pid });\n"
                 "});\n"
                 "process.send({ hello: 'from node', version: process.version });\n";
        }
        std::mutex m;
        std::condition_variable cv;
        std::vector<std::string> messages;
        auto p = manager().spawn(bp::SpawnOptions{
            .file = boson::core::wide_to_utf8(*node),
            .args = {script.string()},
            .node_ipc = true,
            .on_message = [&](std::string_view json) {
                std::scoped_lock lock(m);
                messages.emplace_back(json);
                cv.notify_all();
            },
        });
        PROC_CHECK(p.has_value());
        if (p) {
            {
                std::unique_lock lock(m);
                PROC_CHECK(cv.wait_for(lock, 15s, [&]() { return !messages.empty(); }));
            }
            PROC_CHECK((*p)->send_message(R"({"n":1,"text":"hi"})"));
            {
                std::unique_lock lock(m);
                PROC_CHECK(cv.wait_for(lock, 10s, [&]() { return messages.size() >= 2; }));
                std::cout << "  node said: " << (messages.empty() ? "" : messages.front()) << '\n';
                PROC_CHECK(messages.size() >= 2 && messages[0].find("\"hello\":\"from node\"") != std::string::npos);
                PROC_CHECK(messages.size() >= 2 && messages[1].find(R"("echo":{"n":1,"text":"hi"})") != std::string::npos);
            }
            const auto t0 = std::chrono::steady_clock::now();
            constexpr int kRoundTrips = 2000;
            for (int i = 0; i < kRoundTrips; ++i) {
                (*p)->send_message(R"({"i":)" + std::to_string(i) + "}");
            }
            {
                std::unique_lock lock(m);
                PROC_CHECK(cv.wait_for(lock, 30s, [&]() { return messages.size() >= 2 + kRoundTrips; }));
            }
            const auto t1 = std::chrono::steady_clock::now();
            std::cout << "  " << kRoundTrips << " pipelined IPC round trips with node: "
                      << std::chrono::duration<double, std::micro>(t1 - t0).count() / kRoundTrips << " us each\n";
            PROC_CHECK((*p)->send_message(R"({"bye":true})"));
            const auto st = (*p)->wait(15s);
            PROC_CHECK(st && st->exit_code == 0); // disconnect lets node exit on its own
        }
    }
    step("bundled-Node sidecar pattern: node connects to BOSON_SIDECAR_PIPE, ready on connect, JSON messages");
    {
        const fs::path script = base / L"sidecar.js";
        {
            std::ofstream f(script, std::ios::binary);
            f << "const net = require('net');\n"
                 "const s = net.connect(process.env.BOSON_SIDECAR_PIPE, () => s.write(JSON.stringify({ready: true})));\n"
                 "s.on('data', (d) => { const t = d.toString(); if (t === 'quit') { s.end(); process.exit(0); }\n"
                 "  s.write(JSON.stringify({ upper: t.toUpperCase() })); });\n";
        }
        std::mutex m;
        std::condition_variable cv;
        std::vector<std::string> got;
        bp::Sidecar sidecar(manager(), bp::SidecarOptions{
            .name = "node",
            .spawn = bp::SpawnOptions{.file = boson::core::wide_to_utf8(*node), .args = {script.string()}},
            .ready = bp::ReadyCheck{.kind = bp::ReadyKind::PipeConnect},
            .stop = bp::StopPolicy{.method = bp::StopMethod::PipeMessage, .message = "quit", .grace = 3s},
            .on_pipe_message = [&](std::span<const std::byte> message) {
                std::scoped_lock lock(m);
                got.push_back(as_text(message));
                cv.notify_all();
            },
        });
        const auto t0 = std::chrono::steady_clock::now();
        PROC_CHECK(sidecar.start());
        const auto ready = sidecar.wait_ready(15s);
        PROC_CHECK(ready.has_value());
        std::cout << "  node sidecar ready (pipe connected) after "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() << " ms\n";
        {
            std::unique_lock lock(m);
            PROC_CHECK(cv.wait_for(lock, 10s, [&]() { return !got.empty(); }));
        }
        PROC_CHECK(sidecar.send(std::string_view("abc")));
        {
            std::unique_lock lock(m);
            PROC_CHECK(cv.wait_for(lock, 10s, [&]() { return got.size() >= 2; }));
            PROC_CHECK(got.size() >= 2 && got[0] == R"({"ready":true})" && got[1] == R"({"upper":"ABC"})");
        }
        auto p = sidecar.process();
        PROC_CHECK(sidecar.stop_and_wait(10s));
        PROC_CHECK(p && p->exit_status() && !p->exit_status()->killed);
    }
}

// ---- Process controller through a real WebView2 -----------------------------------------------------------

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = 20s) {
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

std::optional<std::string> eval(boson::core::WebView& webview, std::string_view js) {
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
    pump_until([&]() { return done; }, 10s);
    return result;
}

bool contains(const std::optional<std::string>& haystack, std::string_view needle) {
    return haystack.has_value() && haystack->find(needle) != std::string::npos;
}

const std::string kPage = "<!doctype html><html><head><title>process</title></head><body>process</body></html>";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/index.html") {
        return std::as_bytes(std::span<const char>(kPage.data(), kPage.size()));
    }
    return std::nullopt;
}

std::string js_string(std::string_view text) {
    std::string out;
    static_cast<void>(glz::write_json(text, out));
    return out;
}

void test_controller_headless() {
    step("Process controller via Router: policy gate, unknown ids, list");
    bp::ProcessManager pm;
    auto policy = bp::ExecutablePolicy::create(bp::ExecutablePolicyConfig{.executables = {self_utf8()}, .max_processes = 2});
    PROC_CHECK(policy.has_value());
    boson::ipc::ProcessController controller(pm, {.policy = policy.value_or(nullptr), .transport = nullptr, .binary_channel = "process"});
    boson::ipc::Router<boson::ipc::ProcessController> router(controller);
    const auto denied = router.dispatch_sync(R"({"id":1,"method":"Process.spawn","params":{"command":"C:\\Windows\\System32\\cmd.exe","args":["/c","echo"]}})");
    PROC_CHECK(denied.find("permission denied") != std::string::npos);
    const auto shell = router.dispatch_sync(R"({"id":2,"method":"Process.spawn","params":{"command":"cmd"}})");
    PROC_CHECK(shell.find("not an allowed command") != std::string::npos);
    const auto detached = router.dispatch_sync(R"({"id":3,"method":"Process.spawn","params":{"command":)" + js_string(self_utf8()) +
                                               R"(,"args":["--child","--exit","0"],"detached":true}})");
    PROC_CHECK(detached.find("detached processes are not allowed") != std::string::npos);
    const auto ok = router.dispatch_sync(R"({"id":4,"method":"Process.spawn","params":{"command":)" + js_string(self_utf8()) +
                                         R"(,"args":["--child","--sleep","5000"]}})");
    PROC_CHECK(ok.find("\"success\"") != std::string::npos && ok.find("\"pid\"") != std::string::npos);
    const auto ok2 = router.dispatch_sync(R"({"id":5,"method":"Process.spawn","params":{"command":)" + js_string(self_utf8()) +
                                          R"(,"args":["--child","--sleep","5000"]}})");
    PROC_CHECK(ok2.find("\"success\"") != std::string::npos);
    const auto third = router.dispatch_sync(R"({"id":6,"method":"Process.spawn","params":{"command":)" + js_string(self_utf8()) +
                                            R"(,"args":["--child","--sleep","5000"]}})");
    PROC_CHECK(third.find("max_processes") != std::string::npos);
    const auto listed = router.dispatch_sync(R"({"id":7,"method":"Process.list","params":null})");
    PROC_CHECK(listed.find("\"running\":true") != std::string::npos);
    const auto unknown = router.dispatch_sync(R"({"id":8,"method":"Process.kill","params":{"id":999}})");
    PROC_CHECK(unknown.find("unknown id") != std::string::npos);
    const auto bad_json = router.dispatch_sync(R"({"id":9,"method":"Process.send","params":{"id":1,"message":"{not json"}})");
    PROC_CHECK(bad_json.find("error") != std::string::npos);
    const auto ts = boson::ipc::TsGenerator::generate_with_events<boson::ipc::TsGenerator::EventMaps<boson::ipc::ProcessEvents>,
                                                                  boson::ipc::ProcessController>();
    PROC_CHECK(ts.find("'Process.spawn'") != std::string::npos || ts.find("\"Process.spawn\"") != std::string::npos ||
               ts.find("Process") != std::string::npos);
    PROC_CHECK(ts.find("process:exit") != std::string::npos);
    // ~ProcessController kills the two sleepers.
}

void test_webview(const fs::path& base) {
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
        std::cout << "SKIP: no WebView2 runtime installed (end-to-end Process checks not run)\n";
        return;
    }
    CoTaskMemFree(version);
    step("WebView: Process.* through Bridge + CapabilityPolicy, process:* events reach the page");

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson process test";
    window_cfg.width = 480;
    window_cfg.height = 320;
    window_cfg.quit_on_destroy = false;
    auto window = std::make_unique<boson::core::Window>(window_cfg);
    PROC_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
    window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = (base / L"udf").wstring();
    const auto env = boson::core::Environment::create(env_cfg);
    auto host = std::make_unique<boson::core::WebViewHost>(*window, env);
    host->set_default_asset_lookup(&lookup);
    boson::core::WebViewConfig cfg;
    cfg.initial_url = "boson://app/index.html";
    bool nav_done = false;
    boson::core::WebView& wv = host->create(cfg);
    wv.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { nav_done = e.is_success; };
    PROC_CHECK(pump_until([&]() { return wv.is_ready() && nav_done; }));
    {
        bp::ProcessManager pm;
        auto policy = bp::ExecutablePolicy::create(bp::ExecutablePolicyConfig{.executables = {self_utf8()}});
        boson::ipc::ProcessController controller(pm, {.policy = policy.value_or(nullptr), .transport = nullptr, .binary_channel = "process"});
        boson::ipc::Router<boson::ipc::ProcessController> router(controller);
        boson::ipc::Bridge bridge(runner, marshal, router);
        namespace sec = boson::security;
        sec::CapabilityConfig caps;
        caps.capabilities.push_back(sec::Capability{
            .identifier = "process", .description = {}, .origins = {"boson://app"},
            .allow = {"Process.spawn", "Process.exec", "Process.write", "Process.close_stdin", "Process.kill", "Process.ack", "Process.list"},
            .deny = {"Process.send"}});
        auto caps_policy = sec::CapabilityPolicy::make_shared(caps);
        PROC_CHECK(caps_policy.has_value());
        bridge.attach(wv, {.allowed_origins = {}, .capabilities = caps_policy.value_or(nullptr)});
        connect_process_events(controller, bridge);

        PROC_CHECK(eval(wv, "window.__r = {}; window.__ev = []; chrome.webview.addEventListener('message', e => {"
                            " const d = e.data; if (d && d.type === 'event') window.__ev.push(d); else if (d) window.__r[d.id] = d; }); 1"));
        const std::string exe = js_string(self_utf8());
        PROC_CHECK(eval(wv, "chrome.webview.postMessage({id: 1, method: 'Process.spawn', params: {command: " + exe +
                                ", args: ['--child', '--cat'], output: 'line', flow_control: true}}); 1"));
        PROC_CHECK(pump_until([&]() { return contains(eval(wv, "JSON.stringify(window.__r[1] || null)"), "status"); }, 10s));
        const auto spawned = eval(wv, "JSON.stringify(window.__r[1])");
        std::cout << "  spawn: " << spawned.value_or("<none>") << '\n';
        PROC_CHECK(contains(spawned, "success"));
        // Requests run concurrently on the pool: wait for the write before closing stdin (process.ts chains them).
        PROC_CHECK(eval(wv, "chrome.webview.postMessage({id: 2, method: 'Process.write', params: {id: window.__r[1].data.id, data: 'hello\\nw\\u00f6rld\\n'}}); 1"));
        PROC_CHECK(pump_until([&]() { return contains(eval(wv, "JSON.stringify(window.__r[2] || null)"), "status"); }, 10s));
        PROC_CHECK(contains(eval(wv, "JSON.stringify(window.__r[2])"), "\\\"ok\\\":true"));
        PROC_CHECK(eval(wv, "const id = window.__r[1].data.id;"
                            "chrome.webview.postMessage({id: 3, method: 'Process.close_stdin', params: {id}});"
                            "chrome.webview.postMessage({id: 4, method: 'Process.send', params: {id, message: '1'}}); 1"));
        PROC_CHECK(pump_until([&]() { return contains(eval(wv, "JSON.stringify(window.__ev.map(e => e.event))"), "process:exit"); }, 15s));
        const auto events = eval(wv, "JSON.stringify(window.__ev.map(e => [e.event, e.data.lines || e.data.code]))");
        std::cout << "  events: " << events.value_or("<none>") << '\n';
        PROC_CHECK(contains(events, R"([\"process:stdout\",[\"hello\",\"w\u00f6rld\"]])") || contains(events, "w\\u00f6rld") ||
                   contains(events, "w\xC3\xB6rld"));
        PROC_CHECK(contains(events, R"([\"process:exit\",0])"));
        const auto order = eval(wv, "window.__ev.findIndex(e => e.event === 'process:exit') === window.__ev.length - 1");
        PROC_CHECK(order == std::optional<std::string>("true")); // exit comes after every output event
        PROC_CHECK(contains(eval(wv, "JSON.stringify(window.__r[4])"), "-32003"));

        PROC_CHECK(eval(wv, "window.__ev = []; chrome.webview.postMessage({id: 5, method: 'Process.exec', params: {command: " + exe +
                                ", args: ['--child', '--echo-args', 'a b', 'c\"d']}});"
                                "chrome.webview.postMessage({id: 6, method: 'Process.spawn', params: {command: 'C:\\\\Windows\\\\System32\\\\cmd.exe'}}); 1"));
        PROC_CHECK(pump_until([&]() { return contains(eval(wv, "JSON.stringify(window.__ev.map(e => e.event))"), "process:exec"); }, 15s));
        const auto exec_event = eval(wv, "JSON.stringify(window.__ev.find(e => e.event === 'process:exec').data)");
        std::cout << "  exec: " << exec_event.value_or("<none>") << '\n';
        PROC_CHECK(contains(exec_event, R"(a b\\u0000c\\\"d\\u0000)") || contains(exec_event, R"(\"code\":0)"));
        PROC_CHECK(eval(wv, "window.__ev.find(e => e.event === 'process:exec').data.stdout === 'a b\\u0000c\"d\\u0000'") ==
                   std::optional<std::string>("true"));
        PROC_CHECK(pump_until([&]() { return contains(eval(wv, "JSON.stringify(window.__r[6] || null)"), "status"); }, 10s));
        PROC_CHECK(contains(eval(wv, "JSON.stringify(window.__r[6])"), "permission denied"));

        // Kill a long-running child from the page.
        PROC_CHECK(eval(wv, "window.__ev = []; chrome.webview.postMessage({id: 7, method: 'Process.spawn', params: {command: " + exe +
                                ", args: ['--child', '--sleep', '30000']}}); 1"));
        PROC_CHECK(pump_until([&]() { return contains(eval(wv, "JSON.stringify(window.__r[7] || null)"), "status"); }, 10s));
        PROC_CHECK(eval(wv, "chrome.webview.postMessage({id: 8, method: 'Process.kill', params: {id: window.__r[7].data.id}}); 1"));
        PROC_CHECK(pump_until([&]() { return contains(eval(wv, "JSON.stringify(window.__ev.map(e => e.event))"), "process:exit"); }, 10s));
        const auto killed = eval(wv, "JSON.stringify(window.__ev.map(e => e.data))");
        std::cout << "  after kill: " << killed.value_or("<none>") << '\n';
        PROC_CHECK(contains(killed, R"(\"killed\":true)"));
        disconnect_process_events(controller);
        bridge.detach(wv);
    }
    bool shut = false;
    env->shutdown([&]() { shut = true; });
    pump_until([&]() { return shut; }, 10s);
    host.reset();
    window.reset();
}

std::vector<std::wstring> argv_list() {
    std::vector<std::wstring> out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        for (int i = 0; i < argc; ++i) {
            out.emplace_back(argv[i]);
        }
        LocalFree(argv);
    }
    return out;
}

} // namespace

int wmain() {
    const auto args = argv_list();
    if (args.size() >= 3 && args[1] == L"--child") {
        return run_child(std::span<const std::wstring>(args).subspan(2));
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    const fs::path base = fs::temp_directory_path() /
                          (L"boson-process-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(base);

    test_quoting(base);
    test_pipes();
    test_extra_handles();
    test_jobs();
    test_exec();
    test_policy(base);
    test_named_pipes();
    test_ports();
    test_sidecar(base);
    test_node(base);
    test_controller_headless();
    test_webview(base);

    std::error_code ec;
    for (int i = 0; i < 20; ++i) {
        fs::remove_all(base, ec);
        if (!fs::exists(base)) {
            break;
        }
        Sleep(250);
    }
    PROC_CHECK(!fs::exists(base));
    std::cout << (g_checks - g_failures) << "/" << g_checks << " process checks passed\n";
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
