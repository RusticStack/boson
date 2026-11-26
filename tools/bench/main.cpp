// boson_bench: launches any desktop app (Boson, Electron, Tauri, Wails, ...), follows its whole process tree
// (parent-PID walk + job object, so msedgewebview2.exe / renderer / GPU children are included) and records
// startup, idle and optional load metrics. Writes <out>/<name>.json and <out>/<name>.md.
//
//   boson_bench --exe build\wt\boson_bench_app.exe --name boson_bench_app --wait-ready --load-signal
//   boson_bench --exe "C:\path\MyElectronApp.exe" --name electron --idle 30 -- --some-app-flag
//   boson_bench --compare bench-results\a.json bench-results\b.json [--out table.md]
//   boson_bench --scenario browser --target boson=build\rel\boson_browser.exe --target wails=bench\wails_browser\bin\wails_browser.exe
//               (multi-tab browser comparison, see tools/bench/scenario.hpp and docs/BENCHMARKS.md)
#include <windows.h>
#include <objbase.h>
#include <WebView2.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>

#include "bench_core.hpp"
#include "scenario.hpp"

namespace fs = std::filesystem;
using namespace boson::bench;

namespace {

struct Options {
    BenchConfig config;
    fs::path exe;
    std::vector<std::wstring> args;
    fs::path out_dir = "bench-results";
    std::uint32_t runs = 1;
    double startup_timeout_s = 60.0;
    double load_timeout_s = 300.0;
    double cooldown_s = 5.0;
    double between_runs_s = 3.0;
    bool keep_samples = true;
    std::vector<fs::path> compare;
    std::optional<fs::path> compare_out;
};

constexpr std::wstring_view kUsage =
    L"usage:\n"
    L"  boson_bench --exe <path> [--name <label>] [--out <dir>] [--runs N] [--idle S] [--settle S]\n"
    L"              [--interval MS] [--startup-timeout S] [--wait-ready] [--load-signal]\n"
    L"              [--load-command \"<cmd line>\"] [--load-timeout S] [--cooldown S] [--no-samples]\n"
    L"              [--arg <a>]... [-- <target args>...]\n"
    L"  boson_bench --compare <report.json>... [--out <file.md>]\n";

[[nodiscard]] std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    }
    return out;
}

[[nodiscard]] std::optional<double> to_number(std::wstring_view text) {
    try {
        std::size_t used = 0;
        const double value = std::stod(std::wstring{text}, &used);
        if (used != text.size() || value < 0.0) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<Options> parse(int argc, wchar_t** argv) {
    Options o;
    bool compare_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a{argv[i]};
        const auto next = [&]() -> std::optional<std::wstring_view> {
            if (i + 1 >= argc) {
                return std::nullopt;
            }
            return std::wstring_view{argv[++i]};
        };
        const auto next_number = [&](double& dst) {
            const auto v = next();
            const auto n = v ? to_number(*v) : std::nullopt;
            if (!n) {
                return false;
            }
            dst = *n;
            return true;
        };
        if (a == L"--") {
            for (++i; i < argc; ++i) {
                o.args.emplace_back(argv[i]);
            }
            break;
        }
        if (a == L"--exe") {
            const auto v = next();
            if (!v) return std::nullopt;
            o.exe = fs::path{std::wstring{*v}};
        } else if (a == L"--name") {
            const auto v = next();
            if (!v) return std::nullopt;
            o.config.name = narrow(*v);
        } else if (a == L"--out") {
            const auto v = next();
            if (!v) return std::nullopt;
            o.out_dir = fs::path{std::wstring{*v}};
            o.compare_out = o.out_dir;
        } else if (a == L"--arg") {
            const auto v = next();
            if (!v) return std::nullopt;
            o.args.emplace_back(*v);
        } else if (a == L"--runs") {
            double n = 0;
            if (!next_number(n) || n < 1) return std::nullopt;
            o.runs = static_cast<std::uint32_t>(n);
        } else if (a == L"--idle") {
            if (!next_number(o.config.idle_s)) return std::nullopt;
        } else if (a == L"--settle") {
            if (!next_number(o.config.settle_s)) return std::nullopt;
        } else if (a == L"--interval") {
            if (!next_number(o.config.interval_ms) || o.config.interval_ms < 10) return std::nullopt;
        } else if (a == L"--startup-timeout") {
            if (!next_number(o.startup_timeout_s)) return std::nullopt;
        } else if (a == L"--load-timeout") {
            if (!next_number(o.load_timeout_s)) return std::nullopt;
        } else if (a == L"--cooldown") {
            if (!next_number(o.cooldown_s)) return std::nullopt;
        } else if (a == L"--wait-ready") {
            o.config.wait_ready = true;
        } else if (a == L"--load-signal") {
            o.config.load_signal = true;
        } else if (a == L"--load-command") {
            const auto v = next();
            if (!v) return std::nullopt;
            o.config.load_command = narrow(*v);
        } else if (a == L"--no-samples") {
            o.keep_samples = false;
        } else if (a == L"--compare") {
            compare_mode = true;
        } else if (compare_mode && !a.starts_with(L"--")) {
            o.compare.emplace_back(std::wstring{a});
        } else {
            std::wcerr << L"unknown argument: " << a << L"\n";
            return std::nullopt;
        }
    }
    if (compare_mode) {
        if (o.compare.empty()) return std::nullopt;
        return o;
    }
    if (o.exe.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    o.exe = fs::absolute(o.exe, ec);
    if (o.config.name.empty()) {
        o.config.name = narrow(o.exe.stem().wstring());
    }
    o.config.target = narrow(o.exe.wstring());
    for (const auto& arg : o.args) {
        o.config.args.push_back(narrow(arg));
    }
    return o;
}

/// CommandLineToArgvW-compatible quoting.
[[nodiscard]] std::wstring quote_arg(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring{arg};
    }
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out.push_back(ch);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

[[nodiscard]] std::string registry_string(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buffer[256]{};
    DWORD bytes = sizeof(buffer);
    if (::RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return narrow(buffer);
}

[[nodiscard]] MachineInfo machine_info() {
    MachineInfo info;
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    info.logical_cpus = si.dwNumberOfProcessors;
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (::GlobalMemoryStatusEx(&mem) != FALSE) {
        info.total_ram_mb = mem.ullTotalPhys / (1024ull * 1024ull);
    }
    info.cpu = registry_string(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    while (!info.cpu.empty() && info.cpu.back() == ' ') {
        info.cpu.pop_back();
    }

    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    std::string os = "Windows";
    if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
        const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
        OSVERSIONINFOW ver{};
        ver.dwOSVersionInfoSize = sizeof(ver);
        if (rtl_get_version != nullptr && rtl_get_version(&ver) == 0) {
            os += " " + std::to_string(ver.dwMajorVersion) + "." + std::to_string(ver.dwMinorVersion) + "." + std::to_string(ver.dwBuildNumber);
        }
    }
    const std::string display = registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
    if (!display.empty()) {
        os += " (" + display + ")";
    }
    info.os = os;

    LPWSTR version = nullptr;
    if (SUCCEEDED(::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) && version != nullptr) {
        info.webview2_runtime = narrow(version);
        ::CoTaskMemFree(version);
    }
    return info;
}

[[nodiscard]] std::string iso_timestamp() {
    SYSTEMTIME st{};
    ::GetSystemTime(&st);
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

struct WindowSearch {
    const std::vector<DWORD>* pids = nullptr;
    bool visible_only = true;
    std::vector<HWND> found;
};

BOOL CALLBACK collect_windows(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (std::ranges::find(*search->pids, pid) == search->pids->end()) {
        return TRUE;
    }
    if (search->visible_only) {
        if (::IsWindowVisible(hwnd) == FALSE) {
            return TRUE;
        }
        RECT rc{};
        if (::GetWindowRect(hwnd, &rc) == FALSE || rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0) {
            return TRUE;
        }
    }
    search->found.push_back(hwnd);
    return TRUE;
}

[[nodiscard]] std::vector<HWND> top_level_windows(const std::vector<DWORD>& pids, bool visible_only) {
    WindowSearch search{.pids = &pids, .visible_only = visible_only, .found = {}};
    ::EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

[[nodiscard]] std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct LaunchedTree {
    UniqueHandle job;
    UniqueHandle process;
    DWORD root_pid = 0;
    LONGLONG t0 = 0; ///< QPC just before CreateProcessW
    std::string error;
};

/// @brief Starts exe (suspended, inside a kill-on-close job, SW_SHOWNOACTIVATE) with BOSON_BENCH_DIR=bench_dir.
[[nodiscard]] LaunchedTree launch_tree(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& bench_dir) {
    LaunchedTree out;
    out.job.reset(::CreateJobObjectW(nullptr, nullptr));
    if (out.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; // nothing outlives the harness
        ::SetInformationJobObject(out.job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    ::SetEnvironmentVariableW(L"BOSON_BENCH_DIR", bench_dir.wstring().c_str());

    std::wstring cmd = quote_arg(exe.wstring());
    for (const auto& arg : args) {
        cmd += L' ';
        cmd += quote_arg(arg);
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE; // never steal focus from the user / other tests
    PROCESS_INFORMATION pi{};
    const std::wstring cwd = exe.parent_path().wstring();
    out.t0 = qpc_now();
    const BOOL ok = ::CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi);
    ::SetEnvironmentVariableW(L"BOSON_BENCH_DIR", nullptr);
    if (ok == FALSE) {
        out.error = "CreateProcessW failed with error " + std::to_string(::GetLastError());
        return out;
    }
    out.process.reset(pi.hProcess);
    const UniqueHandle thread{pi.hThread};
    if (out.job && ::AssignProcessToJobObject(out.job.get(), pi.hProcess) == FALSE) {
        std::cerr << "warning: AssignProcessToJobObject failed (" << ::GetLastError() << "); relying on the parent-PID walk\n";
    }
    ::ResumeThread(thread.get());
    out.root_pid = pi.dwProcessId;
    return out;
}

struct CloseOutcome {
    std::optional<double> shutdown_ms;
    bool forced_kill = false;
};

/// @brief WM_CLOSE to the root's windows, then waits up to 20 s for the whole tree to exit (killed after that).
CloseOutcome close_tree(ProcessTreeSampler& sampler, DWORD root_pid, HANDLE job, LONGLONG t0, const std::function<void(Sample)>& on_sample) {
    CloseOutcome out;
    const auto now_ms = [t0]() { return qpc_ms(t0, qpc_now()); };
    const double start = now_ms();
    std::vector<DWORD> root_only{root_pid};
    auto windows = top_level_windows(root_only, true);
    if (windows.empty()) {
        windows = top_level_windows(sampler.live_pids(), true);
    }
    for (HWND hwnd : windows) {
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    const double deadline = start + 20'000.0;
    while (now_ms() < deadline) {
        on_sample(sampler.sample(now_ms(), "shutdown"));
        if (sampler.live_pids().empty()) {
            out.shutdown_ms = now_ms() - start;
            return out;
        }
        ::Sleep(50);
    }
    out.forced_kill = true;
    if (job != nullptr) {
        ::TerminateJobObject(job, 1);
    }
    for (const DWORD pid : sampler.live_pids()) { // processes that broke away from the job
        const UniqueHandle process{::OpenProcess(PROCESS_TERMINATE, FALSE, pid)};
        if (process) {
            ::TerminateProcess(process.get(), 1);
        }
    }
    on_sample(sampler.sample(now_ms(), "shutdown"));
    return out;
}

/// The app writes "<QueryPerformanceCounter value>" into BOSON_BENCH_DIR/<name> (ready, shell_paint, first_nav).
[[nodiscard]] std::optional<LONGLONG> read_stamp(const fs::path& dir, std::wstring_view name) {
    const auto text = read_file(dir / name);
    if (!text || text->empty()) {
        return std::nullopt;
    }
    try {
        return std::stoll(*text);
    } catch (...) {
        return std::nullopt;
    }
}

/// The app writes "<QueryPerformanceCounter value>" into BOSON_BENCH_DIR/ready once its first frame is up.
[[nodiscard]] std::optional<LONGLONG> read_ready_stamp(const fs::path& dir) { return read_stamp(dir, L"ready"); }

class Run {
public:
    Run(const Options& options, std::uint32_t index, std::uint32_t logical_cpus)
        : m_options(options), m_logical_cpus(logical_cpus) {
        m_result.run = index;
        m_dir = fs::temp_directory_path() / ("boson_bench_" + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(index));
    }

    RunResult execute() {
        std::error_code ec;
        fs::remove_all(m_dir, ec);
        fs::create_directories(m_dir, ec);
        if (!launch()) {
            cleanup_dir();
            return std::move(m_result);
        }
        startup();
        if (m_result.error.empty()) {
            timed_phase("settle", m_options.config.settle_s);
            timed_phase("idle", m_options.config.idle_s);
            m_result.processes_after_idle = m_sampler->processes();
            load();
        }
        shutdown();
        for (const char* phase : {"startup", "settle", "idle", "load", "cooldown", "shutdown"}) {
            PhaseSummary summary = summarize_phase(m_result.samples, phase, m_logical_cpus);
            if (summary.samples > 0) {
                m_result.phases.push_back(std::move(summary));
            }
        }
        if (!m_options.keep_samples) {
            m_result.samples.clear();
        }
        cleanup_dir();
        return std::move(m_result);
    }

private:
    [[nodiscard]] double now_ms() const noexcept { return qpc_ms(m_t0, qpc_now()); }

    void cleanup_dir() {
        std::error_code ec;
        fs::remove_all(m_dir, ec);
    }

    bool launch() {
        LaunchedTree tree = launch_tree(m_options.exe, m_options.args, m_dir);
        if (!tree.error.empty()) {
            m_result.error = std::move(tree.error);
            return false;
        }
        m_job = std::move(tree.job);
        m_process = std::move(tree.process);
        m_root_pid = tree.root_pid;
        m_t0 = tree.t0;
        m_sampler.emplace(m_root_pid, m_job.get());
        return true;
    }

    void take_sample(std::string_view phase) {
        m_result.samples.push_back(m_sampler->sample(now_ms(), phase));
        poll_ready();
    }

    void poll_ready() {
        if (m_result.ready_ms.has_value()) {
            return;
        }
        if (const auto stamp = read_ready_stamp(m_dir)) {
            m_result.ready_ms = qpc_ms(m_t0, *stamp);
        }
    }

    [[nodiscard]] bool tree_alive() const { return !m_sampler->live_pids().empty(); }

    void startup() {
        const double deadline = m_options.startup_timeout_s * 1000.0;
        double next_sample = 0.0;
        while (true) {
            const double t = now_ms();
            if (t >= next_sample) {
                take_sample("startup");
                next_sample = t + 100.0;
                if (!tree_alive()) {
                    DWORD code = 0;
                    ::GetExitCodeProcess(m_process.get(), &code);
                    m_result.error = "target exited during startup (exit code " + std::to_string(code) + ")";
                    return;
                }
            }
            if (!m_result.first_window_ms.has_value()) {
                if (!top_level_windows(m_sampler->live_pids(), true).empty()) {
                    m_result.first_window_ms = now_ms();
                }
            }
            poll_ready();
            const bool ready_ok = !m_options.config.wait_ready || m_result.ready_ms.has_value();
            if (m_result.first_window_ms.has_value() && ready_ok) {
                take_sample("startup");
                return;
            }
            if (t >= deadline) {
                m_result.error = m_result.first_window_ms ? "timed out waiting for the ready signal" : "timed out waiting for a visible window";
                return;
            }
            ::Sleep(5);
        }
    }

    template <typename Done>
    void sample_until(std::string_view phase, double max_ms, Done done) {
        const double start = now_ms();
        take_sample(phase);
        while (true) {
            const double target = now_ms() + m_options.config.interval_ms;
            while (now_ms() < target) {
                if (done()) {
                    take_sample(phase);
                    return;
                }
                ::Sleep(static_cast<DWORD>(std::min(50.0, std::max(1.0, target - now_ms()))));
            }
            take_sample(phase);
            if (now_ms() - start >= max_ms || !tree_alive()) {
                return;
            }
        }
    }

    void timed_phase(std::string_view phase, double seconds) {
        if (seconds <= 0.0) {
            return;
        }
        const double end = now_ms() + seconds * 1000.0;
        sample_until(phase, seconds * 1000.0, [&]() { return now_ms() >= end; });
    }

    void load() {
        const bool has_command = !m_options.config.load_command.empty();
        if (!m_options.config.load_signal && !has_command) {
            return;
        }
        const double start = now_ms();
        if (m_options.config.load_signal) {
            const UINT message = ::RegisterWindowMessageW(L"BosonBench.StartLoad");
            const auto windows = top_level_windows(m_sampler->live_pids(), false);
            for (HWND hwnd : windows) {
                ::PostMessageW(hwnd, message, 0, 0);
            }
            const fs::path done_file = m_dir / "load_done";
            sample_until("load", m_options.load_timeout_s * 1000.0, [&]() {
                std::error_code ec;
                return fs::exists(done_file, ec);
            });
            std::error_code ec;
            if (!fs::exists(done_file, ec)) {
                m_result.error = "load phase timed out (no load_done from the app)";
            }
        }
        if (has_command) {
            std::wstring cmd = L"cmd.exe /d /c " + widen(m_options.config.load_command);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE) {
                const UniqueHandle process{pi.hProcess};
                const UniqueHandle thread{pi.hThread};
                sample_until("load", m_options.load_timeout_s * 1000.0, [&]() {
                    return ::WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0;
                });
                if (::WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0) {
                    ::TerminateProcess(process.get(), 1);
                    m_result.error = "load command timed out";
                }
            } else {
                m_result.error = "load command could not be started (" + std::to_string(::GetLastError()) + ")";
            }
        }
        m_result.load_ms = now_ms() - start;
        if (const auto text = read_file(m_dir / "load_result.json"); text && !glz::validate_json(*text)) {
            m_result.load_result.str = *text;
        }
        timed_phase("cooldown", m_options.cooldown_s);
    }

    void shutdown() {
        if (!m_sampler) {
            return;
        }
        const CloseOutcome outcome = close_tree(*m_sampler, m_root_pid, m_job.get(), m_t0, [this](Sample s) { m_result.samples.push_back(std::move(s)); });
        m_result.shutdown_ms = outcome.shutdown_ms;
        m_result.forced_kill = outcome.forced_kill;
    }

    const Options& m_options;
    std::uint32_t m_logical_cpus;
    RunResult m_result;
    fs::path m_dir;
    UniqueHandle m_job;
    UniqueHandle m_process;
    DWORD m_root_pid = 0;
    LONGLONG m_t0 = 0;
    std::optional<ProcessTreeSampler> m_sampler;
};

int compare(const Options& options) {
    std::vector<BenchReport> reports;
    for (const auto& path : options.compare) {
        const auto text = read_file(path);
        BenchReport report;
        if (!text || glz::read<glz::opts{.error_on_unknown_keys = false}>(report, *text)) {
            std::wcerr << L"cannot read report " << path.wstring() << L"\n";
            return 2;
        }
        reports.push_back(std::move(report));
    }
    const std::string md = render_comparison(reports);
    std::cout << md;
    if (options.compare_out) {
        std::ofstream out(*options.compare_out, std::ios::binary);
        out << md;
    }
    return 0;
}


// ---- --scenario browser ------------------------------------------------------------------------------------

namespace scn = boson::bench::scenario;

struct ScenarioTarget {
    std::string name;
    fs::path exe;
    std::vector<std::wstring> args;
};

struct ScenarioOptions {
    std::vector<ScenarioTarget> targets;
    fs::path out_dir = "bench-results";
    std::uint32_t runs = 3;
    std::uint32_t warmup = 1;
    double settle_s = 2.0;
    double run_timeout_s = 300.0;
    double between_runs_s = 3.0;
    std::vector<fs::path> compare;
    std::optional<fs::path> compare_out;
};

constexpr std::wstring_view kScenarioUsage =
    L"  boson_bench --scenario browser --target <name>=<exe> [--target ...] [--target-arg <name>=<arg>]...\n"
    L"              [--runs N] [--warmup N] [--out <dir>] [--checkpoint-settle S] [--run-timeout S] [--cooldown S]\n"
    L"  boson_bench --compare-scenario <report.scenario.json>... [--out <file.md>]\n";

[[nodiscard]] std::optional<ScenarioOptions> parse_scenario(int argc, wchar_t** argv) {
    ScenarioOptions o;
    bool compare_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a{argv[i]};
        const auto next = [&]() -> std::optional<std::wstring_view> {
            if (i + 1 >= argc) return std::nullopt;
            return std::wstring_view{argv[++i]};
        };
        const auto next_number = [&](double& dst) {
            const auto v = next();
            const auto n = v ? to_number(*v) : std::nullopt;
            if (!n) return false;
            dst = *n;
            return true;
        };
        const auto split = [](std::wstring_view v) -> std::optional<std::pair<std::wstring, std::wstring>> {
            const std::size_t eq = v.find(L'=');
            if (eq == std::wstring_view::npos || eq == 0) return std::nullopt;
            return std::pair{std::wstring{v.substr(0, eq)}, std::wstring{v.substr(eq + 1)}};
        };
        if (a == L"--scenario") {
            const auto v = next();
            if (!v || *v != L"browser") return std::nullopt;
        } else if (a == L"--compare-scenario") {
            compare_mode = true;
        } else if (a == L"--target") {
            const auto v = next();
            const auto kv = v ? split(*v) : std::nullopt;
            if (!kv) return std::nullopt;
            std::error_code ec;
            o.targets.push_back(ScenarioTarget{.name = narrow(kv->first), .exe = fs::absolute(fs::path{kv->second}, ec), .args = {}});
        } else if (a == L"--target-arg") {
            const auto v = next();
            const auto kv = v ? split(*v) : std::nullopt;
            if (!kv) return std::nullopt;
            const std::string name = narrow(kv->first);
            const auto it = std::ranges::find_if(o.targets, [&](const ScenarioTarget& t) { return t.name == name; });
            if (it == o.targets.end()) return std::nullopt;
            it->args.push_back(kv->second);
        } else if (a == L"--out") {
            const auto v = next();
            if (!v) return std::nullopt;
            o.out_dir = fs::path{std::wstring{*v}};
            o.compare_out = o.out_dir;
        } else if (a == L"--runs") {
            double n = 0;
            if (!next_number(n) || n < 1) return std::nullopt;
            o.runs = static_cast<std::uint32_t>(n);
        } else if (a == L"--warmup") {
            double n = 0;
            if (!next_number(n)) return std::nullopt;
            o.warmup = static_cast<std::uint32_t>(n);
        } else if (a == L"--checkpoint-settle") {
            if (!next_number(o.settle_s)) return std::nullopt;
        } else if (a == L"--run-timeout") {
            if (!next_number(o.run_timeout_s)) return std::nullopt;
        } else if (a == L"--cooldown") {
            if (!next_number(o.between_runs_s)) return std::nullopt;
        } else if (compare_mode && !a.starts_with(L"--")) {
            o.compare.emplace_back(std::wstring{a});
        } else {
            std::wcerr << L"unknown argument: " << a << L"\n";
            return std::nullopt;
        }
    }
    if (compare_mode) {
        return o.compare.empty() ? std::nullopt : std::optional{o};
    }
    return o.targets.empty() ? std::nullopt : std::optional{o};
}

void write_text_atomically(const fs::path& path, std::string_view text) {
    fs::path tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

/// @brief One launch of a --bench app: startup stamps, memory checkpoints, the window-lag probe, result.json.
class ScenarioRun {
public:
    ScenarioRun(const ScenarioOptions& options, const ScenarioTarget& target, std::uint32_t index)
        : m_options(options), m_target(target) {
        m_result.run = index;
        m_dir = fs::temp_directory_path() / ("boson_bench_scn_" + std::to_string(::GetCurrentProcessId()) + "_" + target.name + "_" + std::to_string(index));
    }

    scn::ScenarioRunResult execute() {
        std::error_code ec;
        fs::remove_all(m_dir, ec);
        fs::create_directories(m_dir, ec);
        std::vector<std::wstring> args = m_target.args;
        args.emplace_back(L"--bench");
        LaunchedTree tree = launch_tree(m_target.exe, args, m_dir);
        if (!tree.error.empty()) {
            m_result.error = std::move(tree.error);
            return std::move(m_result);
        }
        m_job = std::move(tree.job);
        m_process = std::move(tree.process);
        m_root_pid = tree.root_pid;
        m_t0 = tree.t0;
        m_sampler.emplace(m_root_pid, m_job.get());
        drive();
        if (const auto text = read_file(m_dir / "result.json")) {
            scn::AppResult app;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(app, *text)) {
                m_result.error = m_result.error.empty() ? "result.json could not be parsed" : m_result.error;
            } else {
                m_result.app = std::move(app);
            }
        }
        if (!m_result.error.empty()) {
            if (const auto log_text = read_file(m_dir / "app.log"); log_text && !log_text->empty()) {
                const std::size_t keep = std::min<std::size_t>(log_text->size(), 600);
                m_result.error += " | app.log tail: " + log_text->substr(log_text->size() - keep);
            }
        }
        const CloseOutcome outcome = close_tree(*m_sampler, m_root_pid, m_job.get(), m_t0, [](Sample) {});
        m_result.shutdown_ms = outcome.shutdown_ms;
        m_result.forced_kill = outcome.forced_kill;
        fs::remove_all(m_dir, ec);
        return std::move(m_result);
    }

private:
    [[nodiscard]] double now_ms() const noexcept { return qpc_ms(m_t0, qpc_now()); }

    void drive() {
        std::uint64_t last_seq = 0;
        std::string last_name = "startup";
        double next_sample = 0.0;
        const double deadline = m_options.run_timeout_s * 1000.0;
        while (true) {
            const double t = now_ms();
            if (t >= next_sample) {
                static_cast<void>(m_sampler->sample(t, "run"));
                next_sample = t + (m_result.first_window_ms ? 500.0 : 20.0);
                if (m_sampler->live_pids().empty()) {
                    DWORD code = 0;
                    ::GetExitCodeProcess(m_process.get(), &code);
                    m_result.error = "target exited during '" + last_name + "' (exit code " + std::to_string(code) + ")";
                    return;
                }
            }
            if (!m_result.first_window_ms && !top_level_windows(m_sampler->live_pids(), true).empty()) {
                m_result.first_window_ms = now_ms();
            }
            if (!m_result.shell_paint_ms) {
                if (const auto stamp = read_stamp(m_dir, L"shell_paint")) m_result.shell_paint_ms = qpc_ms(m_t0, *stamp);
            }
            if (!m_result.first_nav_ms) {
                if (const auto stamp = read_stamp(m_dir, L"first_nav")) m_result.first_nav_ms = qpc_ms(m_t0, *stamp);
            }
            if (const auto text = read_file(m_dir / "checkpoint")) {
                if (const auto cp = scn::parse_checkpoint(*text); cp && cp->first > last_seq) {
                    last_seq = cp->first;
                    last_name = cp->second;
                    std::cout << "[boson_bench]   checkpoint " << last_name << "\n" << std::flush;
                    handle_checkpoint(last_name);
                    write_text_atomically(m_dir / "ack", std::to_string(last_seq));
                }
            }
            std::error_code ec;
            if (fs::exists(m_dir / "done", ec)) {
                m_result.scenario_ms = now_ms();
                return;
            }
            if (t >= deadline) {
                m_result.error = "timed out after checkpoint '" + last_name + "'";
                return;
            }
            ::Sleep(2);
        }
    }

    void handle_checkpoint(const std::string& name) {
        if (name == "window") {
            const auto text = read_file(m_dir / "hwnds");
            const auto hwnds = text ? scn::parse_hwnds(*text) : std::nullopt;
            if (!hwnds) {
                m_result.lag = scn::LagResult{.error = "no usable hwnds file"};
                return;
            }
            ::Sleep(500);
            m_result.lag = scn::probe_window_lag(hwnds->first, hwnds->second);
            return;
        }
        ::Sleep(static_cast<DWORD>(m_options.settle_s * 1000.0));
        std::vector<scn::CheckpointMemory> samples;
        for (int i = 0; i < 5; ++i) {
            static_cast<void>(m_sampler->sample(now_ms(), name));
            const std::vector<ProcessMetrics> processes = m_sampler->processes();
            samples.push_back(scn::memory_of(name, processes, m_root_pid));
            ::Sleep(250);
        }
        m_result.checkpoints.push_back(scn::median_memory(samples));
    }

    const ScenarioOptions& m_options;
    const ScenarioTarget& m_target;
    scn::ScenarioRunResult m_result;
    fs::path m_dir;
    UniqueHandle m_job;
    UniqueHandle m_process;
    DWORD m_root_pid = 0;
    LONGLONG m_t0 = 0;
    std::optional<ProcessTreeSampler> m_sampler;
};

int run_scenario(const ScenarioOptions& options) {
    const MachineInfo machine = machine_info();
    std::vector<scn::ScenarioReport> reports;
    for (const ScenarioTarget& target : options.targets) {
        scn::ScenarioReport report;
        report.timestamp = iso_timestamp();
        report.machine = machine;
        report.name = target.name;
        report.target = narrow(target.exe.wstring());
        for (const auto& a : target.args) report.args.push_back(narrow(a));
        std::error_code ec;
        const std::uintmax_t bytes = fs::file_size(target.exe, ec);
        report.exe_bytes = ec ? 0 : static_cast<std::uint64_t>(bytes);
        report.checkpoint_settle_s = options.settle_s;
        reports.push_back(std::move(report));
    }
    bool failed = false;
    const std::uint32_t total = options.warmup + options.runs;
    for (std::uint32_t i = 1; i <= total; ++i) {
        for (std::size_t t = 0; t < options.targets.size(); ++t) { // interleaved, so slow drift hits every target alike
            const ScenarioTarget& target = options.targets[t];
            const bool warmup = i <= options.warmup;
            std::cout << "[boson_bench] " << target.name << ": " << (warmup ? "warm-up " : "run ") << (warmup ? i : i - options.warmup) << "/"
                      << (warmup ? options.warmup : options.runs) << "\n" << std::flush;
            ScenarioRun run(options, target, i);
            scn::ScenarioRunResult result = run.execute();
            result.warmup = warmup;
            if (!result.error.empty()) {
                std::cerr << "[boson_bench] " << target.name << " run " << i << ": " << result.error << "\n";
                failed = failed || !warmup;
            }
            reports[t].runs.push_back(std::move(result));
            std::this_thread::sleep_for(std::chrono::duration<double>(options.between_runs_s));
        }
    }
    std::error_code ec;
    fs::create_directories(options.out_dir, ec);
    for (const scn::ScenarioReport& report : reports) {
        std::string json;
        if (glz::write<glz::opts{.prettify = true}>(report, json)) {
            std::cerr << "[boson_bench] failed to serialize " << report.name << "\n";
            return 1;
        }
        std::ofstream(options.out_dir / (report.name + ".scenario.json"), std::ios::binary) << json;
        const std::string md = scn::render_scenario_markdown(report);
        std::ofstream(options.out_dir / (report.name + ".scenario.md"), std::ios::binary) << md;
        std::cout << md << "\n";
    }
    if (reports.size() > 1) {
        const std::string table = scn::render_scenario_comparison(reports);
        std::ofstream(options.out_dir / "comparison.md", std::ios::binary) << table;
        std::cout << table << "\n";
    }
    std::cout << "[boson_bench] wrote reports to " << narrow(fs::absolute(options.out_dir, ec).wstring()) << "\n";
    return failed ? 1 : 0;
}

int compare_scenario(const ScenarioOptions& options) {
    std::vector<scn::ScenarioReport> reports;
    for (const auto& path : options.compare) {
        const auto text = read_file(path);
        scn::ScenarioReport report;
        if (!text || glz::read<glz::opts{.error_on_unknown_keys = false}>(report, *text)) {
            std::wcerr << L"cannot read report " << path.wstring() << L"\n";
            return 2;
        }
        reports.push_back(std::move(report));
    }
    const std::string md = scn::render_scenario_comparison(reports);
    std::cout << md;
    if (options.compare_out) {
        std::ofstream(*options.compare_out, std::ios::binary) << md;
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a{argv[i]};
        if (a == L"--scenario" || a == L"--compare-scenario") {
            const auto scenario = parse_scenario(argc, argv);
            if (!scenario) {
                std::wcerr << kUsage << kScenarioUsage;
                return 2;
            }
            return scenario->compare.empty() ? run_scenario(*scenario) : compare_scenario(*scenario);
        }
    }
    const auto options = parse(argc, argv);
    if (!options) {
        std::wcerr << kUsage << kScenarioUsage;
        return 2;
    }
    if (!options->compare.empty()) {
        return compare(*options);
    }

    BenchReport report;
    report.timestamp = iso_timestamp();
    report.machine = machine_info();
    report.config = options->config;

    bool failed = false;
    for (std::uint32_t i = 1; i <= options->runs; ++i) {
        std::cout << "[boson_bench] " << options->config.name << ": run " << i << "/" << options->runs << "\n" << std::flush;
        Run run(*options, i, report.machine.logical_cpus);
        report.runs.push_back(run.execute());
        if (!report.runs.back().error.empty()) {
            std::cerr << "[boson_bench] run " << i << ": " << report.runs.back().error << "\n";
            failed = true;
        }
        if (i < options->runs) {
            std::this_thread::sleep_for(std::chrono::duration<double>(options->between_runs_s));
        }
    }

    std::error_code ec;
    fs::create_directories(options->out_dir, ec);
    const fs::path json_path = options->out_dir / (options->config.name + ".json");
    const fs::path md_path = options->out_dir / (options->config.name + ".md");
    std::string json;
    if (glz::write<glz::opts{.prettify = true}>(report, json)) {
        std::cerr << "[boson_bench] failed to serialize the report\n";
        return 1;
    }
    {
        std::ofstream out(json_path, std::ios::binary);
        out << json;
    }
    const std::string md = render_markdown(report);
    {
        std::ofstream out(md_path, std::ios::binary);
        out << md;
    }
    std::cout << md << "\n[boson_bench] wrote " << narrow(json_path.wstring()) << " and " << narrow(md_path.wstring()) << "\n";
    return failed ? 1 : 0;
}
