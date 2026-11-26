// IPC hot-path tests: UTF-8 <-> UTF-16 conversion, Router lookup + envelopes, UiMarshal wake coalescing / hooks,
// Bridge event batching. The headless part always runs; the WebView2 part skips when no runtime is installed.
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>
#include <boson/core/text_convert.hpp>

namespace hp {

struct NumReq {
    std::int64_t a = 0;
    std::int64_t b = 0;
};
struct NumRes {
    std::int64_t sum = 0;
};
struct TextReq {
    std::string text;
};
struct TextRes {
    std::string text;
};
/// Shape of the page-side summary (kSummaryScript); namespace-scope so glaze can reflect it.
struct Summary {
    int messages = 0;
    int arrays = 0;
    int events = 0;
    bool ordered = false;
    bool response_last = false;
};
struct EventData {
    std::int64_t n = 0;
};

class MathController {
public:
    BOSON_CONTROLLER("Math");
    [[nodiscard]] NumRes add(NumReq r) const { return NumRes{.sum = r.a + r.b}; }
    [[nodiscard]] NumRes zero() const { return NumRes{.sum = 0}; }
    void noop() const {}
    [[nodiscard]] TextRes echo(TextReq r) const { return TextRes{.text = std::move(r.text)}; }
    [[nodiscard]] TextRes boom() const { throw std::runtime_error("kaboom"); }
};

/// Same controller name and a clashing method: the first controller must win.
class MathShadow {
public:
    BOSON_CONTROLLER("Math");
    [[nodiscard]] NumRes add(NumReq) const { return NumRes{.sum = -1}; }
    [[nodiscard]] NumRes only_here(NumReq r) const { return NumRes{.sum = r.a * 100}; }
};

/// Runtime-only method set, like compat::ElectronIpc.
class DynController {
public:
    BOSON_CONTROLLER("Dyn");
    bool boson_dispatch(std::string_view method, std::uint64_t id, std::string_view params, std::string& out) noexcept {
        if (method != "hello") {
            return false;
        }
        out = boson::ipc::make_success_envelope(id, params.empty() ? std::string_view{"\"none\""} : params);
        return true;
    }
};

/// Emits a.a events from the pool thread, then returns: the response is queued after every event.
class EmitController {
public:
    BOSON_CONTROLLER("Emit");
    explicit EmitController(boson::ipc::Bridge*& bridge) : m_bridge(bridge) {}
    [[nodiscard]] NumRes burst(NumReq r) const {
        for (std::int64_t i = 0; i < r.a; ++i) {
            m_bridge->emit("hp:tick", EventData{.n = i});
        }
        return NumRes{.sum = r.a};
    }

private:
    boson::ipc::Bridge*& m_bridge;
};

} // namespace hp

template <>
struct glz::meta<hp::MathController> {
    using T = hp::MathController;
    static constexpr auto value = glz::object("add", &T::add, "zero", &T::zero, "noop", &T::noop, "echo", &T::echo, "boom", &T::boom);
};
template <>
struct glz::meta<hp::MathShadow> {
    using T = hp::MathShadow;
    static constexpr auto value = glz::object("add", &T::add, "only_here", &T::only_here);
};
template <>
struct glz::meta<hp::EmitController> {
    using T = hp::EmitController;
    static constexpr auto value = glz::object("burst", &T::burst);
};

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_ipc_hotpath.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}
#define HP_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) { std::cout << "[step] " << name << '\n' << std::flush; }

// ---- text conversion -------------------------------------------------------------------------------------

std::wstring reference_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string reference_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void test_text_convert() {
    step("text conversion matches MultiByteToWideChar / WideCharToMultiByte");
    std::mt19937 rng(12345);
    const std::vector<std::wstring_view> alphabets = {
        L"abcdefghijklmnopqrstuvwxyz0123456789{}[]\":,. ",
        L"abc\u00e9\u00fc\u4e2d\u6587{}\":",
        L"ab\U0001F600\U0001F680cd",
        L"\u0080\u07ff\u0800\uffff",
    };
    static constexpr std::size_t kLengths[] = {0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129,
                                               255, 256, 257, 767, 768, 769, 770, 1023, 1024, 4097, 70000};
    for (int round = 0; round < 400; ++round) {
        const std::wstring_view alpha = alphabets[static_cast<std::size_t>(round) % alphabets.size()];
        const std::size_t len = kLengths[static_cast<std::size_t>(round) % std::size(kLengths)] + static_cast<std::size_t>(round / 30);
        std::wstring wide;
        while (wide.size() < len) {
            wide.push_back(alpha[rng() % alpha.size()]);
        }
        const std::string utf8 = reference_to_utf8(wide);

        std::string got8;
        boson::core::wide_to_utf8_into(wide, got8);
        HP_CHECK(got8 == utf8);
        HP_CHECK(boson::core::wide_to_utf8(wide) == utf8);

        std::wstring got16;
        boson::core::utf8_to_wide_into(utf8, got16);
        HP_CHECK(got16 == reference_to_wide(utf8));
        HP_CHECK(boson::core::utf8_to_wide(utf8) == reference_to_wide(utf8));

        const boson::core::Utf8Text text{wide};
        HP_CHECK(text.view() == utf8);
        if (!text.view().empty()) {
            HP_CHECK(text.view().data()[text.view().size()] == '\0'); // the router may read the view as NUL-terminated
        }
    }
    {
        // ASCII for 5000 characters, then one non-ASCII character: SIMD and API chunks in one string.
        std::string utf8(5000, 'x');
        utf8 += "\xC3\xA9";
        std::wstring wide;
        boson::core::utf8_to_wide_into(utf8, wide);
        HP_CHECK(wide == reference_to_wide(utf8));
        std::string back;
        boson::core::wide_to_utf8_into(wide, back);
        HP_CHECK(back == utf8);
    }
    {
        const std::string bad = "ab\xFF\xFE" "cd\xC3";
        std::wstring got;
        boson::core::utf8_to_wide_into(bad, got);
        HP_CHECK(got == reference_to_wide(bad)); // invalid UTF-8 behaves like MultiByteToWideChar, never crashes
    }
    {
        // Nested Utf8Text (a handler that pumps messages) must not share the recycled buffer.
        const std::wstring big(5000, L'q');
        const boson::core::Utf8Text outer{big};
        {
            const std::wstring other(6000, L'z');
            const boson::core::Utf8Text inner{other};
            HP_CHECK(inner.view() == std::string(6000, 'z'));
        }
        HP_CHECK(outer.view() == std::string(5000, 'q'));
    }
}

// ---- router ----------------------------------------------------------------------------------------------

std::string write_reference_response(std::uint64_t id, std::string_view data_json) {
    boson::ipc::RpcResponse response{.id = id, .status = "success", .data = glz::raw_json{std::string{data_json}}};
    std::string out;
    [[maybe_unused]] const auto ec = glz::write_json(response, out);
    return out;
}

bool contains(std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; }

void test_router() {
    step("router: table lookup, envelopes, error paths");
    hp::MathController math;
    hp::MathShadow shadow;
    hp::DynController dyn;
    boson::ipc::Router<hp::MathController, hp::MathShadow, hp::DynController> router(math, shadow, dyn);

    HP_CHECK(router.dispatch_sync(R"({"id":7,"method":"Math.add","params":{"a":2,"b":40}})") == R"({"id":7,"status":"success","data":{"sum":42}})");
    HP_CHECK(router.dispatch_sync(R"({"id":8,"method":"Math.zero","params":{}})") == R"({"id":8,"status":"success","data":{"sum":0}})");
    HP_CHECK(router.dispatch_sync(R"({"id":9,"method":"Math.noop"})") == R"({"id":9,"status":"success","data":null})");
    HP_CHECK(router.dispatch_sync(R"({"id":9,"method":"Math.noop","params":null})") == R"({"id":9,"status":"success","data":null})");
    // First controller with a given name+method wins; later controllers still contribute their own methods.
    HP_CHECK(router.dispatch_sync(R"({"id":1,"method":"Math.add","params":{"a":1,"b":1}})") == R"({"id":1,"status":"success","data":{"sum":2}})");
    HP_CHECK(router.dispatch_sync(R"({"id":2,"method":"Math.only_here","params":{"a":3,"b":0}})") == R"({"id":2,"status":"success","data":{"sum":300}})");
    // Dynamic controllers are reached after the static table.
    HP_CHECK(router.dispatch_sync(R"({"id":3,"method":"Dyn.hello","params":{"x":1}})") == R"({"id":3,"status":"success","data":{"x":1}})");
    HP_CHECK(contains(router.dispatch_sync(R"({"id":3,"method":"Dyn.nope","params":{}})"), "-32601"));

    HP_CHECK(contains(router.dispatch_sync(R"({"id":4,"method":"Math.nope","params":{}})"), "\"code\":-32601"));
    HP_CHECK(contains(router.dispatch_sync(R"({"id":4,"method":"Nope.add","params":{}})"), "\"code\":-32601"));
    HP_CHECK(contains(router.dispatch_sync(R"({"id":4,"method":"MathAdd","params":{}})"), "expected Controller.method"));
    HP_CHECK(contains(router.dispatch_sync(R"({"id":4,"method":"Math.add","params":{"a":"x"}})"), "\"code\":-32602"));
    HP_CHECK(contains(router.dispatch_sync(R"({"id":5,"method":"Math.boom","params":{}})"), "kaboom"));
    HP_CHECK(contains(router.dispatch_sync(R"({"id":5,"method":"Math.boom","params":{}})"), "\"id\":5"));
    HP_CHECK(contains(router.dispatch_sync("not json"), "\"code\":-32700"));
    HP_CHECK(contains(router.dispatch_sync(R"({"id":4,"method":"Math.add","params":{"a":1,"b":2},"extra":1})"), "\"code\":-32700"));

    // An escaped method name cannot be viewed; the owning fallback still routes it.
    HP_CHECK(router.dispatch_sync(R"({"id":6,"method":"Math\u002eadd","params":{"a":5,"b":5}})") == R"({"id":6,"status":"success","data":{"sum":10}})");

    {
        // A 3 MiB string with escapes and non-ASCII round-trips unchanged (params view -> arg -> result -> envelope).
        const std::string unit = "line1\n\"quoted\" \\ back\t\xC3\xA9\xE4\xB8\xAD";
        std::string text;
        while (text.size() < 3 * 1024 * 1024) {
            text += unit; // whole code points only: glaze rejects invalid UTF-8
        }
        std::string text_json;
        HP_CHECK(!glz::write_json(text, text_json));
        const std::string response = router.dispatch_sync(R"({"id":11,"method":"Math.echo","params":{"text":)" + text_json + "}}");
        const std::string expected = write_reference_response(11, R"({"text":)" + text_json + "}");
        HP_CHECK(response == expected);
        if (response != expected) {
            std::size_t at = 0;
            while (at < response.size() && at < expected.size() && response[at] == expected[at]) {
                ++at;
            }
            std::cerr << "  response head: " << response.substr(0, 200) << '\n';
            std::cerr << "  first difference at " << at << " of " << response.size() << "/" << expected.size() << ": got '"
                      << response.substr(at, 40) << "' expected '" << expected.substr(at, 40) << "'\n";
        }
    }

    // The params view is not NUL-terminated inside the message: a bare number still parses.
    HP_CHECK(router.dispatch_sync(R"({"id":12,"method":"Dyn.hello","params":5})") == R"({"id":12,"status":"success","data":5})");
    {
        const std::string frame = std::string{R"({"id":13,"method":"Math.add","params":{"a":4,"b":4}})"} + "XXXX";
        HP_CHECK(router.dispatch_sync(std::string_view{frame}.substr(0, frame.size() - 4)) == R"({"id":13,"status":"success","data":{"sum":8}})");
    }
}

void test_envelopes() {
    step("envelopes are byte-identical to the glaze output");
    for (const std::uint64_t id : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{18446744073709551615ULL}}) {
        for (const std::string_view data : {std::string_view{"null"}, std::string_view{R"({"a":[1,2,{"b":"c"}]})"}, std::string_view{"\"s\""}}) {
            HP_CHECK(boson::ipc::make_success_envelope(id, data) == write_reference_response(id, data));
        }
    }
    HP_CHECK(boson::ipc::make_success_envelope(3, "") == R"({"id":3,"status":"success","data":null})");
    HP_CHECK(boson::ipc::make_event_envelope("a:b", "{\"x\":1}") == R"({"type":"event","event":"a:b","data":{"x":1}})");
    HP_CHECK(boson::ipc::make_event_envelope("a:b", "") == R"({"type":"event","event":"a:b","data":null})");
    HP_CHECK(boson::ipc::make_event_envelope("we\"ird\\name\n", "1") == R"({"type":"event","event":"we\"ird\\name\n","data":1})");
    glz::generic parsed;
    HP_CHECK(!glz::read_json(parsed, boson::ipc::make_event_envelope("n\"x", "[1]")));
}

void test_async_dispatch() {
    step("dispatch_async: parse on the worker, reply on the UI thread");
    hp::MathController math;
    boson::ipc::Router<hp::MathController> router(math);
    boson::exec::Scheduler scheduler(2);
    std::atomic<int> wakes{0};
    boson::exec::UiMarshal marshal([&]() { ++wakes; });
    boson::exec::TaskRunner runner(scheduler, marshal);

    std::vector<std::string> replies;
    constexpr int kCalls = 200;
    for (int i = 0; i < kCalls; ++i) {
        router.dispatch_async(
            R"({"id":)" + std::to_string(i) + R"(,"method":"Math.add","params":{"a":)" + std::to_string(i) + R"(,"b":1}})", runner,
            [&replies](std::string response) { replies.push_back(std::move(response)); });
    }
    router.dispatch_async("garbage", runner, [&replies](std::string response) { replies.push_back(std::move(response)); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (replies.size() < kCalls + 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        marshal.drain_on_ui_thread();
    }
    HP_CHECK(replies.size() == kCalls + 1);
    std::vector<bool> seen(kCalls, false);
    int errors = 0;
    for (const std::string& reply : replies) {
        glz::generic parsed;
        if (glz::read_json(parsed, reply)) {
            continue;
        }
        const auto& object = parsed.get_object();
        if (object.at("status").get_string() == "error") {
            ++errors;
            continue;
        }
        const int id = static_cast<int>(object.at("id").get_number());
        const int sum = static_cast<int>(object.at("data").get_object().at("sum").get_number());
        HP_CHECK(sum == id + 1);
        seen[static_cast<std::size_t>(id)] = true;
    }
    HP_CHECK(errors == 1);
    HP_CHECK(std::ranges::all_of(seen, [](bool b) { return b; }));
    HP_CHECK(wakes.load() >= 1);
}

// ---- UiMarshal -------------------------------------------------------------------------------------------

void test_ui_marshal() {
    step("UiMarshal: one wake per batch, FIFO order, drain hooks");
    int wakes = 0;
    boson::exec::UiMarshal marshal([&]() { ++wakes; });
    std::string order;
    std::vector<std::string> delivered;
    marshal.set_deliver_callback([&](std::string_view payload) { delivered.emplace_back(payload); });

    marshal.post_task([&]() { order += 'a'; });
    marshal.enqueue("p1");
    marshal.post_reply([&](std::string payload) { order += payload; }, "b");
    marshal.post_task([&]() { order += 'c'; });
    marshal.enqueue("p2");
    HP_CHECK(wakes == 1); // five posts, one wake
    HP_CHECK(marshal.pending_count() == 5);

    int hook_runs = 0;
    std::size_t pending_in_hook = 99;
    marshal.add_drain_hook(&order, [&]() {
        ++hook_runs;
        pending_in_hook = marshal.pending_count();
    });
    marshal.drain_on_ui_thread();
    HP_CHECK(order == "abc");
    HP_CHECK((delivered == std::vector<std::string>{"p1", "p2"}));
    HP_CHECK(marshal.pending_count() == 0);
    HP_CHECK(hook_runs == 1 && pending_in_hook == 0);

    marshal.post_task([]() {});
    HP_CHECK(wakes == 2); // drained: the next post wakes again
    marshal.post_task([]() {});
    HP_CHECK(wakes == 2);
    marshal.drain_on_ui_thread();

    // A task that posts from inside the drain re-arms the wake.
    const int w0 = wakes;
    marshal.post_task([&]() { marshal.post_task([&]() { order += 'n'; }); });
    HP_CHECK(wakes == w0 + 1);
    marshal.drain_on_ui_thread();
    HP_CHECK(wakes == w0 + 2);
    marshal.drain_on_ui_thread();
    HP_CHECK(order.back() == 'n');

    const int runs_before_remove = hook_runs;
    marshal.remove_drain_hook(&order);
    marshal.post_task([]() {});
    marshal.drain_on_ui_thread();
    HP_CHECK(hook_runs == runs_before_remove);

    // Many producers: nothing lost, wakes far fewer than posts.
    std::atomic<int> ran{0};
    std::atomic<int> w{0};
    boson::exec::UiMarshal m([&]() { ++w; });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 5000; ++i) {
                m.post_task([&]() { ++ran; });
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    m.drain_on_ui_thread();
    HP_CHECK(ran.load() == 20000);
    HP_CHECK(w.load() == 1);
}

// ---- WebView2: Bridge event batching ---------------------------------------------------------------------

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
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
    pump_until([&]() { return done; }, std::chrono::seconds{10});
    return result;
}

// Records every raw message: window.__raw = [JSON string per message].
constexpr std::string_view kPage =
    "<!doctype html><html><head><title>hp</title><script>window.__raw=[];"
    "chrome.webview.addEventListener('message',e=>{window.__raw.push(JSON.stringify(e.data));});"
    "</script></head><body>hp</body></html>";

constexpr std::string_view kSummaryScript =
    "(()=>{const msgs=window.__raw.map(s=>JSON.parse(s));const flat=[];let arrays=0;"
    "for(const m of msgs){if(Array.isArray(m)){arrays++;flat.push(...m);}else flat.push(m);}"
    "const ev=flat.filter(x=>x&&x.type==='event').map(x=>x.data.n);"
    "const ri=flat.findIndex(x=>x&&x.id===5);"
    "return {messages:msgs.length,arrays:arrays,events:ev.length,ordered:ev.every((v,i)=>v===i),response_last:ri===flat.length-1&&ri>=0};})()";

std::optional<hp::Summary> summarize(boson::core::WebView& webview) {
    const auto text = eval(webview, kSummaryScript);
    hp::Summary summary;
    if (!text.has_value() || glz::read_json(summary, *text)) {
        return std::nullopt;
    }
    return summary;
}

void test_bridge_batching() {
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
        std::cout << "SKIP: no WebView2 runtime installed (event batching not tested)\n";
        return;
    }
    CoTaskMemFree(version);
    step("Bridge: event batching in a real WebView2");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path user_data = std::filesystem::path(temp) / (L"boson-hp-" + std::to_wstring(GetCurrentProcessId()));

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson IPC hot-path test";
    window_cfg.width = 400;
    window_cfg.height = 300;
    window_cfg.quit_on_destroy = false;
    auto window = std::make_unique<boson::core::Window>(window_cfg);
    HP_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
    window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = user_data.wstring();
    const auto env = boson::core::Environment::create(env_cfg);
    auto host = std::make_unique<boson::core::WebViewHost>(*window, env);

    bool loaded = false;
    boson::core::WebViewConfig cfg;
    cfg.initial_url = "boson://app/index.html";
    boson::core::WebView& webview = host->create(
        cfg,
        boson::core::WebViewCallbacks{
            .asset_lookup = [](std::string_view path) -> std::optional<std::span<const std::byte>> {
                if (path == "/index.html") {
                    return std::as_bytes(std::span{kPage.data(), kPage.size()});
                }
                return std::nullopt;
            },
        });
    webview.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { loaded = e.is_success; };
    HP_CHECK(pump_until([&]() { return webview.is_ready() && loaded; }));

    boson::ipc::Bridge* bridge_ptr = nullptr;
    hp::EmitController controller(bridge_ptr);
    boson::ipc::Router<hp::EmitController> router(controller);
    boson::ipc::Bridge bridge(runner, marshal, router);
    bridge_ptr = &bridge;

    const auto run_burst = [&](bool batch) -> std::optional<hp::Summary> {
        bridge.attach(webview, boson::ipc::BridgeAttachOptions{.batch_events = batch});
        eval(webview, "window.__raw.length=0; 1");
        eval(webview, R"(chrome.webview.postMessage({id:5,method:'Emit.burst',params:{a:200,b:0}}); 1)");
        pump_until([&]() {
            const auto n = eval(webview, "window.__raw.length");
            const auto s = summarize(webview);
            return s.has_value() && s->response_last && s->events == 200 && n.has_value();
        }, std::chrono::seconds{15});
        return summarize(webview);
    };

    const auto batched = run_burst(true);
    HP_CHECK(batched.has_value());
    if (batched) {
        std::cout << "  batched:   " << batched->messages << " messages (" << batched->arrays << " arrays) for 200 events + 1 response\n";
        HP_CHECK(batched->events == 200);
        HP_CHECK(batched->ordered);        // events arrive in emit order
        HP_CHECK(batched->response_last);  // the response never overtakes events emitted before it
        HP_CHECK(batched->arrays >= 1);
        HP_CHECK(batched->messages < 201); // fewer postMessage deliveries than events + response
    }
    const auto plain = run_burst(false);
    HP_CHECK(plain.has_value());
    if (plain) {
        std::cout << "  unbatched: " << plain->messages << " messages (" << plain->arrays << " arrays)\n";
        HP_CHECK(plain->events == 200 && plain->ordered && plain->response_last);
        HP_CHECK(plain->arrays == 0);      // default: one message per event, exactly the old wire format
        HP_CHECK(plain->messages == 201);
    }

    bridge.detach(webview);
    host.reset();
    window.reset();
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    test_text_convert();
    test_envelopes();
    test_router();
    test_async_dispatch();
    test_ui_marshal();
    test_bridge_batching();
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
