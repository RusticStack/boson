#pragma once

// Benchmark mode of boson_browser (boson_browser.exe --bench, driven by boson_bench --scenario browser).
//
// Runs the same scripted scenario as bench/wails_browser (the Wails v3 equivalent) and talks to the harness
// through files in %BOSON_BENCH_DIR%:
//   shell_paint / first_nav    QueryPerformanceCounter stamps (first shell frame, first tab NavigationCompleted)
//   checkpoint                 "<seq> <name>": the harness samples memory (or runs the window-lag probe for
//                              "window") and answers with ack = "<seq>"
//   hwnds                      "<frame hwnd> <content hwnd>" for the window-lag probe
//   result.json / done         in-app timings, written once at the end
// Everything here runs on the UI thread except BenchController's methods (pool threads), which marshal.

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

#include "browser_bench_assets.hpp"

namespace boson::browser::bench {

/// @brief What the driver needs from the browser. Browser implements it; all calls are UI-thread only.
class TabOps {
public:
    virtual ~TabOps() = default;
    /// @brief Opens url in a new, active tab.
    virtual core::WebView& bench_open_tab(std::string url) = 0;
    virtual void bench_activate_tab(std::uint64_t id) = 0;
    virtual void bench_close_tab(std::uint64_t id) = 0;
    [[nodiscard]] virtual std::vector<std::uint64_t> bench_tab_ids() const = 0;
    [[nodiscard]] virtual std::uint64_t bench_active_tab() const = 0;
};

/// The __benchHost adapter for bench_shell.js over Boson's JSON-RPC (ids far above the shell's own).
inline constexpr std::string_view kShellAdapterJs = R"JS((() => {
  const wv = window.chrome && window.chrome.webview;
  if (!wv || window.__benchHost) return;
  let nextId = 1000000000;
  const pending = new Map();
  const listeners = {};
  wv.addEventListener('message', (e) => {
    const d = e.data;
    if (!d) return;
    if (d.type === 'event') { (listeners[d.event] || []).forEach((f) => f(d.data)); return; }
    const p = pending.get(d.id);
    if (!p) return;
    pending.delete(d.id);
    if (d.status === 'success') p.res(d.data); else p.rej(new Error((d.error && d.error.message) || 'IPC error'));
  });
  const call = (method, params) => new Promise((res, rej) => {
    const id = nextId++;
    pending.set(id, { res, rej });
    wv.postMessage({ id, method, params: params || {} });
  });
  const on = (event, f) => (listeners[event] = listeners[event] || []).push(f);
  window.__benchHost = {
    painted: () => call('Bench.painted'),
    ping: (seq) => call('Bench.ping', { seq }).then((r) => r.seq),
    echo: (text) => call('Bench.echo', { text }).then((r) => r.text),
    emitBurst: (n) => call('Bench.emit_burst', { count: n }),
    onTick: (f) => on('bench:tick', f),
    onRunIpc: (f) => on('bench:run-ipc', f),
    report: (json) => call('Bench.report', { json }),
  };
})();
)JS";

struct AppResult {
    std::string framework = "boson";
    std::string sleep_mode = "try_suspend";
    glz::raw_json ipc{"null"};
    std::vector<double> new_tab_nav_ms;      ///< new tab (warm) -> NavigationCompleted
    std::vector<double> tab_switch_ms;       ///< activate an awake hidden tab -> its next two frames
    std::vector<double> wake_ms;             ///< activate a sleeping tab -> its next two frames
    std::vector<double> recreate_nav_ms;     ///< destroy a tab + create it again -> NavigationCompleted
    std::vector<double> recreate_visible_ms; ///< same, -> its first two frames
    std::uint32_t slept_at_5 = 0;
    std::uint32_t slept_at_10 = 0;
    std::vector<std::string> errors;
};

struct MarkReply {
    std::string bench;
    std::uint64_t seq = 0;
};

struct SeqMessage {
    std::uint64_t seq = 0;
};
struct TextMessage {
    std::string text;
};
struct CountRequest {
    std::uint32_t count = 0;
};
struct JsonReport {
    std::string json;
};
struct Tick {
    std::uint32_t i = 0;
};

[[nodiscard]] inline LONGLONG qpc_now() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

[[nodiscard]] inline double qpc_ms(LONGLONG from, LONGLONG to) noexcept {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    return static_cast<double>(to - from) * 1000.0 / static_cast<double>(freq.QuadPart);
}

/// @brief Scripted scenario; see the header comment. Identical step order to bench/wails_browser/bench.go.
class Driver {
public:
    static constexpr std::uint32_t kSwitches = 20;
    static constexpr std::uint32_t kRecreates = 5;

    Driver(std::filesystem::path dir, core::Window& window, core::WebViewHost& host, TabOps& tabs)
        : m_dir(std::move(dir)), m_window(window), m_host(host), m_tabs(tabs) {}

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    void set_bridge(ipc::Bridge* bridge) noexcept { m_bridge = bridge; }

    /// @brief Injects the adapter + bench_shell.js into every document of the shell WebView.
    static void configure_shell(core::WebViewConfig& shell_cfg) {
        std::string script{kShellAdapterJs};
        script += '\n';
        script += kShellJs;
        shell_cfg.scripts_on_document_created.push_back(core::utf8_to_wide(script));
    }

    [[nodiscard]] std::string next_tab_url() { return std::format("boson://app/bench_tab.html?tab={}", ++m_tab_counter); }

    // ---- inputs ----------------------------------------------------------------------------------------

    void on_shell_painted() {
        if (m_shell_paint != 0) return;
        m_shell_paint = qpc_now();
        write_file(L"shell_paint", std::to_string(m_shell_paint));
        write_file(L"ready", std::to_string(m_shell_paint));
        maybe_started();
    }

    void on_tab_navigated(std::uint64_t id, bool ok) {
        if (!ok) m_result.errors.push_back(std::format("tab {} navigation failed", id));
        if (m_first_nav == 0) {
            m_first_nav = qpc_now();
            write_file(L"first_nav", std::to_string(m_first_nav));
            maybe_started();
            return;
        }
        if (m_nav_wait && m_nav_wait->first == id) {
            auto done = std::move(m_nav_wait->second);
            m_nav_wait.reset();
            done();
        }
    }

    void on_tab_message(std::uint64_t /*id*/, std::string_view json) {
        MarkReply reply;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(reply, json) || reply.bench != "mark") return;
        if (!m_mark || m_mark->seq != reply.seq) return;
        m_mark_timer.stop();
        const double ms = qpc_ms(m_mark->t0, qpc_now());
        auto done = std::move(m_mark->done);
        m_mark.reset();
        done(ms);
    }

    void on_tab_slept(std::uint64_t /*id*/) {
        if (!m_sleep_done) return;
        ++m_slept;
        if (m_slept >= m_sleep_expected) finish_sleep();
    }

    void on_ipc_report(std::string json) {
        if (glz::validate_json(json)) {
            m_result.errors.push_back("IPC report is not valid JSON");
            json = "null";
        }
        m_result.ipc.str = std::move(json);
        if (auto next = std::exchange(m_ipc_done, {})) next();
    }

private:
    using Step = std::function<void()>;

    void maybe_started() {
        if (m_shell_paint == 0 || m_first_nav == 0 || m_started) return;
        m_started = true;
        after(std::chrono::milliseconds{500}, [this]() {
            checkpoint("tabs_1", [this]() {
                write_hwnds();
                checkpoint("window", [this]() { run_ipc([this]() { stage_5(); }); });
            });
        });
    }

    void stage_5() {
        open_until(5, [this]() {
            checkpoint("tabs_5", [this]() {
                sleep_hidden(m_result.slept_at_5, [this]() {
                    checkpoint("tabs_5_slept", [this]() { wake_hidden([this]() { stage_10(); }); });
                });
            });
        });
    }

    void stage_10() {
        open_until(10, [this]() {
            checkpoint("tabs_10", [this]() {
                sleep_hidden(m_result.slept_at_10, [this]() {
                    checkpoint("tabs_10_slept", [this]() {
                        wake_hidden([this]() { switch_test(0, [this]() { recreate_test(0, [this]() { finish(); }); }); });
                    });
                });
            });
        });
    }

    // ---- steps -------------------------------------------------------------------------------------------

    void run_ipc(Step next) {
        m_ipc_done = std::move(next);
        if (m_bridge == nullptr || !m_bridge->emit_all("bench:run-ipc")) {
            m_result.errors.push_back("could not start the IPC test");
            if (auto n = std::exchange(m_ipc_done, {})) n();
        }
    }

    void open_until(std::size_t count, Step next) {
        if (m_tabs.bench_tab_ids().size() >= count) {
            next();
            return;
        }
        const LONGLONG t0 = qpc_now();
        core::WebView& webview = m_tabs.bench_open_tab(next_tab_url());
        wait_nav(webview.id(), [this, t0, count, next = std::move(next)]() mutable {
            m_result.new_tab_nav_ms.push_back(qpc_ms(t0, qpc_now()));
            after(std::chrono::milliseconds{100}, [this, count, next = std::move(next)]() mutable { open_until(count, std::move(next)); });
        });
    }

    void sleep_hidden(std::uint32_t& slept_out, Step next) {
        const std::uint64_t active = m_tabs.bench_active_tab();
        m_sleep_expected = 0;
        for (const std::uint64_t id : m_tabs.bench_tab_ids()) {
            if (id != active) ++m_sleep_expected;
        }
        m_slept = 0;
        m_slept_out = &slept_out;
        m_sleep_done = std::move(next);
        // The real SleepPolicy path (lower the memory target, TrySuspend), with the hidden timeout forced to 0.
        m_host.set_sleep_policy(core::SleepPolicy{.hidden_timeout = std::chrono::milliseconds{0}, .check_interval = std::chrono::hours{1}});
        m_host.check_sleep_now();
        m_sleep_timer.start(std::chrono::seconds{15}, [this]() {
            m_result.errors.push_back(std::format("only {} of {} hidden tabs reported sleeping", m_slept, m_sleep_expected));
            finish_sleep();
        });
        if (m_sleep_expected == 0) finish_sleep();
    }

    void finish_sleep() {
        m_sleep_timer.stop();
        if (m_slept_out != nullptr) *m_slept_out = m_slept;
        m_host.set_sleep_policy(core::SleepPolicy{}); // back to the browser's default (5 min hidden)
        if (auto next = std::exchange(m_sleep_done, {})) {
            after(std::chrono::milliseconds{200}, std::move(next));
        }
    }

    /// Activates every hidden tab once (oldest first), timing activate -> two frames; ends on the newest tab.
    void wake_hidden(Step next) {
        std::vector<std::uint64_t> ids = m_tabs.bench_tab_ids();
        const std::uint64_t active = m_tabs.bench_active_tab();
        std::erase(ids, active);
        ids.push_back(active);
        wake_one(std::move(ids), 0, std::move(next));
    }

    void wake_one(std::vector<std::uint64_t> ids, std::size_t index, Step next) {
        if (index >= ids.size()) {
            next();
            return;
        }
        const std::uint64_t id = ids[index];
        const bool record = index + 1 < ids.size(); // the last one was the awake active tab
        const LONGLONG t0 = qpc_now();
        m_tabs.bench_activate_tab(id);
        mark(id, t0, [this, ids = std::move(ids), index, record, next = std::move(next)](double ms) mutable {
            if (record) m_result.wake_ms.push_back(ms);
            after(std::chrono::milliseconds{100}, [this, ids = std::move(ids), index, next = std::move(next)]() mutable {
                wake_one(std::move(ids), index + 1, std::move(next));
            });
        });
    }

    void switch_test(std::uint32_t i, Step next) {
        const std::vector<std::uint64_t> ids = m_tabs.bench_tab_ids();
        if (i >= kSwitches || ids.size() < 2) {
            next();
            return;
        }
        const std::uint64_t target = ids[i % 2];
        const LONGLONG t0 = qpc_now();
        m_tabs.bench_activate_tab(target);
        mark(target, t0, [this, i, next = std::move(next)](double ms) mutable {
            m_result.tab_switch_ms.push_back(ms);
            after(std::chrono::milliseconds{100}, [this, i, next = std::move(next)]() mutable { switch_test(i + 1, std::move(next)); });
        });
    }

    /// Destroys a hidden tab and opens the same page again as the active tab (the alternative to sleeping).
    void recreate_test(std::uint32_t i, Step next) {
        const std::vector<std::uint64_t> ids = m_tabs.bench_tab_ids();
        const std::uint64_t active = m_tabs.bench_active_tab();
        std::uint64_t victim = 0;
        for (const std::uint64_t id : ids) {
            if (id != active) {
                victim = id;
                break;
            }
        }
        if (i >= kRecreates || victim == 0) {
            next();
            return;
        }
        const LONGLONG t0 = qpc_now();
        m_tabs.bench_close_tab(victim);
        core::WebView& webview = m_tabs.bench_open_tab(next_tab_url());
        const std::uint64_t id = webview.id();
        wait_nav(id, [this, t0, id, i, next = std::move(next)]() mutable {
            m_result.recreate_nav_ms.push_back(qpc_ms(t0, qpc_now()));
            mark(id, t0, [this, i, next = std::move(next)](double ms) mutable {
                m_result.recreate_visible_ms.push_back(ms);
                after(std::chrono::milliseconds{100}, [this, i, next = std::move(next)]() mutable { recreate_test(i + 1, std::move(next)); });
            });
        });
    }

    void finish() {
        std::string json;
        if (glz::write_json(m_result, json)) json = R"({"errors":["cannot serialize the result"]})";
        write_file(L"result.json", json);
        write_file(L"done", "1");
    }

    // ---- primitives ----------------------------------------------------------------------------------------

    void after(std::chrono::milliseconds delay, Step next) {
        m_delay_next = std::move(next);
        m_delay.start(delay, [this]() {
            if (auto n = std::exchange(m_delay_next, {})) n();
        });
    }

    void wait_nav(std::uint64_t id, Step done) { m_nav_wait.emplace(id, std::move(done)); }

    void mark(std::uint64_t id, LONGLONG t0, std::function<void(double)> done) {
        const std::uint64_t seq = ++m_mark_seq;
        m_mark = PendingMark{.seq = seq, .t0 = t0, .done = std::move(done)};
        bool posted = false;
        for (const auto& webview : m_host.webviews()) {
            if (webview->id() == id) posted = webview->post_web_message_as_json(std::format(R"({{"bench":"mark","seq":{}}})", seq));
        }
        if (!posted) m_result.errors.push_back(std::format("mark {}: could not post to tab {}", seq, id));
        m_mark_timer.start(std::chrono::seconds{15}, [this, seq]() {
            if (!m_mark || m_mark->seq != seq) return;
            m_result.errors.push_back(std::format("mark {} timed out", seq));
            auto cb = std::move(m_mark->done);
            m_mark.reset();
            cb(-1.0);
        });
    }

    void checkpoint(std::string_view name, Step next) {
        const std::uint64_t seq = ++m_checkpoint_seq;
        m_checkpoint_name = std::string{name};
        m_checkpoint_next = std::move(next);
        m_checkpoint_start = std::chrono::steady_clock::now();
        write_file(L"checkpoint", std::format("{} {}", seq, name));
        m_ack_timer.start(std::chrono::milliseconds{10}, [this, seq]() { poll_ack(seq); }, true);
    }

    void poll_ack(std::uint64_t seq) {
        const std::optional<std::string> text = read_file(L"ack");
        bool acked = false;
        if (text) {
            try {
                acked = std::stoull(*text) == seq;
            } catch (...) {
                acked = false;
            }
        }
        const bool timed_out = std::chrono::steady_clock::now() - m_checkpoint_start > std::chrono::seconds{120};
        if (!acked && !timed_out) return;
        m_ack_timer.stop();
        if (!acked) m_result.errors.push_back("checkpoint " + m_checkpoint_name + ": no ack from the harness");
        if (auto n = std::exchange(m_checkpoint_next, {})) n();
    }

    void write_hwnds() {
        HWND content = nullptr;
        const std::uint64_t active = m_tabs.bench_active_tab();
        for (const auto& webview : m_host.webviews()) {
            if (webview->id() == active) content = webview->container_hwnd();
        }
        write_file(L"hwnds", std::format("{} {}", reinterpret_cast<std::uintptr_t>(m_window.hwnd()), reinterpret_cast<std::uintptr_t>(content)));
    }

    void write_file(std::wstring_view name, std::string_view text) const {
        const std::filesystem::path target = m_dir / name;
        std::filesystem::path tmp = target;
        tmp += L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    [[nodiscard]] std::optional<std::string> read_file(std::wstring_view name) const {
        std::ifstream in(m_dir / name, std::ios::binary);
        if (!in) return std::nullopt;
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    struct PendingMark {
        std::uint64_t seq = 0;
        LONGLONG t0 = 0;
        std::function<void(double)> done;
    };

    std::filesystem::path m_dir;
    core::Window& m_window;
    core::WebViewHost& m_host;
    TabOps& m_tabs;
    ipc::Bridge* m_bridge = nullptr;
    AppResult m_result;

    std::uint32_t m_tab_counter = 0;
    LONGLONG m_shell_paint = 0;
    LONGLONG m_first_nav = 0;
    bool m_started = false;

    core::UiTimer m_delay;
    Step m_delay_next;
    core::UiTimer m_ack_timer;
    std::uint64_t m_checkpoint_seq = 0;
    std::string m_checkpoint_name;
    Step m_checkpoint_next;
    std::chrono::steady_clock::time_point m_checkpoint_start{};

    std::optional<std::pair<std::uint64_t, Step>> m_nav_wait;
    core::UiTimer m_mark_timer;
    std::uint64_t m_mark_seq = 0;
    std::optional<PendingMark> m_mark;

    core::UiTimer m_sleep_timer;
    std::uint32_t m_sleep_expected = 0;
    std::uint32_t m_slept = 0;
    std::uint32_t* m_slept_out = nullptr;
    Step m_sleep_done;

    Step m_ipc_done;
};

/// @brief Bench.* RPC for bench_shell.js. Pool threads; UI-thread work is marshaled to the Driver.
class BenchController {
public:
    BOSON_CONTROLLER("Bench");

    explicit BenchController(exec::UiMarshal& marshal) : m_marshal(marshal) {}

    void set_driver(Driver* driver) noexcept { m_driver = driver; }
    void set_bridge(ipc::Bridge* bridge) noexcept { m_bridge = bridge; }

    void painted() const {
        if (m_driver != nullptr) m_marshal.post_task([driver = m_driver]() { driver->on_shell_painted(); });
    }
    [[nodiscard]] SeqMessage ping(SeqMessage req) const noexcept { return req; }
    [[nodiscard]] TextMessage echo(TextMessage req) const { return req; }
    void emit_burst(CountRequest req) const {
        if (m_bridge == nullptr) return;
        for (std::uint32_t i = 0; i < req.count; ++i) {
            m_bridge->emit("bench:tick", Tick{.i = i});
        }
    }
    void report(JsonReport req) const {
        if (m_driver != nullptr) {
            m_marshal.post_task([driver = m_driver, json = std::move(req.json)]() mutable { driver->on_ipc_report(std::move(json)); });
        }
    }

private:
    exec::UiMarshal& m_marshal;
    Driver* m_driver = nullptr;
    ipc::Bridge* m_bridge = nullptr;
};

} // namespace boson::browser::bench

template <>
struct glz::meta<boson::browser::bench::BenchController> {
    using T = boson::browser::bench::BenchController;
    static constexpr auto value = glz::object(
        "painted", &T::painted,
        "ping", &T::ping,
        "echo", &T::echo,
        "emit_burst", &T::emit_burst,
        "report", &T::report);
};
