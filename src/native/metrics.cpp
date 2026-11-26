#include <boson/native/metrics.hpp>

#include <psapi.h>

#include <algorithm>
#include <chrono>

namespace boson::native {

namespace {

[[nodiscard]] std::uint64_t to_u64(const FILETIME& ft) noexcept {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

[[nodiscard]] std::uint64_t now_filetime() noexcept {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    return to_u64(ft);
}

constexpr std::uint64_t kUnixEpochFiletime = 116444736000000000ULL;

struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h != nullptr) {
            CloseHandle(h);
        }
    }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

struct RawSample {
    ProcessMetric metric;
    std::uint64_t cpu_100ns = 0;
    std::uint64_t created_100ns = 0;
};

[[nodiscard]] RawSample read_process(std::uint32_t pid, std::string type) {
    RawSample raw;
    raw.metric.pid = pid;
    raw.metric.type = std::move(type);
    const bool self = pid == GetCurrentProcessId();
    UniqueHandle owned;
    HANDLE process = GetCurrentProcess();
    if (!self) {
        owned.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        process = owned.get();
        if (process == nullptr) {
            raw.metric.accessible = false;
            return raw;
        }
    }
    PROCESS_MEMORY_COUNTERS_EX2 counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE) {
        raw.metric.working_set_bytes = counters.WorkingSetSize;
        raw.metric.peak_working_set_bytes = counters.PeakWorkingSetSize;
        raw.metric.private_bytes = counters.PrivateUsage;
    } else {
        raw.metric.accessible = false;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE) {
        raw.cpu_100ns = to_u64(kernel) + to_u64(user);
        raw.created_100ns = to_u64(created);
        raw.metric.cpu_time_ms = raw.cpu_100ns / 10000;
        if (raw.created_100ns > kUnixEpochFiletime) {
            raw.metric.created_unix_ms = (raw.created_100ns - kUnixEpochFiletime) / 10000;
        }
    }
    return raw;
}

[[nodiscard]] std::uint32_t logical_processors() noexcept {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count == 0 ? 1U : static_cast<std::uint32_t>(count);
}

[[nodiscard]] double percent(std::uint64_t cpu, std::uint64_t wall, std::uint32_t processors) noexcept {
    if (wall == 0 || processors == 0) {
        return 0.0;
    }
    return std::clamp(100.0 * static_cast<double>(cpu) / (static_cast<double>(wall) * processors), 0.0, 100.0);
}

} // namespace

std::vector<core::ProcessInfo> collect_webview_processes(std::span<const std::shared_ptr<core::Environment>> environments) {
    std::vector<core::ProcessInfo> out;
    for (const auto& env : environments) {
        if (env == nullptr) {
            continue;
        }
        for (const core::ProcessInfo& info : env->process_infos()) {
            if (std::ranges::none_of(out, [&info](const core::ProcessInfo& p) { return p.pid == info.pid; })) {
                out.push_back(info);
            }
        }
    }
    return out;
}

MetricsSampler::MetricsSampler() : m_processors(logical_processors()) {}

ProcessMetric MetricsSampler::measure(std::uint32_t pid, std::string type) {
    RawSample raw = read_process(pid, std::move(type));
    const std::uint64_t now = now_filetime();
    if (raw.created_100ns != 0 && now > raw.created_100ns) {
        raw.metric.cpu_percent = percent(raw.cpu_100ns, now - raw.created_100ns, logical_processors());
    }
    return raw.metric;
}

AppMetrics MetricsSampler::sample(std::span<const core::ProcessInfo> webview_processes) {
    const auto started = std::chrono::steady_clock::now();
    AppMetrics metrics;
    metrics.logical_processors = m_processors;
    metrics.processes.reserve(webview_processes.size() + 1);

    std::vector<RawSample> raws;
    raws.reserve(webview_processes.size() + 1);
    raws.push_back(read_process(GetCurrentProcessId(), "app"));
    for (const core::ProcessInfo& info : webview_processes) {
        if (info.pid != 0 && info.pid != GetCurrentProcessId()) {
            raws.push_back(read_process(info.pid, std::string(core::to_string(info.kind))));
        }
    }
    const std::uint64_t now = now_filetime();

    std::scoped_lock lock(m_mutex);
    std::map<std::uint32_t, CpuMark> next;
    for (RawSample& raw : raws) {
        if (raw.metric.accessible && raw.created_100ns != 0) {
            const auto previous = m_marks.find(raw.metric.pid);
            if (previous != m_marks.end() && now > previous->second.wall_100ns && raw.cpu_100ns >= previous->second.cpu_100ns) {
                raw.metric.cpu_percent = percent(raw.cpu_100ns - previous->second.cpu_100ns, now - previous->second.wall_100ns, m_processors);
            } else if (now > raw.created_100ns) {
                raw.metric.cpu_percent = percent(raw.cpu_100ns, now - raw.created_100ns, m_processors);
            }
            next[raw.metric.pid] = CpuMark{.cpu_100ns = raw.cpu_100ns, .wall_100ns = now};
        }
        metrics.total_working_set_bytes += raw.metric.working_set_bytes;
        metrics.total_private_bytes += raw.metric.private_bytes;
        metrics.total_cpu_percent += raw.metric.cpu_percent;
        metrics.processes.push_back(std::move(raw.metric));
    }
    m_marks = std::move(next); // exited PIDs drop out
    metrics.sample_duration_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    return metrics;
}

} // namespace boson::native
