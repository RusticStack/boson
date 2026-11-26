#pragma once

// Process-tree sampling, statistics and report rendering for boson_bench. Header-only so the unit tests
// (tests/test_porting.cpp) exercise exactly the code the harness runs. Windows 10/11 only.

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

namespace boson::bench {

// ---- Small RAII + conversion helpers --------------------------------------------------------------------

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : m_handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.m_handle, nullptr));
        }
        return *this;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (m_handle != nullptr) {
            ::CloseHandle(m_handle);
        }
        m_handle = handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }
    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_handle != nullptr; }

private:
    HANDLE m_handle = nullptr;
};

[[nodiscard]] inline std::string narrow(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
    }
    return out;
}

[[nodiscard]] inline std::uint64_t filetime_100ns(const FILETIME& ft) noexcept {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

[[nodiscard]] inline double qpc_ms(LONGLONG from, LONGLONG to) noexcept {
    LARGE_INTEGER freq{};
    ::QueryPerformanceFrequency(&freq);
    return static_cast<double>(to - from) * 1000.0 / static_cast<double>(freq.QuadPart);
}

[[nodiscard]] inline LONGLONG qpc_now() noexcept {
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    return now.QuadPart;
}

// ---- Process tree discovery -------------------------------------------------------------------------------

struct ProcessEntry {
    DWORD pid = 0;
    DWORD parent_pid = 0;
    std::string exe;
};

[[nodiscard]] inline std::vector<ProcessEntry> snapshot_processes() {
    std::vector<ProcessEntry> out;
    const UniqueHandle snap{::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (!snap) {
        return out;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = ::Process32FirstW(snap.get(), &entry); ok != FALSE; ok = ::Process32NextW(snap.get(), &entry)) {
        out.push_back(ProcessEntry{.pid = entry.th32ProcessID, .parent_pid = entry.th32ParentProcessID, .exe = narrow(entry.szExeFile)});
    }
    return out;
}

/// @brief root plus every transitive child in entries (parent-PID links). accept(parent, child) can veto a
/// link, e.g. when the child is older than the parent (the parent PID was reused). Root is always first.
[[nodiscard]] inline std::vector<DWORD> descendants(
    std::span<const ProcessEntry> entries,
    DWORD root_pid,
    const std::function<bool(DWORD parent, DWORD child)>& accept = {}) {
    std::vector<DWORD> out{root_pid};
    std::unordered_set<DWORD> seen{root_pid};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const DWORD parent = out[i];
        for (const auto& entry : entries) {
            if (entry.parent_pid == parent && entry.pid != parent && entry.pid != 0 && !seen.contains(entry.pid) &&
                (!accept || accept(parent, entry.pid))) {
                seen.insert(entry.pid);
                out.push_back(entry.pid);
            }
        }
    }
    return out;
}

/// @brief PIDs currently assigned to job (empty when job is null or the query fails).
[[nodiscard]] inline std::vector<DWORD> job_process_ids(HANDLE job) {
    std::vector<DWORD> out;
    if (job == nullptr) {
        return out;
    }
    std::vector<ULONG_PTR> buffer(256);
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
        const DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(ULONG_PTR));
        if (::QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, bytes, nullptr) == FALSE &&
            ::GetLastError() != ERROR_MORE_DATA) {
            return out;
        }
        if (list->NumberOfProcessIdsInList < list->NumberOfAssignedProcesses) {
            buffer.resize(buffer.size() * 4);
            continue;
        }
        for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
            out.push_back(static_cast<DWORD>(list->ProcessIdList[i]));
        }
        return out;
    }
    return out;
}

// ---- Metrics ----------------------------------------------------------------------------------------------

/// Same layout as PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 1809+), declared here so the harness does not
/// depend on NTDDI_VERSION. Older systems reject the size and the sampler falls back to _EX.
struct MemoryCountersEx2 {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
    SIZE_T PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};

struct ProcessMetrics {
    std::uint32_t pid = 0;
    std::uint32_t parent_pid = 0;
    std::string name;
    bool alive = true;
    std::uint64_t private_working_set = 0; ///< bytes (Task Manager "Memory" column)
    std::uint64_t working_set = 0;         ///< bytes, includes shared pages
    std::uint64_t commit = 0;              ///< private bytes (PrivateUsage)
    std::uint32_t handles = 0;
    double cpu_ms = 0.0;                   ///< kernel + user since process start
};

struct Sample {
    double t_ms = 0.0; ///< since launch
    std::string phase;
    std::uint32_t process_count = 0;
    std::uint64_t private_working_set = 0;
    std::uint64_t working_set = 0;
    std::uint64_t commit = 0;
    std::uint32_t handles = 0;
    double cpu_ms = 0.0; ///< cumulative over every process ever seen in the tree (exited ones included)
};

/// @brief Tracks a root process and all of its descendants (parent-PID walk unioned with job membership),
/// keeping a handle on each so CPU time of exited children is still accounted for.
class ProcessTreeSampler {
public:
    explicit ProcessTreeSampler(DWORD root_pid, HANDLE job = nullptr) : m_root_pid(root_pid), m_job(job) {}

    /// @brief Refreshes the tree and returns one aggregated sample.
    Sample sample(double t_ms, std::string_view phase) {
        refresh_tree();
        Sample out{.t_ms = t_ms, .phase = std::string{phase}};
        for (auto& tracked : m_tracked) {
            read_metrics(tracked);
            out.cpu_ms += tracked.metrics.cpu_ms;
            if (!tracked.metrics.alive) {
                continue;
            }
            ++out.process_count;
            out.private_working_set += tracked.metrics.private_working_set;
            out.working_set += tracked.metrics.working_set;
            out.commit += tracked.metrics.commit;
            out.handles += tracked.metrics.handles;
        }
        return out;
    }

    /// @brief Per-process metrics as of the last sample (live processes only).
    [[nodiscard]] std::vector<ProcessMetrics> processes() const {
        std::vector<ProcessMetrics> out;
        for (const auto& tracked : m_tracked) {
            if (tracked.metrics.alive) {
                out.push_back(tracked.metrics);
            }
        }
        return out;
    }

    [[nodiscard]] std::vector<DWORD> live_pids() const {
        std::vector<DWORD> out;
        for (const auto& tracked : m_tracked) {
            if (tracked.metrics.alive) {
                out.push_back(tracked.metrics.pid);
            }
        }
        return out;
    }

    [[nodiscard]] DWORD root_pid() const noexcept { return m_root_pid; }

private:
    struct Tracked {
        UniqueHandle handle;
        std::uint64_t created = 0;
        ProcessMetrics metrics;
    };

    [[nodiscard]] Tracked* find_live(DWORD pid) {
        for (auto& tracked : m_tracked) {
            if (tracked.metrics.pid == pid && tracked.metrics.alive) {
                return &tracked;
            }
        }
        return nullptr;
    }

    [[nodiscard]] static std::uint64_t creation_time(HANDLE process) noexcept {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (::GetProcessTimes(process, &created, &exited, &kernel, &user) == FALSE) {
            return 0;
        }
        return filetime_100ns(created);
    }

    void refresh_tree() {
        const std::vector<ProcessEntry> entries = snapshot_processes();
        const auto accept = [this](DWORD parent, DWORD child) {
            // A child cannot be older than its parent; that only happens when the parent PID was reused.
            const Tracked* parent_info = find_live(parent);
            if (parent_info == nullptr || parent_info->created == 0) {
                return true;
            }
            const UniqueHandle probe{::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child)};
            return !probe || creation_time(probe.get()) >= parent_info->created;
        };
        std::vector<DWORD> pids = descendants(entries, m_root_pid, accept);
        for (const DWORD pid : job_process_ids(m_job)) {
            if (std::ranges::find(pids, pid) == pids.end()) {
                pids.push_back(pid);
            }
        }
        for (const DWORD pid : pids) {
            if (find_live(pid) != nullptr) {
                continue;
            }
            UniqueHandle handle{::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid)};
            if (!handle) {
                continue; // exited between snapshot and open, or another user's process
            }
            Tracked tracked{.handle = std::move(handle), .created = 0, .metrics = {}};
            tracked.created = creation_time(tracked.handle.get());
            tracked.metrics.pid = pid;
            for (const auto& entry : entries) {
                if (entry.pid == pid) {
                    tracked.metrics.parent_pid = entry.parent_pid;
                    tracked.metrics.name = entry.exe;
                    break;
                }
            }
            m_tracked.push_back(std::move(tracked));
        }
    }

    static void read_metrics(Tracked& tracked) {
        ProcessMetrics& m = tracked.metrics;
        if (!m.alive) {
            return;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (::GetProcessTimes(tracked.handle.get(), &created, &exited, &kernel, &user) != FALSE) {
            m.cpu_ms = static_cast<double>(filetime_100ns(kernel) + filetime_100ns(user)) / 10'000.0;
        }
        if (::WaitForSingleObject(tracked.handle.get(), 0) == WAIT_OBJECT_0) {
            m.alive = false; // keep the final CPU time, drop memory
            m.private_working_set = 0;
            m.working_set = 0;
            m.commit = 0;
            m.handles = 0;
            return;
        }
        MemoryCountersEx2 ex2{};
        ex2.cb = sizeof(ex2);
        if (::GetProcessMemoryInfo(tracked.handle.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&ex2), sizeof(ex2)) != FALSE) {
            m.private_working_set = ex2.PrivateWorkingSetSize;
            m.working_set = ex2.WorkingSetSize;
            m.commit = ex2.PrivateUsage;
        } else {
            PROCESS_MEMORY_COUNTERS_EX ex{};
            ex.cb = sizeof(ex);
            if (::GetProcessMemoryInfo(tracked.handle.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&ex), sizeof(ex)) != FALSE) {
                m.working_set = ex.WorkingSetSize;
                m.commit = ex.PrivateUsage;
            }
        }
        DWORD handles = 0;
        if (::GetProcessHandleCount(tracked.handle.get(), &handles) != FALSE) {
            m.handles = handles;
        }
    }

    DWORD m_root_pid;
    HANDLE m_job;
    std::vector<Tracked> m_tracked;
};

// ---- Statistics -------------------------------------------------------------------------------------------

/// @brief Linear-interpolated percentile (p in [0,100]) of an ascending-sorted series; 0 for an empty one.
[[nodiscard]] inline double percentile_sorted(std::span<const double> sorted, double p) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const double rank = std::clamp(p, 0.0, 100.0) / 100.0 * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(rank));
    const auto hi = std::min(lo + 1, sorted.size() - 1);
    const double frac = rank - static_cast<double>(lo);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

struct SeriesStats {
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double last = 0.0;
};

[[nodiscard]] inline SeriesStats stats_of(std::vector<double> values) {
    SeriesStats out{};
    if (values.empty()) {
        return out;
    }
    out.last = values.back();
    double sum = 0.0;
    for (const double v : values) {
        sum += v;
    }
    out.mean = sum / static_cast<double>(values.size());
    std::ranges::sort(values);
    out.min = values.front();
    out.max = values.back();
    out.p50 = percentile_sorted(values, 50.0);
    out.p95 = percentile_sorted(values, 95.0);
    return out;
}

struct PhaseSummary {
    std::string name;
    double duration_ms = 0.0;
    std::uint32_t samples = 0;
    SeriesStats private_working_set_mb;
    SeriesStats working_set_mb;
    SeriesStats commit_mb;
    SeriesStats handles;
    SeriesStats process_count;
    double cpu_ms = 0.0; ///< CPU consumed by the whole tree during the phase
    double cpu_percent_of_one_core = 0.0;
    double cpu_percent_of_machine = 0.0;
};

[[nodiscard]] inline PhaseSummary summarize_phase(std::span<const Sample> samples, std::string_view phase, std::uint32_t logical_cpus) {
    PhaseSummary out{.name = std::string{phase}};
    std::vector<double> pws;
    std::vector<double> ws;
    std::vector<double> commit;
    std::vector<double> handles;
    std::vector<double> count;
    const Sample* first = nullptr;
    const Sample* last = nullptr;
    constexpr double kMb = 1024.0 * 1024.0;
    for (const auto& s : samples) {
        if (s.phase != phase) {
            continue;
        }
        if (first == nullptr) {
            first = &s;
        }
        last = &s;
        pws.push_back(static_cast<double>(s.private_working_set) / kMb);
        ws.push_back(static_cast<double>(s.working_set) / kMb);
        commit.push_back(static_cast<double>(s.commit) / kMb);
        handles.push_back(static_cast<double>(s.handles));
        count.push_back(static_cast<double>(s.process_count));
    }
    if (first == nullptr) {
        return out;
    }
    out.samples = static_cast<std::uint32_t>(pws.size());
    out.duration_ms = last->t_ms - first->t_ms;
    out.private_working_set_mb = stats_of(std::move(pws));
    out.working_set_mb = stats_of(std::move(ws));
    out.commit_mb = stats_of(std::move(commit));
    out.handles = stats_of(std::move(handles));
    out.process_count = stats_of(std::move(count));
    out.cpu_ms = last->cpu_ms - first->cpu_ms;
    if (out.duration_ms > 0.0) {
        out.cpu_percent_of_one_core = out.cpu_ms / out.duration_ms * 100.0;
        out.cpu_percent_of_machine = out.cpu_percent_of_one_core / static_cast<double>(std::max(logical_cpus, 1u));
    }
    return out;
}

// ---- Report -----------------------------------------------------------------------------------------------

struct RunResult {
    std::uint32_t run = 1;
    std::optional<double> first_window_ms; ///< first visible, non-empty top-level window of the tree
    std::optional<double> ready_ms;        ///< app-reported ready (BOSON_BENCH_DIR/ready QPC stamp)
    std::optional<double> load_ms;         ///< duration of the load phase
    std::optional<double> shutdown_ms;     ///< WM_CLOSE until every tracked process exited
    bool forced_kill = false;
    std::vector<PhaseSummary> phases;
    std::vector<ProcessMetrics> processes_after_idle;
    std::vector<Sample> samples;
    glz::raw_json load_result{"null"};
    std::string error;
};

struct MachineInfo {
    std::string os;
    std::string cpu;
    std::uint32_t logical_cpus = 0;
    std::uint64_t total_ram_mb = 0;
    std::string webview2_runtime;
};

struct BenchConfig {
    std::string name;
    std::string target;
    std::vector<std::string> args;
    double idle_s = 30.0;
    double settle_s = 3.0;
    double interval_ms = 500.0;
    bool load_signal = false;
    std::string load_command;
    bool wait_ready = false;
};

struct BenchReport {
    std::string tool = "boson_bench";
    std::string timestamp;
    MachineInfo machine;
    BenchConfig config;
    std::vector<RunResult> runs;
};

[[nodiscard]] inline const PhaseSummary* find_phase(const RunResult& run, std::string_view name) noexcept {
    for (const auto& phase : run.phases) {
        if (phase.name == name) {
            return &phase;
        }
    }
    return nullptr;
}

[[nodiscard]] inline std::string fmt(double value, int decimals = 1) {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return buf;
}

[[nodiscard]] inline std::string fmt_opt(const std::optional<double>& value, int decimals = 0) {
    return value.has_value() ? fmt(*value, decimals) : std::string{"n/a"};
}

/// @brief Median of the values; std::nullopt when there are none.
[[nodiscard]] inline std::optional<double> median_of(std::vector<double> values) {
    if (values.empty()) {
        return std::nullopt;
    }
    std::ranges::sort(values);
    return percentile_sorted(values, 50.0);
}

/// @brief One line per run plus per-phase and per-process tables of the last run.
[[nodiscard]] inline std::string render_markdown(const BenchReport& report) {
    std::string md;
    md += "# boson_bench: " + report.config.name + "\n\n";
    md += "- Target: \x60" + report.config.target + "\x60";
    for (const auto& arg : report.config.args) {
        md += " \x60" + arg + "\x60";
    }
    md += "\n- When: " + report.timestamp + "\n";
    md += "- Machine: " + report.machine.os + ", " + report.machine.cpu + " (" + std::to_string(report.machine.logical_cpus) +
          " logical CPUs), " + std::to_string(report.machine.total_ram_mb) + " MB RAM\n";
    if (!report.machine.webview2_runtime.empty()) {
        md += "- WebView2 runtime: " + report.machine.webview2_runtime + "\n";
    }
    md += "- Idle window: " + fmt(report.config.idle_s, 0) + " s after " + fmt(report.config.settle_s, 0) + " s settle, sampled every " +
          fmt(report.config.interval_ms, 0) + " ms\n\n";

    md += "| Run | First window (ms) | Ready (ms) | Idle private WS (MB, mean) | Idle commit (MB, mean) | Idle handles | Idle processes | Idle CPU (% of one core) | Load (ms) | Shutdown (ms) |\n";
    md += "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    std::vector<double> first_windows;
    std::vector<double> readies;
    std::vector<double> idle_pws;
    std::vector<double> idle_commit;
    std::vector<double> idle_cpu;
    for (const auto& run : report.runs) {
        const PhaseSummary* idle = find_phase(run, "idle");
        md += "| " + std::to_string(run.run) + " | " + fmt_opt(run.first_window_ms) + " | " + fmt_opt(run.ready_ms) + " | ";
        if (idle != nullptr && idle->samples > 0) {
            md += fmt(idle->private_working_set_mb.mean) + " | " + fmt(idle->commit_mb.mean) + " | " + fmt(idle->handles.mean, 0) + " | " +
                  fmt(idle->process_count.max, 0) + " | " + fmt(idle->cpu_percent_of_one_core, 2) + " | ";
            idle_pws.push_back(idle->private_working_set_mb.mean);
            idle_commit.push_back(idle->commit_mb.mean);
            idle_cpu.push_back(idle->cpu_percent_of_one_core);
        } else {
            md += "n/a | n/a | n/a | n/a | n/a | ";
        }
        md += fmt_opt(run.load_ms) + " | " + fmt_opt(run.shutdown_ms) + (run.forced_kill ? " (killed)" : "") + " |\n";
        if (run.first_window_ms) {
            first_windows.push_back(*run.first_window_ms);
        }
        if (run.ready_ms) {
            readies.push_back(*run.ready_ms);
        }
    }
    if (report.runs.size() > 1) {
        md += "| **median** | " + fmt_opt(median_of(first_windows)) + " | " + fmt_opt(median_of(readies)) + " | " +
              fmt_opt(median_of(idle_pws), 1) + " | " + fmt_opt(median_of(idle_commit), 1) + " | | | " + fmt_opt(median_of(idle_cpu), 2) +
              " | | |\n";
    }

    for (const auto& run : report.runs) {
        if (!run.error.empty()) {
            md += "\nRun " + std::to_string(run.run) + " error: " + run.error + "\n";
        }
    }

    if (!report.runs.empty()) {
        const RunResult& run = report.runs.back();
        md += "\n## Phases (run " + std::to_string(run.run) + ")\n\n";
        md += "| Phase | Duration (s) | Samples | Private WS MB (mean / max) | Commit MB (mean / max) | Working set MB (mean) | Handles (mean) | Processes (max) | CPU ms | CPU % one core |\n";
        md += "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
        for (const auto& p : run.phases) {
            md += "| " + p.name + " | " + fmt(p.duration_ms / 1000.0, 1) + " | " + std::to_string(p.samples) + " | " +
                  fmt(p.private_working_set_mb.mean) + " / " + fmt(p.private_working_set_mb.max) + " | " + fmt(p.commit_mb.mean) + " / " +
                  fmt(p.commit_mb.max) + " | " + fmt(p.working_set_mb.mean) + " | " + fmt(p.handles.mean, 0) + " | " +
                  fmt(p.process_count.max, 0) + " | " + fmt(p.cpu_ms, 0) + " | " + fmt(p.cpu_percent_of_one_core, 2) + " |\n";
        }
        if (!run.processes_after_idle.empty()) {
            md += "\n## Processes at the end of idle (run " + std::to_string(run.run) + ")\n\n";
            md += "| PID | Parent | Image | Private WS (MB) | Commit (MB) | Handles | CPU ms (lifetime) |\n";
            md += "|---:|---:|---|---:|---:|---:|---:|\n";
            constexpr double kMb = 1024.0 * 1024.0;
            for (const auto& p : run.processes_after_idle) {
                md += "| " + std::to_string(p.pid) + " | " + std::to_string(p.parent_pid) + " | " + p.name + " | " +
                      fmt(static_cast<double>(p.private_working_set) / kMb) + " | " + fmt(static_cast<double>(p.commit) / kMb) + " | " +
                      std::to_string(p.handles) + " | " + fmt(p.cpu_ms, 0) + " |\n";
            }
        }
    }
    for (const auto& run : report.runs) {
        if (run.load_result.str.empty() || run.load_result.str == "null") {
            continue;
        }
        std::string pretty;
        if (glz::prettify_json(run.load_result.str, pretty)) {
            pretty = run.load_result.str;
        }
        md += "\n## Load result reported by the app (run " + std::to_string(run.run) + ")\n\n\x60\x60\x60json\n" + pretty + "\n\x60\x60\x60\n";
    }
    return md;
}

/// @brief Side-by-side table of several reports (boson_bench --compare a.json b.json ...).
[[nodiscard]] inline std::string render_comparison(std::span<const BenchReport> reports) {
    std::string md;
    md += "| App | Runs | First window (ms, median) | Ready (ms, median) | Idle private WS (MB) | Idle commit (MB) | Idle handles | Processes | Idle CPU (% of one core) |\n";
    md += "|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& report : reports) {
        std::vector<double> first_windows;
        std::vector<double> readies;
        std::vector<double> pws;
        std::vector<double> commit;
        std::vector<double> handles;
        std::vector<double> procs;
        std::vector<double> cpu;
        for (const auto& run : report.runs) {
            if (run.first_window_ms) {
                first_windows.push_back(*run.first_window_ms);
            }
            if (run.ready_ms) {
                readies.push_back(*run.ready_ms);
            }
            if (const PhaseSummary* idle = find_phase(run, "idle"); idle != nullptr && idle->samples > 0) {
                pws.push_back(idle->private_working_set_mb.mean);
                commit.push_back(idle->commit_mb.mean);
                handles.push_back(idle->handles.mean);
                procs.push_back(idle->process_count.max);
                cpu.push_back(idle->cpu_percent_of_one_core);
            }
        }
        md += "| " + report.config.name + " | " + std::to_string(report.runs.size()) + " | " + fmt_opt(median_of(first_windows)) + " | " +
              fmt_opt(median_of(readies)) + " | " + fmt_opt(median_of(pws), 1) + " | " + fmt_opt(median_of(commit), 1) + " | " +
              fmt_opt(median_of(handles)) + " | " + fmt_opt(median_of(procs)) + " | " + fmt_opt(median_of(cpu), 2) + " |\n";
    }
    return md;
}

} // namespace boson::bench
