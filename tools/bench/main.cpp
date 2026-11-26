// boson_bench: launches any desktop app (Boson, Electron, Tauri, Wails, ...), follows its whole process tree
// (parent-PID walk + job object, so msedgewebview2.exe / renderer / GPU children are included) and records
// startup, idle and optional load metrics. Writes <out>/<name>.json and <out>/<name>.md.
//
//   boson_bench --exe build\wt\boson_bench_app.exe --name boson_bench_app --wait-ready --load-signal
//   boson_bench --exe "C:\path\MyElectronApp.exe" --name electron --idle 30 -- --some-app-flag
//   boson_bench --compare bench-results\a.json bench-results\b.json [--out table.md]
#include <windows.h>
#include <objbase.h>
#include <WebView2.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>

#include "bench_core.hpp"

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

/// The app writes "<QueryPerformanceCounter value>" into BOSON_BENCH_DIR/ready once its first frame is up.
[[nodiscard]] std::optional<LONGLONG> read_ready_stamp(const fs::path& dir) {
    const auto text = read_file(dir / "ready");
    if (!text || text->empty()) {
        return std::nullopt;
    }
    try {
        return std::stoll(*text);
    } catch (...) {
        return std::nullopt;
    }
}

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
        m_job.reset(::CreateJobObjectW(nullptr, nullptr));
        if (m_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; // nothing outlives the harness
            ::SetInformationJobObject(m_job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }
        ::SetEnvironmentVariableW(L"BOSON_BENCH_DIR", m_dir.wstring().c_str());

        std::wstring cmd = quote_arg(m_options.exe.wstring());
        for (const auto& arg : m_options.args) {
            cmd += L' ';
            cmd += quote_arg(arg);
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNOACTIVATE; // never steal focus from the user / other tests
        PROCESS_INFORMATION pi{};
        const std::wstring cwd = m_options.exe.parent_path().wstring();
        m_t0 = qpc_now();
        const BOOL ok = ::CreateProcessW(
            m_options.exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi);
        ::SetEnvironmentVariableW(L"BOSON_BENCH_DIR", nullptr);
        if (ok == FALSE) {
            m_result.error = "CreateProcessW failed with error " + std::to_string(::GetLastError());
            return false;
        }
        m_process.reset(pi.hProcess);
        const UniqueHandle thread{pi.hThread};
        if (m_job && ::AssignProcessToJobObject(m_job.get(), pi.hProcess) == FALSE) {
            std::cerr << "warning: AssignProcessToJobObject failed (" << ::GetLastError() << "); relying on the parent-PID walk\n";
        }
        ::ResumeThread(thread.get());
        m_root_pid = pi.dwProcessId;
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
        const double start = now_ms();
        std::vector<DWORD> root_only{m_root_pid};
        auto windows = top_level_windows(root_only, true);
        if (windows.empty()) {
            windows = top_level_windows(m_sampler->live_pids(), true);
        }
        for (HWND hwnd : windows) {
            ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        const double deadline = start + 20'000.0;
        while (now_ms() < deadline) {
            m_result.samples.push_back(m_sampler->sample(now_ms(), "shutdown"));
            if (!tree_alive()) {
                m_result.shutdown_ms = now_ms() - start;
                return;
            }
            ::Sleep(50);
        }
        m_result.forced_kill = true;
        if (m_job) {
            ::TerminateJobObject(m_job.get(), 1);
        }
        for (const DWORD pid : m_sampler->live_pids()) { // processes that broke away from the job
            const UniqueHandle process{::OpenProcess(PROCESS_TERMINATE, FALSE, pid)};
            if (process) {
                ::TerminateProcess(process.get(), 1);
            }
        }
        m_result.samples.push_back(m_sampler->sample(now_ms(), "shutdown"));
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

} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto options = parse(argc, argv);
    if (!options) {
        std::wcerr << kUsage;
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
