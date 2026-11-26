#pragma once

// boson_bench --scenario browser: data model, window-lag probe, memory reduction and report rendering for the
// multi-tab browser comparison (boson_browser --bench vs bench/wails_browser --bench). Header-only so the unit
// tests (tests/test_porting.cpp) exercise the code the harness runs. The protocol is described in
// examples/browser/bench_driver.hpp and docs/BENCHMARKS.md.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

#include "bench_core.hpp"

namespace boson::bench::scenario {

// ---- What the app reports (result.json, same schema for every framework) ---------------------------------

struct LatencyStats {
    std::uint32_t n = 0;
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
};

struct Throughput {
    std::uint32_t n = 0;
    double total_ms = 0.0;
    double calls_per_s = 0.0;
};

struct EchoStats {
    std::vector<double> runs_ms;
    double median_ms = 0.0;
    double mb_per_s = 0.0;
};

struct EmitStats {
    std::uint32_t n = 0;
    std::uint32_t received = 0;
    double total_ms = 0.0;
    double events_per_s = 0.0;
    bool timeout = false;
};

struct IpcResult {
    bool ok = false;
    std::string error;
    LatencyStats ping_sequential;
    Throughput ping_concurrent;
    EchoStats echo_1mib;
    EmitStats emit_burst;
};

struct AppResult {
    std::string framework;
    std::string sleep_mode;
    std::optional<IpcResult> ipc;
    std::vector<double> new_tab_nav_ms;
    std::vector<double> tab_switch_ms;
    std::vector<double> wake_ms;
    std::vector<double> recreate_nav_ms;
    std::vector<double> recreate_visible_ms;
    std::uint32_t slept_at_5 = 0;
    std::uint32_t slept_at_10 = 0;
    std::vector<std::string> errors;
};

// ---- What the harness measures ------------------------------------------------------------------------------

struct CheckpointMemory {
    std::string name;
    std::uint32_t processes = 0;
    double host_commit_mb = 0.0;       ///< host process private bytes (PrivateUsage)
    double host_private_ws_mb = 0.0;
    double webview_ws_mb = 0.0;        ///< working set summed over every non-host process of the tree (WebView2)
    double webview_private_ws_mb = 0.0;
    double tree_private_ws_mb = 0.0;   ///< host + WebView2, private working set (Task Manager "Memory")
    double tree_commit_mb = 0.0;
};

struct LagResult {
    std::vector<double> move_ms;       ///< SetWindowPos(frame) -> content rect aligned again, one move at a time
    std::vector<double> resize_ms;     ///< same for size changes
    double drag_in_sync_pct = 0.0;     ///< 60 Hz moves: frames at which the content was where it belongs
    double drag_catchup_ms = 0.0;      ///< after the last drag step
    double live_resize_in_sync_pct = 0.0;
    double live_resize_catchup_ms = 0.0;
    std::uint32_t timeouts = 0;
    std::string error;
};

struct ScenarioRunResult {
    std::uint32_t run = 0;
    bool warmup = false;
    std::string error;
    std::optional<double> first_window_ms;
    std::optional<double> shell_paint_ms; ///< launch -> first shell frame (double rAF in the shell)
    std::optional<double> first_nav_ms;   ///< launch -> NavigationCompleted of the first tab
    std::optional<double> scenario_ms;    ///< launch -> done
    std::optional<double> shutdown_ms;
    bool forced_kill = false;
    std::vector<CheckpointMemory> checkpoints;
    std::optional<LagResult> lag;
    std::optional<AppResult> app;
};

struct ScenarioReport {
    std::string tool = "boson_bench --scenario browser";
    std::string timestamp;
    MachineInfo machine;
    std::string name;
    std::string target;
    std::vector<std::string> args;
    std::uint64_t exe_bytes = 0;
    double checkpoint_settle_s = 2.0;
    std::vector<ScenarioRunResult> runs;
};

// ---- Protocol helpers -----------------------------------------------------------------------------------

/// @brief "<seq> <name>" (the app's checkpoint file).
[[nodiscard]] inline std::optional<std::pair<std::uint64_t, std::string>> parse_checkpoint(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    const std::size_t space = text.find(' ');
    if (space == std::string_view::npos || space == 0 || space + 1 >= text.size()) {
        return std::nullopt;
    }
    std::uint64_t seq = 0;
    for (const char c : text.substr(0, space)) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        seq = seq * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return std::pair{seq, std::string{text.substr(space + 1)}};
}

/// @brief "<frame hwnd> <content hwnd>" as decimal integers.
[[nodiscard]] inline std::optional<std::pair<HWND, HWND>> parse_hwnds(std::string_view text) {
    std::uint64_t values[2]{};
    std::size_t index = 0;
    bool in_number = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            if (index >= 2) {
                return std::nullopt;
            }
            values[index] = values[index] * 10 + static_cast<std::uint64_t>(c - '0');
            in_number = true;
        } else if (in_number) {
            ++index;
            in_number = false;
        }
    }
    if (in_number) {
        ++index;
    }
    if (index != 2 || values[0] == 0 || values[1] == 0) {
        return std::nullopt;
    }
    return std::pair{reinterpret_cast<HWND>(static_cast<std::uintptr_t>(values[0])), reinterpret_cast<HWND>(static_cast<std::uintptr_t>(values[1]))};
}

// ---- Memory ---------------------------------------------------------------------------------------------

[[nodiscard]] inline CheckpointMemory memory_of(std::string_view name, std::span<const ProcessMetrics> processes, std::uint32_t root_pid) {
    constexpr double kMb = 1024.0 * 1024.0;
    CheckpointMemory out{.name = std::string{name}};
    for (const ProcessMetrics& p : processes) {
        if (!p.alive) {
            continue;
        }
        ++out.processes;
        out.tree_private_ws_mb += static_cast<double>(p.private_working_set) / kMb;
        out.tree_commit_mb += static_cast<double>(p.commit) / kMb;
        if (p.pid == root_pid) {
            out.host_commit_mb = static_cast<double>(p.commit) / kMb;
            out.host_private_ws_mb = static_cast<double>(p.private_working_set) / kMb;
        } else {
            out.webview_ws_mb += static_cast<double>(p.working_set) / kMb;
            out.webview_private_ws_mb += static_cast<double>(p.private_working_set) / kMb;
        }
    }
    return out;
}

/// @brief Field-wise median of several samples of the same checkpoint.
[[nodiscard]] inline CheckpointMemory median_memory(std::span<const CheckpointMemory> samples) {
    CheckpointMemory out;
    if (samples.empty()) {
        return out;
    }
    out.name = samples.front().name;
    const auto med = [&](auto field) {
        std::vector<double> values;
        for (const CheckpointMemory& s : samples) {
            values.push_back(static_cast<double>(s.*field));
        }
        return median_of(std::move(values)).value_or(0.0);
    };
    out.processes = static_cast<std::uint32_t>(med(&CheckpointMemory::processes) + 0.5);
    out.host_commit_mb = med(&CheckpointMemory::host_commit_mb);
    out.host_private_ws_mb = med(&CheckpointMemory::host_private_ws_mb);
    out.webview_ws_mb = med(&CheckpointMemory::webview_ws_mb);
    out.webview_private_ws_mb = med(&CheckpointMemory::webview_private_ws_mb);
    out.tree_private_ws_mb = med(&CheckpointMemory::tree_private_ws_mb);
    out.tree_commit_mb = med(&CheckpointMemory::tree_commit_mb);
    return out;
}

// ---- Window lag probe -------------------------------------------------------------------------------------

struct RectOffset {
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
};

[[nodiscard]] inline RectOffset offset_between(const RECT& frame, const RECT& content) noexcept {
    return RectOffset{content.left - frame.left, content.top - frame.top, content.right - frame.right, content.bottom - frame.bottom};
}

/// @brief True when content sits where the baseline offset says it belongs relative to frame (+-tolerance px).
[[nodiscard]] inline bool aligned(const RECT& frame, const RECT& content, const RectOffset& offset, long tolerance = 1) noexcept {
    const auto close_to = [tolerance](long a, long b) { return a - b <= tolerance && b - a <= tolerance; };
    return close_to(content.left, frame.left + offset.left) && close_to(content.top, frame.top + offset.top) &&
           close_to(content.right, frame.right + offset.right) && close_to(content.bottom, frame.bottom + offset.bottom);
}

struct LagProbeConfig {
    std::uint32_t moves = 20;
    long move_px = 48;
    std::uint32_t resizes = 12;
    long resize_px = 64;
    std::uint32_t live_steps = 60;
    long live_move_px = 6;
    long live_resize_px = 4;
    double frame_ms = 1000.0 / 60.0;
    double gap_ms = 150.0;
    double timeout_ms = 1000.0;
};

/// @brief Moves and resizes frame (another process's window, or this thread's for tests) with SetWindowPos and
/// measures how long content's window rect takes to follow. A child HWND follows inside the SetWindowPos call;
/// an owned top-level window follows only when the app repositions it. Restores the original rect at the end.
[[nodiscard]] inline LagResult probe_window_lag(HWND frame, HWND content, const LagProbeConfig& cfg = {}) {
    LagResult out;
    RECT f0{};
    RECT c0{};
    if (IsWindow(frame) == FALSE || IsWindow(content) == FALSE || GetWindowRect(frame, &f0) == FALSE || GetWindowRect(content, &c0) == FALSE) {
        out.error = "window-lag probe: invalid frame or content window";
        return out;
    }
    const RectOffset offset = offset_between(f0, c0);
    const auto is_aligned = [&]() {
        RECT f{};
        RECT c{};
        return GetWindowRect(frame, &f) != FALSE && GetWindowRect(content, &c) != FALSE && aligned(f, c, offset);
    };
    const auto wait_aligned = [&](LONGLONG t0) -> std::optional<double> {
        while (true) {
            if (is_aligned()) {
                return qpc_ms(t0, qpc_now());
            }
            if (qpc_ms(t0, qpc_now()) > cfg.timeout_ms) {
                return std::nullopt;
            }
            YieldProcessor();
        }
    };
    const auto place = [&](const RECT& r, UINT flags) {
        SetWindowPos(frame, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, flags | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    };
    const auto spin_until = [](LONGLONG deadline) {
        while (qpc_now() < deadline) {
            YieldProcessor();
        }
    };
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    const auto ticks = [&](double ms) { return static_cast<LONGLONG>(ms * static_cast<double>(freq.QuadPart) / 1000.0); };

    const auto discrete = [&](std::uint32_t count, long px, bool resize, std::vector<double>& samples) {
        for (std::uint32_t i = 0; i < count; ++i) {
            // Alternate between +px and +2px from the start rect: never back at the baseline, so a content
            // window that does not follow at all can never look aligned by accident.
            const long d = px * (1 + static_cast<long>(i % 2));
            RECT cur = f0;
            if (resize) {
                cur.right += d;
            } else {
                cur.left += d;
                cur.right += d;
            }
            const LONGLONG t0 = qpc_now();
            place(cur, resize ? SWP_NOMOVE : SWP_NOSIZE);
            if (const auto lag = wait_aligned(t0)) {
                samples.push_back(*lag);
            } else {
                ++out.timeouts;
            }
            Sleep(static_cast<DWORD>(cfg.gap_ms));
        }
    };
    const auto live = [&](long px, bool resize, double& in_sync_pct, double& catchup_ms) {
        RECT cur = f0;
        std::uint32_t checks = 0;
        std::uint32_t in_sync = 0;
        LONGLONG next = qpc_now();
        LONGLONG last_move = next;
        for (std::uint32_t k = 0; k < cfg.live_steps; ++k) {
            spin_until(next);
            if (k > 0) { // one frame after the previous step: is the content where it belongs?
                ++checks;
                if (is_aligned()) {
                    ++in_sync;
                }
            }
            const long d = px; // one direction only, so the path never crosses the baseline
            if (resize) {
                cur.right += d;
            } else {
                cur.left += d;
                cur.right += d;
            }
            last_move = qpc_now();
            place(cur, resize ? SWP_NOMOVE : SWP_NOSIZE);
            next += ticks(cfg.frame_ms);
        }
        // The last step: poll right away, so the catch-up time is measured from the move itself.
        const std::optional<double> lag = wait_aligned(last_move);
        ++checks;
        if (lag && *lag <= cfg.frame_ms) {
            ++in_sync;
        }
        in_sync_pct = 100.0 * static_cast<double>(in_sync) / static_cast<double>(checks);
        if (lag) {
            catchup_ms = *lag;
        } else {
            catchup_ms = cfg.timeout_ms;
            ++out.timeouts;
        }
        Sleep(static_cast<DWORD>(cfg.gap_ms));
    };

    discrete(cfg.moves, cfg.move_px, false, out.move_ms);
    live(cfg.live_move_px, false, out.drag_in_sync_pct, out.drag_catchup_ms);
    place(f0, 0);
    static_cast<void>(wait_aligned(qpc_now()));
    Sleep(static_cast<DWORD>(cfg.gap_ms));
    discrete(cfg.resizes, cfg.resize_px, true, out.resize_ms);
    live(cfg.live_resize_px, true, out.live_resize_in_sync_pct, out.live_resize_catchup_ms);
    place(f0, 0);
    static_cast<void>(wait_aligned(qpc_now()));
    return out;
}

// ---- Aggregation and rendering ----------------------------------------------------------------------------

struct MetricRow {
    std::string group;
    std::string metric;
    int decimals = 1;
    bool lower_is_better = true;
    std::vector<double> values; ///< one per measured (non-warm-up) run
};

[[nodiscard]] inline std::optional<double> median_vec(const std::vector<double>& values) {
    std::vector<double> valid;
    for (const double v : values) {
        if (v >= 0.0) {
            valid.push_back(v);
        }
    }
    return median_of(std::move(valid));
}

[[nodiscard]] inline std::optional<double> p95_vec(std::vector<double> values) {
    std::erase_if(values, [](double v) { return v < 0.0; });
    if (values.empty()) {
        return std::nullopt;
    }
    std::ranges::sort(values);
    return percentile_sorted(values, 95.0);
}

inline const std::vector<std::string>& checkpoint_names() {
    static const std::vector<std::string> names{"tabs_1", "tabs_5", "tabs_5_slept", "tabs_10", "tabs_10_slept"};
    return names;
}

inline constexpr std::string_view kSleptMetric = "tabs asleep at 5 / at 10";

/// @brief One row per metric; each row holds the per-run value (the median of the run's samples where a run
/// has many).
[[nodiscard]] inline std::vector<MetricRow> metric_rows(const ScenarioReport& report) {
    std::vector<MetricRow> rows;
    std::map<std::string, std::size_t> index;
    const auto add = [&](std::string group, std::string metric, std::optional<double> value, int decimals = 1, bool lower = true) {
        const std::string key = group + "|" + metric;
        auto it = index.find(key);
        if (it == index.end()) {
            it = index.emplace(key, rows.size()).first;
            rows.push_back(MetricRow{.group = std::move(group), .metric = std::move(metric), .decimals = decimals, .lower_is_better = lower, .values = {}});
        }
        if (value.has_value()) {
            rows[it->second].values.push_back(*value);
        }
    };
    for (const ScenarioRunResult& run : report.runs) {
        if (run.warmup || !run.error.empty()) {
            continue;
        }
        add("Startup", "first window (ms)", run.first_window_ms, 0);
        add("Startup", "shell first paint (ms)", run.shell_paint_ms, 0);
        add("Startup", "first tab NavigationCompleted (ms)", run.first_nav_ms, 0);
        for (const std::string& name : checkpoint_names()) {
            const auto it = std::ranges::find_if(run.checkpoints, [&](const CheckpointMemory& m) { return m.name == name; });
            const bool found = it != run.checkpoints.end();
            add("Memory " + name, "host private bytes (MB)", found ? std::optional{it->host_commit_mb} : std::nullopt);
            add("Memory " + name, "WebView2 tree working set (MB)", found ? std::optional{it->webview_ws_mb} : std::nullopt);
            add("Memory " + name, "whole tree private WS (MB)", found ? std::optional{it->tree_private_ws_mb} : std::nullopt);
            add("Memory " + name, "processes", found ? std::optional{static_cast<double>(it->processes)} : std::nullopt, 0);
        }
        if (run.app) {
            const AppResult& app = *run.app;
            add("Tabs", "new tab -> NavigationCompleted (ms, median)", median_vec(app.new_tab_nav_ms));
            add("Tabs", "tab switch -> 2 frames (ms, median)", median_vec(app.tab_switch_ms));
            add("Tabs", "tab switch -> 2 frames (ms, p95)", p95_vec(app.tab_switch_ms));
            add("Tabs", "wake sleeping tab -> 2 frames (ms, median)", median_vec(app.wake_ms));
            add("Tabs", "destroy + recreate -> NavigationCompleted (ms, median)", median_vec(app.recreate_nav_ms));
            add("Tabs", "destroy + recreate -> 2 frames (ms, median)", median_vec(app.recreate_visible_ms));
            add("Tabs", std::string{kSleptMetric}, static_cast<double>(app.slept_at_5) * 100.0 + app.slept_at_10, 0, false);
            if (app.ipc && app.ipc->ok) {
                const IpcResult& ipc = *app.ipc;
                add("IPC", "ping round trip (ms, mean of 1000)", ipc.ping_sequential.mean_ms, 3);
                add("IPC", "ping round trip (ms, p99)", ipc.ping_sequential.p99_ms, 3);
                add("IPC", "1000 concurrent pings (calls/s)", ipc.ping_concurrent.calls_per_s, 0, false);
                add("IPC", "1 MiB echo round trip (ms, median of 5)", ipc.echo_1mib.median_ms, 1);
                add("IPC", "event emit, 10000 events (events/s)", ipc.emit_burst.events_per_s, 0, false);
            }
        }
        if (run.lag && run.lag->error.empty()) {
            const LagResult& lag = *run.lag;
            add("Window", "move: content follows after (ms, median)", median_vec(lag.move_ms), 2);
            add("Window", "move: content follows after (ms, p95)", p95_vec(lag.move_ms), 2);
            add("Window", "60 Hz drag: frames in sync (%)", lag.drag_in_sync_pct, 0, false);
            add("Window", "60 Hz drag: catch-up after last step (ms)", lag.drag_catchup_ms, 2);
            add("Window", "resize: content follows after (ms, median)", median_vec(lag.resize_ms), 2);
            add("Window", "60 Hz live resize: frames in sync (%)", lag.live_resize_in_sync_pct, 0, false);
            add("Window", "60 Hz live resize: catch-up (ms)", lag.live_resize_catchup_ms, 2);
        }
        add("Lifecycle", "whole scenario (s)", run.scenario_ms ? std::optional{*run.scenario_ms / 1000.0} : std::nullopt);
        add("Lifecycle", "shutdown (ms)", run.shutdown_ms, 0);
    }
    std::erase_if(rows, [](const MetricRow& r) { return r.values.empty(); });
    return rows;
}

/// @brief "median [min-max]" of a row (the tabs-asleep pseudo metric is printed as "a / b").
[[nodiscard]] inline std::string format_cell(const MetricRow* row) {
    if (row == nullptr || row->values.empty()) {
        return "n/a";
    }
    std::vector<double> sorted = row->values;
    std::ranges::sort(sorted);
    const double med = median_of(sorted).value_or(0.0);
    if (row->metric == kSleptMetric) {
        const auto v = static_cast<long long>(med + 0.5);
        return std::to_string(v / 100) + " / " + std::to_string(v % 100);
    }
    std::string cell = fmt(med, row->decimals);
    if (sorted.size() > 1) {
        cell += " [" + fmt(sorted.front(), row->decimals) + "-" + fmt(sorted.back(), row->decimals) + "]";
    }
    return cell;
}

[[nodiscard]] inline std::string render_scenario_markdown(const ScenarioReport& report) {
    std::string md = "# boson_bench --scenario browser: " + report.name + "\n\n";
    std::uint32_t measured = 0;
    std::uint32_t failed = 0;
    for (const auto& run : report.runs) {
        if (!run.warmup) {
            ++measured;
            failed += run.error.empty() ? 0 : 1;
        }
    }
    md += "- Target: `" + report.target + "`";
    for (const auto& a : report.args) {
        md += " `" + a + "`";
    }
    md += "\n- Executable: " + std::to_string(report.exe_bytes) + " bytes\n";
    md += "- Machine: " + report.machine.os + ", " + report.machine.cpu + " (" + std::to_string(report.machine.logical_cpus) +
          " logical CPUs), " + std::to_string(report.machine.total_ram_mb) + " MB RAM, WebView2 " + report.machine.webview2_runtime + "\n";
    md += "- Measured: " + report.timestamp + ", " + std::to_string(measured) + " run(s)" +
          (failed > 0 ? " (" + std::to_string(failed) + " failed)" : std::string{}) + ", memory settle " +
          fmt(report.checkpoint_settle_s, 1) + " s. Cells: median [min-max] over runs.\n\n";
    md += "| Group | Metric | Value |\n|---|---|---:|\n";
    for (const MetricRow& row : metric_rows(report)) {
        md += "| " + row.group + " | " + row.metric + " | " + format_cell(&row) + " |\n";
    }
    for (const auto& run : report.runs) {
        if (!run.error.empty()) {
            md += "\n- Run " + std::to_string(run.run) + (run.warmup ? " (warm-up)" : "") + " failed: " + run.error;
        }
        if (run.app) {
            for (const auto& e : run.app->errors) {
                md += "\n- Run " + std::to_string(run.run) + " app note: " + e;
            }
        }
    }
    md += "\n";
    return md;
}

/// @brief Side-by-side table; with exactly two reports a "B / A" column (ratio of medians) is added.
[[nodiscard]] inline std::string render_scenario_comparison(std::span<const ScenarioReport> reports) {
    std::string md = "| Group | Metric |";
    std::string sep = "|---|---|";
    for (const auto& r : reports) {
        md += " " + r.name + " |";
        sep += "---:|";
    }
    if (reports.size() == 2) {
        md += " " + reports[1].name + " / " + reports[0].name + " |";
        sep += "---:|";
    }
    md += "\n" + sep + "\n";
    std::vector<std::vector<MetricRow>> all;
    for (const auto& r : reports) {
        all.push_back(metric_rows(r));
    }
    std::vector<std::pair<std::string, std::string>> order;
    for (const auto& rows : all) {
        for (const auto& row : rows) {
            std::pair<std::string, std::string> key{row.group, row.metric};
            if (std::ranges::find(order, key) == order.end()) {
                order.push_back(std::move(key));
            }
        }
    }
    for (const auto& [group, metric] : order) {
        md += "| " + group + " | " + metric + " |";
        std::vector<const MetricRow*> found;
        for (const auto& rows : all) {
            const auto it = std::ranges::find_if(rows, [&](const MetricRow& r) { return r.group == group && r.metric == metric; });
            found.push_back(it == rows.end() ? nullptr : &*it);
            md += " " + format_cell(found.back()) + " |";
        }
        if (reports.size() == 2) {
            std::string ratio;
            if (found[0] != nullptr && found[1] != nullptr && metric != kSleptMetric) {
                const double a = median_of(found[0]->values).value_or(0.0);
                const double b = median_of(found[1]->values).value_or(0.0);
                if (a > 0.0) {
                    ratio = fmt(b / a, 2) + "x";
                }
            }
            md += " " + ratio + " |";
        }
        md += "\n";
    }
    return md;
}

} // namespace boson::bench::scenario
