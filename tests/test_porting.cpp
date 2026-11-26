// Porting helpers (compat::ElectronIpc, DynamicController routing, sender context) and the boson_bench core.
// The headless part always runs; the WebView part skips when no WebView2 runtime is installed.
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>
#include <boson/ipc/system_controller.hpp>
#include <boson/ipc/ts_generator.hpp>

#include "bench_core.hpp"

namespace pt {

struct AddRequest {
    int a = 0;
    int b = 0;
};

struct AddResult {
    int sum = 0;
};

class MathController {
public:
    BOSON_CONTROLLER("Math");
    [[nodiscard]] AddResult add(AddRequest req) const { return AddResult{.sum = req.a + req.b}; }
};

// C++ twin of the Go TabService in tests/js/fixtures/wails/tabs.go.txt: the generated Wails bindings in
// tests/js/fixtures/wails/bindings call it through compat/wails.ts. Request structs name the positional args.
struct Tab {
    int id = 0;
    std::string url;
    std::string title;
    bool muted = false;
    std::vector<std::string> tags;
    std::map<std::string, std::string> meta;
    /// Go's `Opener *Tab`: null or a Tab object. Kept as raw JSON because glaze cannot reflect a
    /// self-referential C++ struct (shared_ptr<Tab> recurses at compile time).
    glz::raw_json opener{"null"};
};

struct CreateTabRequest {
    std::string raw_url;
    std::string title;
};

struct RenameRequest {
    Tab tab;
    std::string title;
};

struct ResizeRequest {
    int id = 0;
    int width = 0;
    int height = 0;
};

struct SetMutedRequest {
    int id = 0;
    bool muted = false;
};

class TabService {
public:
    BOSON_CONTROLLER("TabService");

    [[nodiscard]] std::optional<Tab> create_tab(CreateTabRequest req) {
        if (req.raw_url.empty()) {
            throw std::invalid_argument("empty url");
        }
        const std::scoped_lock lock(m_mutex);
        Tab tab{
            .id = static_cast<int>(m_tabs.size()) + 1,
            .url = std::move(req.raw_url),
            .title = std::move(req.title),
            .muted = false,
            .tags = {"new"},
            .meta = {{"source", "wails"}},
            .opener = glz::raw_json{"null"},
        };
        if (!m_tabs.empty()) {
            std::string first;
            if (glz::write_json(m_tabs.front(), first)) {
                throw std::runtime_error("cannot serialize the opener tab");
            }
            tab.opener = glz::raw_json{std::move(first)};
        }
        m_tabs.push_back(tab);
        return tab;
    }

    [[nodiscard]] std::vector<Tab> list_tabs() const {
        const std::scoped_lock lock(m_mutex);
        return m_tabs;
    }

    [[nodiscard]] Tab rename(RenameRequest req) const {
        req.tab.title = std::move(req.title);
        return req.tab;
    }

    [[nodiscard]] bool resize(ResizeRequest req) const { return req.id == 1 && req.width > req.height; }

    void set_muted(SetMutedRequest req) {
        const std::scoped_lock lock(m_mutex);
        for (auto& tab : m_tabs) {
            if (tab.id == req.id) {
                tab.muted = req.muted;
                return;
            }
        }
        throw std::out_of_range("no tab " + std::to_string(req.id));
    }

    [[nodiscard]] std::string version() const { return "1.0"; }

private:
    mutable std::mutex m_mutex;
    std::vector<Tab> m_tabs;
};

struct FlagResponse {
    bool value = false;
};

struct FakeTitleRequest {
    std::string title;
};

struct FakeToggleRequest {
    bool enabled = false;
};

/// Same "Boson" method names and request members as boson::ipc::SystemController (checked by
/// test_method_params), recording calls instead of moving a real window, so the test never steals focus.
class FakeWindowController {
public:
    BOSON_CONTROLLER("Boson");

    void minimize() { record("minimize", [](State& s) { s.minimized = true; }); }
    [[nodiscard]] FlagResponse maximize_or_restore() {
        bool now = false;
        record("maximize_or_restore", [&now](State& s) { now = s.maximized = !s.maximized; });
        return FlagResponse{.value = now};
    }
    void restore() { record("restore", [](State& s) { s.minimized = s.maximized = false; }); }
    void close() { record("close", [](State&) {}); }
    void quit() { record("quit", [](State&) {}); }
    void set_title(FakeTitleRequest req) { record("set_title:" + req.title, [](State&) {}); }
    void set_fullscreen(FakeToggleRequest req) {
        record(req.enabled ? "set_fullscreen:1" : "set_fullscreen:0", [&req](State& s) { s.fullscreen = req.enabled; });
    }
    [[nodiscard]] FlagResponse is_maximized() const { return read([](const State& s) { return s.maximized; }); }
    [[nodiscard]] FlagResponse is_minimized() const { return read([](const State& s) { return s.minimized; }); }
    [[nodiscard]] FlagResponse is_fullscreen() const { return read([](const State& s) { return s.fullscreen; }); }
    [[nodiscard]] std::vector<std::string> calls() const {
        const std::scoped_lock lock(m_mutex);
        return m_calls;
    }

private:
    struct State {
        bool minimized = false;
        bool maximized = false;
        bool fullscreen = false;
    };

    template <typename Fn>
    void record(std::string call, Fn&& change) {
        const std::scoped_lock lock(m_mutex);
        m_calls.push_back(std::move(call));
        change(m_state);
    }

    template <typename Fn>
    [[nodiscard]] FlagResponse read(Fn&& get) const {
        const std::scoped_lock lock(m_mutex);
        return FlagResponse{.value = get(m_state)};
    }

    mutable std::mutex m_mutex;
    State m_state;
    std::vector<std::string> m_calls;
};

} // namespace pt

template <>
struct glz::meta<pt::MathController> {
    using T = pt::MathController;
    static constexpr auto value = glz::object("add", &T::add);
};

// Wails method names (PascalCase keys) bound to snake_case C++ methods.
template <>
struct glz::meta<pt::TabService> {
    using T = pt::TabService;
    static constexpr auto value = glz::object(
        "CreateTab", &T::create_tab,
        "ListTabs", &T::list_tabs,
        "Rename", &T::rename,
        "Resize", &T::resize,
        "SetMuted", &T::set_muted,
        "Version", &T::version);
};

template <>
struct glz::meta<pt::FakeWindowController> {
    using T = pt::FakeWindowController;
    static constexpr auto value = glz::object(
        "minimize", &T::minimize,
        "maximize_or_restore", &T::maximize_or_restore,
        "restore", &T::restore,
        "close", &T::close,
        "quit", &T::quit,
        "set_title", &T::set_title,
        "set_fullscreen", &T::set_fullscreen,
        "is_maximized", &T::is_maximized,
        "is_minimized", &T::is_minimized,
        "is_fullscreen", &T::is_fullscreen,
        "calls", &T::calls);
};

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_porting.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define PT_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

using boson::compat::ElectronIpc;
using boson::compat::IpcEvent;
using Json = ElectronIpc::Json;

static_assert(boson::ipc::DynamicController<ElectronIpc>);
static_assert(!boson::ipc::DynamicController<pt::MathController>);

void test_electron_invoke() {
    step("ElectronIpc.invoke through Router");
    pt::MathController math;
    ElectronIpc ipc;
    boson::ipc::Router<pt::MathController, ElectronIpc> router(math, ipc);

    PT_CHECK(ipc.handle("sum", [](const IpcEvent& e, Json args) {
        double total = 0;
        for (const auto& v : args.get_array()) {
            total += v.get_number();
        }
        Json out;
        out["total"] = total;
        out["channel"] = std::string{e.channel};
        return out;
    }));
    PT_CHECK(!ipc.handle("sum", [](const IpcEvent&, Json) { return Json{}; })); // duplicate, like Electron
    PT_CHECK(ipc.has_handler("sum"));

    std::string r = router.dispatch_sync(R"({"id":1,"method":"ElectronIpc.invoke","params":{"channel":"sum","args":[1,2,3.5]}})");
    PT_CHECK(contains(r, R"("status":"success")"));
    PT_CHECK(contains(r, R"("total":6.5)"));
    PT_CHECK(contains(r, R"("channel":"sum")"));
    PT_CHECK(contains(r, R"("id":1)"));

    // Static controllers keep working next to the dynamic one.
    r = router.dispatch_sync(R"({"id":2,"method":"Math.add","params":{"a":2,"b":40}})");
    PT_CHECK(contains(r, R"("sum":42)"));

    // Missing args means [] ; non-array args is rejected.
    PT_CHECK(ipc.handle("count", [](const IpcEvent&, Json args) { return Json(static_cast<double>(args.get_array().size())); }));
    r = router.dispatch_sync(R"({"id":3,"method":"ElectronIpc.invoke","params":{"channel":"count"}})");
    PT_CHECK(contains(r, R"("data":0)"));
    r = router.dispatch_sync(R"({"id":4,"method":"ElectronIpc.invoke","params":{"channel":"count","args":{"x":1}}})");
    PT_CHECK(contains(r, R"("code":-32602)"));

    // Unknown channel / method / bad params.
    r = router.dispatch_sync(R"({"id":5,"method":"ElectronIpc.invoke","params":{"channel":"nope","args":[]}})");
    PT_CHECK(contains(r, R"("code":-32601)") && contains(r, "nope"));
    r = router.dispatch_sync(R"({"id":6,"method":"ElectronIpc.frobnicate","params":{}})");
    PT_CHECK(contains(r, R"("code":-32601)") && contains(r, "Method not found"));
    r = router.dispatch_sync(R"({"id":7,"method":"ElectronIpc.invoke","params":{"args":[]}})");
    PT_CHECK(contains(r, R"("code":-32602)"));

    // Exceptions are caught at the bridge layer and become error envelopes.
    PT_CHECK(ipc.handle("boom", [](const IpcEvent&, Json) -> Json { throw std::runtime_error("kaboom"); }));
    r = router.dispatch_sync(R"({"id":8,"method":"ElectronIpc.invoke","params":{"channel":"boom","args":[]}})");
    PT_CHECK(contains(r, R"("status":"error")") && contains(r, "kaboom") && contains(r, R"("id":8)"));
    PT_CHECK(ipc.handle("boom2", [](const IpcEvent&, Json) -> Json { throw 42; }));
    r = router.dispatch_sync(R"({"id":9,"method":"ElectronIpc.invoke","params":{"channel":"boom2","args":[]}})");
    PT_CHECK(contains(r, R"("status":"error")") && contains(r, "unknown C++ exception"));

    // Raw handlers: zero-parse in, validated JSON out.
    PT_CHECK(ipc.handle_raw("raw", [](const IpcEvent&, std::string_view args) { return std::string{"{\"echo\":"} + std::string{args} + "}"; }));
    r = router.dispatch_sync(R"({"id":10,"method":"ElectronIpc.invoke","params":{"channel":"raw","args":["a",{"b":[1]}]}})");
    PT_CHECK(contains(r, R"("echo":["a",{"b":[1]}])"));
    PT_CHECK(ipc.handle_raw("raw_bad", [](const IpcEvent&, std::string_view) { return std::string{"{not json"}; }));
    r = router.dispatch_sync(R"({"id":11,"method":"ElectronIpc.invoke","params":{"channel":"raw_bad","args":[]}})");
    PT_CHECK(contains(r, R"("code":-32603)") && contains(r, "invalid JSON"));
    PT_CHECK(ipc.handle_raw("raw_void", [](const IpcEvent&, std::string_view) { return std::string{}; }));
    r = router.dispatch_sync(R"({"id":12,"method":"ElectronIpc.invoke","params":{"channel":"raw_void","args":[]}})");
    PT_CHECK(contains(r, R"("data":null)"));

    // handleOnce / removeHandler.
    int once_calls = 0;
    PT_CHECK(ipc.handle_once("once", [&once_calls](const IpcEvent&, Json) { ++once_calls; return Json(true); }));
    r = router.dispatch_sync(R"({"id":13,"method":"ElectronIpc.invoke","params":{"channel":"once","args":[]}})");
    PT_CHECK(contains(r, R"("data":true)"));
    r = router.dispatch_sync(R"({"id":14,"method":"ElectronIpc.invoke","params":{"channel":"once","args":[]}})");
    PT_CHECK(contains(r, R"("code":-32601)"));
    PT_CHECK(once_calls == 1);
    PT_CHECK(ipc.remove_handler("sum"));
    PT_CHECK(!ipc.remove_handler("sum"));
    PT_CHECK(!ipc.has_handler("sum"));
    PT_CHECK(ipc.handle("sum", [](const IpcEvent&, Json) { return Json{}; })); // re-registrable after removal

    // Without a Bridge nothing can be sent.
    PT_CHECK(!ipc.send(1, "x", 1, std::string{"two"}));
}

void test_electron_send_and_listeners() {
    step("ElectronIpc.send / on / once / removeListener");
    ElectronIpc ipc;
    boson::ipc::Router<ElectronIpc> router(ipc);

    std::vector<std::string> seen;
    const auto a = ipc.on("log", [&seen](const IpcEvent&, const Json& args) { seen.push_back("a:" + args[0].get_string()); });
    ipc.once("log", [&seen](const IpcEvent&, const Json& args) { seen.push_back("once:" + args[0].get_string()); });
    ipc.on("log", [](const IpcEvent&, const Json&) { throw std::runtime_error("listener failure must be contained"); });
    PT_CHECK(a != 0);
    PT_CHECK(ipc.listener_count("log") == 3);

    std::string r = router.dispatch_sync(R"({"id":1,"method":"ElectronIpc.send","params":{"channel":"log","args":["x"]}})");
    PT_CHECK(contains(r, R"("status":"success")"));
    PT_CHECK(seen.size() == 2);
    PT_CHECK(ipc.listener_count("log") == 2);
    r = router.dispatch_sync(R"({"id":2,"method":"ElectronIpc.send","params":{"channel":"log","args":["y"]}})");
    PT_CHECK(seen.size() == 3 && seen.back() == "a:y");
    PT_CHECK(ipc.remove_listener(a));
    PT_CHECK(!ipc.remove_listener(a));
    r = router.dispatch_sync(R"({"id":3,"method":"ElectronIpc.send","params":{"channel":"log","args":["z"]}})");
    PT_CHECK(seen.size() == 3);
    ipc.remove_all_listeners("log");
    PT_CHECK(ipc.listener_count("log") == 0);

    // send with no listener still succeeds (Electron drops it silently).
    r = router.dispatch_sync(R"({"id":4,"method":"ElectronIpc.send","params":{"channel":"nobody","args":[]}})");
    PT_CHECK(contains(r, R"("status":"success")"));

    // Frontend emit (Tauri emit / Wails Events.Emit) reaches on_event listeners with [payload].
    Json last{};
    ipc.on_event("app:ping", [&last](const IpcEvent&, const Json& args) { last = args[0]; });
    r = router.dispatch_sync(R"({"id":5,"method":"ElectronIpc.emit","params":{"event":"app:ping","payload":{"n":7},"broadcast":false}})");
    PT_CHECK(contains(r, R"("status":"success")"));
    PT_CHECK(last.is_object() && last["n"].get_number() == 7.0);
    r = router.dispatch_sync(R"({"id":6,"method":"ElectronIpc.emit","params":{"payload":1}})");
    PT_CHECK(contains(r, R"("code":-32602)"));
    ipc.remove_all_listeners();
    r = router.dispatch_sync(R"({"id":7,"method":"ElectronIpc.emit","params":{"event":"app:ping","payload":2,"broadcast":false}})");
    PT_CHECK(last["n"].get_number() == 7.0);

    PT_CHECK(boson::compat::encode_ipc_args(1, std::string{"a"}, true) == R"([1,"a",true])");
    PT_CHECK(boson::compat::encode_ipc_args() == "[]");
    PT_CHECK(boson::compat::ipc_event_name("ch") == "ipc:ch");
}

void test_sender_context() {
    step("sender id carried from the Bridge thread to the pool thread");
    PT_CHECK(boson::ipc::current_sender_id() == 0);
    {
        const boson::ipc::SenderScope outer{11};
        PT_CHECK(boson::ipc::current_sender_id() == 11);
        {
            const boson::ipc::SenderScope inner{22};
            PT_CHECK(boson::ipc::current_sender_id() == 22);
        }
        PT_CHECK(boson::ipc::current_sender_id() == 11);
    }
    PT_CHECK(boson::ipc::current_sender_id() == 0);

    ElectronIpc ipc;
    boson::ipc::Router<ElectronIpc> router(ipc);
    std::atomic<std::uint64_t> seen_sender{0};
    std::atomic<DWORD> handler_thread{0};
    ipc.handle("who", [&](const IpcEvent& e, Json) {
        seen_sender = e.sender_id;
        handler_thread = ::GetCurrentThreadId();
        return Json(static_cast<double>(e.sender_id));
    });

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([]() {});
    boson::exec::TaskRunner runner(scheduler, marshal);
    std::optional<std::string> reply;
    {
        const boson::ipc::SenderScope scope{77};
        router.dispatch_async(R"({"id":1,"method":"ElectronIpc.invoke","params":{"channel":"who","args":[]}})", runner,
                              [&reply](std::string r) { reply = std::move(r); });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (!reply && std::chrono::steady_clock::now() < deadline) {
        marshal.drain_on_ui_thread();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    PT_CHECK(reply.has_value() && contains(*reply, R"("data":77)"));
    PT_CHECK(seen_sender == 77);
    PT_CHECK(handler_thread != 0 && handler_thread != ::GetCurrentThreadId());
}

void test_method_params() {
    step("TsGenerator::method_params_json (positional names for the Wails shim)");
    using boson::ipc::TsGenerator;
    const std::string tabs = TsGenerator::method_params_json<pt::TabService, pt::MathController>();
    PT_CHECK(tabs == "{\n"
                     "    \"TabService.CreateTab\": [\"raw_url\", \"title\"],\n"
                     "    \"TabService.ListTabs\": [],\n"
                     "    \"TabService.Rename\": [\"tab\", \"title\"],\n"
                     "    \"TabService.Resize\": [\"id\", \"width\", \"height\"],\n"
                     "    \"TabService.SetMuted\": [\"id\", \"muted\"],\n"
                     "    \"TabService.Version\": [],\n"
                     "    \"Math.add\": [\"a\", \"b\"]\n"
                     "}");
    PT_CHECK(TsGenerator::method_params_json<>() == "{}");
    const std::string module = TsGenerator::generate_method_params<pt::MathController>();
    PT_CHECK(contains(module, "export const bosonMethodParams: Readonly<Record<string, readonly string[] | null>> = {"));
    PT_CHECK(contains(module, "\"Math.add\": [\"a\", \"b\"]\n};\n"));

    // Every method the fake window controller (and so the Wails Window shim) uses exists on the real
    // SystemController with the same parameter names.
    const std::string real = TsGenerator::method_params_json<boson::ipc::SystemController>();
    std::istringstream fake{TsGenerator::method_params_json<pt::FakeWindowController>()};
    int compared = 0;
    for (std::string line; std::getline(fake, line);) {
        if (!line.starts_with("    \"Boson.") || contains(line, "Boson.calls")) {
            continue;
        }
        if (line.ends_with(',')) {
            line.pop_back();
        }
        ++compared;
        if (!contains(real, line)) {
            std::cerr << "  SystemController lacks: " << line << "\n";
            PT_CHECK(false);
        }
    }
    PT_CHECK(compared == 10);
    PT_CHECK(contains(real, "\"Boson.set_min_size\": [\"width\", \"height\"]"));
    PT_CHECK(contains(real, "\"Boson.set_zoom\": [\"factor\"]"));
    PT_CHECK(contains(real, "\"Boson.set_always_on_top\": [\"enabled\"]"));

    // The C++ side of the generated-bindings round trip, through the Router.
    pt::TabService tabs_service;
    boson::ipc::Router<pt::TabService> router(tabs_service);
    std::string r = router.dispatch_sync(R"({"id":1,"method":"TabService.CreateTab","params":{"raw_url":"https://a","title":"A"}})");
    PT_CHECK(contains(r, R"("id":1,"url":"https://a","title":"A","muted":false,"tags":["new"],"meta":{"source":"wails"},"opener":null)"));
    r = router.dispatch_sync(R"({"id":2,"method":"TabService.CreateTab","params":{"raw_url":"https://b","title":"B"}})");
    PT_CHECK(contains(r, R"("opener":{"id":1,)"));
    r = router.dispatch_sync(R"({"id":3,"method":"TabService.Rename","params":{"tab":{"id":4,"url":"u","title":"t","muted":true,"tags":[],"meta":{},"opener":null},"title":"T2"}})");
    PT_CHECK(contains(r, R"("id":4,"url":"u","title":"T2","muted":true)"));
    r = router.dispatch_sync(R"({"id":4,"method":"TabService.SetMuted","params":{"id":9,"muted":true}})");
    PT_CHECK(contains(r, R"("status":"error")") && contains(r, "no tab 9"));
}

// ---- bench core ------------------------------------------------------------------------------------------

void test_bench_tree_and_stats() {
    step("bench: descendants, stats, report rendering");
    using boson::bench::ProcessEntry;
    const std::vector<ProcessEntry> entries{
        {.pid = 10, .parent_pid = 1, .exe = "app.exe"},
        {.pid = 20, .parent_pid = 10, .exe = "msedgewebview2.exe"},
        {.pid = 21, .parent_pid = 20, .exe = "msedgewebview2.exe"},
        {.pid = 22, .parent_pid = 20, .exe = "msedgewebview2.exe"},
        {.pid = 30, .parent_pid = 99, .exe = "other.exe"},
        {.pid = 40, .parent_pid = 22, .exe = "crashpad.exe"},
        {.pid = 50, .parent_pid = 50, .exe = "self-parent.exe"},
    };
    auto pids = boson::bench::descendants(entries, 10);
    PT_CHECK(pids.size() == 5 && pids.front() == 10);
    PT_CHECK(std::ranges::find(pids, 40) != pids.end() && std::ranges::find(pids, 30) == pids.end());
    pids = boson::bench::descendants(entries, 10, [](DWORD, DWORD child) { return child != 22; });
    PT_CHECK(pids.size() == 3); // 22 vetoed, so its child 40 is unreachable too
    PT_CHECK(boson::bench::descendants(entries, 50).size() == 1);

    const std::vector<double> sorted{1, 2, 3, 4};
    PT_CHECK(boson::bench::percentile_sorted(sorted, 50) == 2.5);
    PT_CHECK(boson::bench::percentile_sorted(sorted, 0) == 1 && boson::bench::percentile_sorted(sorted, 100) == 4);
    PT_CHECK(boson::bench::percentile_sorted({}, 50) == 0);
    const auto s = boson::bench::stats_of({4, 1, 3, 2});
    PT_CHECK(s.min == 1 && s.max == 4 && s.mean == 2.5 && s.last == 2 && s.p50 == 2.5);

    constexpr std::uint64_t kMb = 1024 * 1024;
    std::vector<boson::bench::Sample> samples{
        {.t_ms = 0, .phase = "startup", .process_count = 1, .private_working_set = 5 * kMb, .working_set = 9 * kMb, .commit = 6 * kMb, .handles = 50, .cpu_ms = 10},
        {.t_ms = 1000, .phase = "idle", .process_count = 6, .private_working_set = 100 * kMb, .working_set = 200 * kMb, .commit = 120 * kMb, .handles = 1000, .cpu_ms = 500},
        {.t_ms = 2000, .phase = "idle", .process_count = 6, .private_working_set = 110 * kMb, .working_set = 210 * kMb, .commit = 130 * kMb, .handles = 1010, .cpu_ms = 520},
        {.t_ms = 3000, .phase = "idle", .process_count = 7, .private_working_set = 120 * kMb, .working_set = 220 * kMb, .commit = 140 * kMb, .handles = 1020, .cpu_ms = 540},
    };
    const auto idle = boson::bench::summarize_phase(samples, "idle", 8);
    PT_CHECK(idle.samples == 3 && idle.duration_ms == 2000);
    PT_CHECK(idle.private_working_set_mb.mean == 110 && idle.commit_mb.max == 140);
    PT_CHECK(idle.process_count.max == 7 && idle.handles.mean == 1010);
    PT_CHECK(idle.cpu_ms == 40 && idle.cpu_percent_of_one_core == 2.0 && idle.cpu_percent_of_machine == 0.25);
    PT_CHECK(boson::bench::summarize_phase(samples, "load", 8).samples == 0);

    boson::bench::BenchReport report;
    report.timestamp = "2026-01-01T00:00:00Z";
    report.config.name = "unit";
    report.config.target = "C:\\x\\app.exe";
    report.machine.logical_cpus = 8;
    boson::bench::RunResult run;
    run.first_window_ms = 123.4;
    run.ready_ms = 250.0;
    run.phases.push_back(idle);
    run.samples = samples;
    report.runs.push_back(run);
    run.run = 2;
    run.first_window_ms = 200.0;
    report.runs.push_back(run);
    const std::string md = boson::bench::render_markdown(report);
    PT_CHECK(contains(md, "# boson_bench: unit"));
    PT_CHECK(contains(md, "| 1 | 123 | 250 | 110.0 | 130.0 | 1010 | 7 | 2.00 |"));
    PT_CHECK(contains(md, "| **median** | 162 |"));

    std::string json;
    PT_CHECK(!glz::write_json(report, json));
    boson::bench::BenchReport back;
    PT_CHECK(!glz::read<glz::opts{.error_on_unknown_keys = false}>(back, json));
    PT_CHECK(back.runs.size() == 2 && back.runs[0].first_window_ms == std::optional<double>{123.4});
    PT_CHECK(back.runs[0].phases.size() == 1 && back.runs[0].phases[0].private_working_set_mb.mean == 110);
    const std::vector<boson::bench::BenchReport> both{report, back};
    const std::string table = boson::bench::render_comparison(both);
    PT_CHECK(contains(table, "| unit | 2 | 162 | 250 | 110.0 | 130.0 | 1010 | 7 | 2.00 |"));
}

void test_bench_live_sampling() {
    step("bench: live process-tree sampling of this process and a child tree");
    boson::bench::UniqueHandle job{::CreateJobObjectW(nullptr, nullptr)};
    PT_CHECK(static_cast<bool>(job));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    // cmd.exe -> ping.exe : a two-level tree with no window.
    std::wstring cmd = L"cmd.exe /d /c ping -n 4 127.0.0.1 >nul";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL started = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    PT_CHECK(started != FALSE);
    if (started == FALSE) {
        return;
    }
    boson::bench::UniqueHandle process{pi.hProcess};
    boson::bench::UniqueHandle thread{pi.hThread};
    PT_CHECK(::AssignProcessToJobObject(job.get(), pi.hProcess) != FALSE);
    ::ResumeThread(thread.get());

    boson::bench::ProcessTreeSampler self_sampler(::GetCurrentProcessId());
    boson::bench::ProcessTreeSampler child_sampler(pi.dwProcessId, job.get());
    std::uint32_t max_self = 0;
    std::uint32_t max_child = 0;
    boson::bench::Sample last_self{};
    bool found_ping = false;
    const auto sees_ping = [&self_sampler]() {
        for (const auto& p : self_sampler.processes()) {
            if (_stricmp(p.name.c_str(), "PING.EXE") == 0) {
                return true;
            }
        }
        return false;
    };
    for (int i = 0; i < 60 && (!found_ping || max_child < 2); ++i) {
        last_self = self_sampler.sample(i * 50.0, "probe");
        const auto child = child_sampler.sample(i * 50.0, "probe");
        max_self = std::max(max_self, last_self.process_count);
        max_child = std::max(max_child, child.process_count);
        found_ping = found_ping || sees_ping();
        ::Sleep(50);
    }
    PT_CHECK(max_self >= 3);  // this test + cmd (+ conhost) + ping
    PT_CHECK(max_child >= 2); // cmd + ping
    PT_CHECK(last_self.private_working_set > 0 && last_self.commit > 0 && last_self.handles > 0);
    PT_CHECK(boson::bench::job_process_ids(job.get()).size() >= 1);
    if (!found_ping) {
        for (const auto& p : self_sampler.processes()) {
            std::cerr << "  tracked: " << p.pid << " " << p.name << "\n";
        }
    }
    PT_CHECK(found_ping);

    PT_CHECK(::WaitForSingleObject(process.get(), 15000) == WAIT_OBJECT_0);
    ::Sleep(200);
    const auto after = child_sampler.sample(5000, "probe");
    PT_CHECK(after.process_count == 0);
    PT_CHECK(after.cpu_ms >= 0.0);
    PT_CHECK(child_sampler.live_pids().empty());
}

// ---- WebView part ------------------------------------------------------------------------------------

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
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
    pump_until([&]() { return done; }, std::chrono::seconds{10});
    return result;
}

const std::string kPage =
    "<!doctype html><html><head><title>porting</title><script>window.__events=[];window.__responses=[];"
    "if(window.chrome&&chrome.webview){chrome.webview.addEventListener('message',e=>{const d=e.data;"
    "if(d&&d.type==='event'){window.__events.push(d);}else{window.__responses.push(d);}});}</script></head><body>P</body></html>";

/// Runs the bundled TypeScript shim self-test (tests/js/compat_selftest.ts) in webview when it was built.
void run_shim_selftest(boson::core::WebView& webview) {
    step("Electron / Tauri / Wails TypeScript shims in the WebView");
#ifdef BOSON_PORTING_SHIM_JS
    const std::filesystem::path bundle{BOSON_PORTING_SHIM_JS};
#else
    const std::filesystem::path bundle;
#endif
    std::error_code ec;
    if (bundle.empty() || !std::filesystem::exists(bundle, ec)) {
        std::cout << "SKIP (shim self-test): bundle not built (bun not found at configure time)\n";
        return;
    }
    std::ifstream in(bundle, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string js = ss.str();
    PT_CHECK(!js.empty());
    // Positional parameter names from C++ reflection, as boson_tsgen writes them to boson-method-params.ts.
    const std::string params = boson::ipc::TsGenerator::method_params_json<pt::TabService>();
    PT_CHECK(eval(webview, "window.__tsgenParams = " + params + ";1").has_value());
    PT_CHECK(eval(webview, js + "\n;1").has_value());
    PT_CHECK(pump_until([&]() { return eval(webview, "window.__compat && window.__compat.done").value_or("") == "true"; }, std::chrono::seconds{30}));
    const std::string failures = eval(webview, "JSON.stringify(window.__compat.failures)").value_or("?");
    const std::string passed = eval(webview, "window.__compat.passed").value_or("0");
    std::cout << "  shim self-test: " << passed << " JS assertions passed, failures: " << failures << "\n";
    PT_CHECK(failures == "\"[]\"");
    PT_CHECK(std::stoi(passed) == 60); // every expect() in compat_selftest.ts
}

void test_webview_roundtrip() {
    step("ElectronIpc over a real Bridge with two WebViews");
    {
        LPWSTR version = nullptr;
        if (FAILED(::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
            std::cout << "SKIP (WebView part): no WebView2 runtime installed\n";
            return;
        }
        ::CoTaskMemFree(version);
    }
    const std::filesystem::path user_data = std::filesystem::temp_directory_path() / (L"boson-porting-" + std::to_wstring(::GetCurrentProcessId()));
    {
        boson::core::WindowConfig window_cfg;
        window_cfg.title = L"Boson porting test";
        window_cfg.width = 640;
        window_cfg.height = 480;
        window_cfg.quit_on_destroy = false;
        auto window = std::make_unique<boson::core::Window>(window_cfg);
        PT_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

        boson::exec::Scheduler scheduler(2);
        boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
        window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
        boson::exec::TaskRunner runner(scheduler, marshal);

        boson::core::EnvironmentConfig env_cfg;
        env_cfg.user_data_folder = user_data.wstring();
        const auto env = boson::core::Environment::create(env_cfg);
        auto host = std::make_unique<boson::core::WebViewHost>(*window, env);
        host->set_default_asset_lookup([](std::string_view path) -> std::optional<std::span<const std::byte>> {
            if (path == "/index.html") {
                return std::as_bytes(std::span{kPage.data(), kPage.size()});
            }
            return std::nullopt;
        });
        bool a_nav = false;
        bool b_nav = false;
        boson::core::WebView& a = host->create(boson::core::WebViewConfig{});
        boson::core::WebView& b = host->create(boson::core::WebViewConfig{});
        a.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { a_nav = e.is_success; };
        b.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { b_nav = e.is_success; };
        host->set_layout([&a, &b](boson::core::WebViewHost&, const boson::core::LayoutContext& ctx) {
            a.set_bounds(RECT{0, 0, ctx.width() / 2, ctx.height()});
            b.set_bounds(RECT{ctx.width() / 2, 0, ctx.width(), ctx.height()});
        });
        PT_CHECK(pump_until([&]() { return a_nav && b_nav; }));

        pt::MathController math;
        ElectronIpc ipc;
        pt::TabService tab_service;
        pt::FakeWindowController fake_window;
        boson::ipc::Router<pt::MathController, ElectronIpc, pt::TabService, pt::FakeWindowController> router(math, ipc, tab_service, fake_window);
        auto bridge = std::make_unique<boson::ipc::Bridge>(runner, marshal, router);
        bridge->attach(a);
        bridge->attach(b);
        ipc.set_bridge(bridge.get());

        std::atomic<std::uint64_t> handler_sender{0};
        ipc.handle("whoami", [&](const IpcEvent& e, Json args) {
            handler_sender = e.sender_id;
            e.reply("pong", args[0].get_string(), e.sender_id);
            return Json(static_cast<double>(e.sender_id));
        });
        ipc.handle("fail", [](const IpcEvent&, Json) -> Json { throw std::runtime_error("handler exploded"); });
        ipc.handle("sum", [](const IpcEvent&, Json args) {
            double total = 0;
            for (const auto& v : args.get_array()) {
                total += v.get_number();
            }
            Json out;
            out["total"] = total;
            return out;
        });
        ipc.handle("greet", [](const IpcEvent&, Json args) { return Json("Hello, " + args[0]["name"].get_string() + "!"); });
        std::atomic<int> sent{0};
        ipc.on("fire", [&sent](const IpcEvent&, const Json&) { ++sent; });
        Json emitted{};
        std::atomic<bool> emitted_seen{false};
        ipc.on_event("ui:changed", [&](const IpcEvent&, const Json& args) {
            emitted = args[0];
            emitted_seen = true;
        });

        eval(a, "chrome.webview.postMessage({id:1,method:'ElectronIpc.invoke',params:{channel:'whoami',args:['hi']}});"
                "chrome.webview.postMessage({id:2,method:'ElectronIpc.invoke',params:{channel:'fail',args:[]}});"
                "chrome.webview.postMessage({id:3,method:'ElectronIpc.send',params:{channel:'fire',args:[1]}});"
                "chrome.webview.postMessage({id:4,method:'Math.add',params:{a:1,b:2}});1");
        PT_CHECK(pump_until([&]() { return eval(a, "window.__responses.length").value_or("0") == "4"; }, std::chrono::seconds{10}));
        PT_CHECK(handler_sender == a.id());
        PT_CHECK(sent == 1);
        const auto responses = eval(a, "JSON.stringify(window.__responses.sort((x,y)=>x.id-y.id))").value_or("");
        PT_CHECK(contains(responses, "handler exploded"));
        PT_CHECK(contains(responses, std::string{"\\\"data\\\":"} + std::to_string(a.id())));
        PT_CHECK(contains(responses, "\\\"sum\\\":3"));
        // The reply event went to the sender only.
        PT_CHECK(pump_until([&]() { return eval(a, "window.__events.length").value_or("0") == "1"; }, std::chrono::seconds{5}));
        const auto events_a = eval(a, "JSON.stringify(window.__events)").value_or("");
        PT_CHECK(contains(events_a, "ipc:pong") && contains(events_a, "hi"));
        PT_CHECK(eval(b, "window.__events.length") == std::optional<std::string>{"0"});
        PT_CHECK(eval(b, "window.__responses.length") == std::optional<std::string>{"0"});

        // webContents.send to one WebView, broadcast to all, frontend emit re-broadcast.
        PT_CHECK(ipc.send(b.id(), "only-b", 5));
        PT_CHECK(ipc.broadcast("all", std::string{"x"}));
        PT_CHECK(pump_until([&]() { return eval(b, "window.__events.length").value_or("0") == "2"; }, std::chrono::seconds{5}));
        PT_CHECK(contains(eval(b, "JSON.stringify(window.__events)").value_or(""), "ipc:only-b"));
        PT_CHECK(pump_until([&]() { return eval(a, "window.__events.length").value_or("0") == "2"; }, std::chrono::seconds{5}));
        PT_CHECK(!contains(eval(a, "JSON.stringify(window.__events)").value_or(""), "only-b"));

        eval(b, "chrome.webview.postMessage({id:9,method:'ElectronIpc.emit',params:{event:'ui:changed',payload:{v:3}}});1");
        PT_CHECK(pump_until([&]() { return emitted_seen.load() && eval(a, "window.__events.length").value_or("0") == "3"; }, std::chrono::seconds{10}));
        PT_CHECK(emitted_seen && emitted.is_object() && emitted["v"].get_number() == 3.0);
        PT_CHECK(contains(eval(a, "JSON.stringify(window.__events[2])").value_or(""), "ui:changed"));

        run_shim_selftest(a);

        bool shut_down = false;
        env->shutdown([&]() { shut_down = true; });
        PT_CHECK(pump_until([&]() { return shut_down; }, std::chrono::seconds{10}));
        ipc.set_bridge(nullptr);
        bridge.reset();
        host.reset();
        window.reset();
    }
    std::error_code ec;
    std::filesystem::remove_all(user_data, ec);
}

} // namespace

int main() {
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    test_electron_invoke();
    test_electron_send_and_listeners();
    test_sender_context();
    test_method_params();
    test_bench_tree_and_stats();
    test_bench_live_sampling();
    test_webview_roundtrip();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " porting checks passed\n";
    ::CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
