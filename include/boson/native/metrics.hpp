#pragma once

#include <windows.h>

#include <boson/core/environment.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace boson::native {

/// @brief Memory / CPU of one process (Electron's ProcessMetric).
struct ProcessMetric {
    std::uint32_t pid = 0;
    std::string type;                  ///< "app" (this process) or core::to_string(ProcessKind): browser, renderer, gpu...
    std::uint64_t working_set_bytes = 0;
    std::uint64_t peak_working_set_bytes = 0;
    std::uint64_t private_bytes = 0;   ///< PROCESS_MEMORY_COUNTERS_EX2::PrivateUsage (commit charge)
    double cpu_percent = 0.0;          ///< of all logical CPUs since the previous sample (first: since start)
    std::uint64_t cpu_time_ms = 0;     ///< kernel + user since process start
    std::uint64_t created_unix_ms = 0;
    bool accessible = true;            ///< false when OpenProcess failed (exited / access denied)
};

struct AppMetrics {
    std::vector<ProcessMetric> processes;
    std::uint64_t total_working_set_bytes = 0;
    std::uint64_t total_private_bytes = 0;
    double total_cpu_percent = 0.0;
    std::uint32_t logical_processors = 0;
    std::uint64_t sample_duration_us = 0; ///< time the sample itself took
};

/// @brief PIDs + kinds of every process of the given environments (Environment::process_infos). UI thread.
[[nodiscard]] std::vector<core::ProcessInfo> collect_webview_processes(
    std::span<const std::shared_ptr<core::Environment>> environments);

/// @brief Samples this process plus a set of WebView2 processes (GetProcessMemoryInfo + GetProcessTimes).
/// Nothing runs between samples (no timer): CPU percentages are computed from the previous sample of the
/// same PID. Thread-safe; call sample() off the UI thread when the process list is long.
class MetricsSampler {
public:
    MetricsSampler();

    [[nodiscard]] AppMetrics sample(std::span<const core::ProcessInfo> webview_processes);
    /// @brief One process, without CPU delta bookkeeping (cpu_percent = average since process start).
    [[nodiscard]] static ProcessMetric measure(std::uint32_t pid, std::string type);

private:
    struct CpuMark {
        std::uint64_t cpu_100ns = 0;
        std::uint64_t wall_100ns = 0;
    };

    std::mutex m_mutex;
    std::map<std::uint32_t, CpuMark> m_marks;
    std::uint32_t m_processors = 1;
};

} // namespace boson::native
