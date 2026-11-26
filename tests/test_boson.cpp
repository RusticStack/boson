#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include <glaze/glaze.hpp>

#include <boson/app/lifecycle.hpp>
#include <boson/core/environment.hpp>
#include <boson/core/ui_timer.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>
#include <boson/ipc/bridge.hpp>

#include <boson/assets/asset_registry.hpp>
#include <boson/core/inplace_vector.hpp>
#include <boson/core/protocol.hpp>
#include <boson/exec/scheduler.hpp>
#include <boson/exec/task_runner.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/protocol_types.hpp>
#include <boson/ipc/router.hpp>
#include <boson/ipc/system_controller.hpp>
#include <boson/ipc/ts_generator.hpp>

#include "../examples/demo/app_controller.hpp"
#include "../examples/demo/demo_events.hpp"

namespace {

int g_failures = 0;

void check(bool condition, std::string_view expr, const char* file, int line) {
    if (!condition) {
        std::cerr << "[FAIL] " << file << ":" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define BOSON_CHECK(cond) check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

void test_ipc_router() {
    boson::demo::AppController app;
    boson::ipc::Router<boson::demo::AppController> router(app);

    // 1. Valid call: App.ping
    {
        const std::string raw = router.dispatch_sync(R"({"id":1,"method":"App.ping","params":null})");
        boson::ipc::RpcResponse resp{};
        const auto ec = glz::read_json(resp, raw);
        BOSON_CHECK(!ec);
        BOSON_CHECK(resp.id == 1);
        BOSON_CHECK(resp.status == "success");
        BOSON_CHECK(resp.data.has_value());
        BOSON_CHECK(!resp.error.has_value());

        boson::demo::PingResult ping{};
        const auto data_ec = glz::read_json(ping, resp.data->str);
        BOSON_CHECK(!data_ec);
        BOSON_CHECK(ping.pong);
        BOSON_CHECK(ping.timestamp_us > 0);
    }

    // 2. Valid call: App.get_system_info
    {
        const std::string raw = router.dispatch_sync(R"({"id":2,"method":"App.get_system_info","params":null})");
        boson::ipc::RpcResponse resp{};
        const auto ec = glz::read_json(resp, raw);
        BOSON_CHECK(!ec);
        BOSON_CHECK(resp.id == 2);
        BOSON_CHECK(resp.status == "success");
        BOSON_CHECK(resp.data.has_value());

        boson::demo::SystemInfo info{};
        const auto data_ec = glz::read_json(info, resp.data->str);
        BOSON_CHECK(!data_ec);
        BOSON_CHECK(!info.os.empty());
        BOSON_CHECK(!info.architecture.empty());
        BOSON_CHECK(info.cpu_cores >= 1);
    }

    // 3. Valid call: App.compute_primes
    {
        const std::string raw = router.dispatch_sync(R"({"id":3,"method":"App.compute_primes","params":{"limit":100}})");
        boson::ipc::RpcResponse resp{};
        const auto ec = glz::read_json(resp, raw);
        BOSON_CHECK(!ec);
        BOSON_CHECK(resp.id == 3);
        BOSON_CHECK(resp.status == "success");
        BOSON_CHECK(resp.data.has_value());

        boson::demo::ComputePrimesResult primes{};
        const auto data_ec = glz::read_json(primes, resp.data->str);
        BOSON_CHECK(!data_ec);
        BOSON_CHECK(primes.limit == 100);
        BOSON_CHECK(primes.count == 25);
        BOSON_CHECK(primes.largest_prime == 97);
    }

    // 4. Unknown method error
    {
        const std::string raw = router.dispatch_sync(R"({"id":4,"method":"App.does_not_exist","params":null})");
        boson::ipc::RpcResponse resp{};
        const auto ec = glz::read_json(resp, raw);
        BOSON_CHECK(!ec);
        BOSON_CHECK(resp.id == 4);
        BOSON_CHECK(resp.status == "error");
        BOSON_CHECK(resp.error.has_value());
        BOSON_CHECK(resp.error->code == -32601);
    }

    // 5. Malformed JSON error
    {
        const std::string raw = router.dispatch_sync(R"({"id":5, broken json)");
        boson::ipc::RpcResponse resp{};
        const auto ec = glz::read_json(resp, raw);
        BOSON_CHECK(!ec);
        BOSON_CHECK(resp.status == "error");
        BOSON_CHECK(resp.error.has_value());
        BOSON_CHECK(resp.error->code == -32700);
    }

    // 6. C++ exception caught into JSON error envelope
    {
        const std::string raw = router.dispatch_sync(R"({"id":6,"method":"App.trigger_error","params":null})");
        boson::ipc::RpcResponse resp{};
        const auto ec = glz::read_json(resp, raw);
        BOSON_CHECK(!ec);
        BOSON_CHECK(resp.id == 6);
        BOSON_CHECK(resp.status == "error");
        BOSON_CHECK(resp.error.has_value());
        BOSON_CHECK(resp.error->code == -32000);
        BOSON_CHECK(resp.error->message.find("Simulated C++ exception") != std::string::npos);
    }
}

void test_async_exec_and_ui_marshal() {
    const std::thread::id caller_tid = std::this_thread::get_id();

    std::mutex mu;
    std::condition_variable cv;
    bool woken = false;
    std::vector<std::string> delivered;

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal(
        [&]() {
            std::scoped_lock lock(mu);
            woken = true;
            cv.notify_one();
        },
        [&](std::string_view payload) {
            delivered.emplace_back(payload);
        });

    boson::exec::TaskRunner runner(scheduler, marshal);

    std::atomic<bool> ran_on_worker{false};
    std::thread::id worker_tid{};

    boson::demo::AppController app;
    boson::ipc::Router<boson::demo::AppController> router(app);

    runner.dispatch(42, [&]() -> std::string {
        worker_tid = std::this_thread::get_id();
        ran_on_worker.store(worker_tid != caller_tid, std::memory_order_release);
        return router.dispatch_sync(R"({"id":42,"method":"App.ping","params":null})");
    });

    {
        std::unique_lock lock(mu);
        const bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&]() { return woken; });
        BOSON_CHECK(ok);
    }

    BOSON_CHECK(ran_on_worker.load(std::memory_order_acquire));
    BOSON_CHECK(worker_tid != caller_tid);
    BOSON_CHECK(marshal.pending_count() == 1);

    marshal.drain_on_ui_thread();
    BOSON_CHECK(marshal.pending_count() == 0);
    BOSON_CHECK(delivered.size() == 1);

    if (!delivered.empty()) {
        boson::ipc::RpcResponse resp{};
        const auto ec = glz::read_json(resp, delivered.front());
        BOSON_CHECK(!ec);
        BOSON_CHECK(resp.id == 42);
        BOSON_CHECK(resp.status == "success");
    }
}

void test_inplace_vector() {
    boson::inplace_vector<int, 8> vec;
    BOSON_CHECK(vec.empty());
    BOSON_CHECK(vec.capacity() == 8);
    BOSON_CHECK(vec.max_size() == 8);

    vec.push_back(10);
    vec.emplace_back(20);
    BOSON_CHECK(vec.size() == 2);
    BOSON_CHECK(vec[0] == 10);
    BOSON_CHECK(vec[1] == 20);

    for (int i = 2; i < 8; ++i) {
        vec.push_back(i * 10);
    }
    BOSON_CHECK(vec.size() == 8);
    BOSON_CHECK(vec.back() == 70);

    BOSON_CHECK(vec.try_push_back(999) == nullptr);
    BOSON_CHECK(vec.try_emplace_back(999) == nullptr);

    bool threw_bad_alloc = false;
    try {
        vec.push_back(999);
    } catch (const std::bad_alloc&) {
        threw_bad_alloc = true;
    }
    BOSON_CHECK(threw_bad_alloc);
    BOSON_CHECK(vec.size() == 8);

    vec.pop_back();
    BOSON_CHECK(vec.size() == 7);
    vec.clear();
    BOSON_CHECK(vec.empty());
}

void test_assets_and_protocol() {
    const auto root = boson::assets::AssetRegistry::find("/");
    const auto index_html = boson::assets::AssetRegistry::find("index.html");
    const auto index_js = boson::assets::AssetRegistry::find("/assets/index.js");
    const auto index_css = boson::assets::AssetRegistry::find("/assets/index.css");
    const auto missing = boson::assets::AssetRegistry::find("/does_not_exist.txt");

    BOSON_CHECK(root.has_value() && !root->empty());
    BOSON_CHECK(index_html.has_value() && !index_html->empty());
    BOSON_CHECK(index_js.has_value() && !index_js->empty());
    BOSON_CHECK(index_css.has_value() && !index_css->empty());
    BOSON_CHECK(!missing.has_value());

    const auto html_mime = boson::core::ProtocolHandler::mime_type_for("index.html");
    const auto js_mime = boson::core::ProtocolHandler::mime_type_for("/assets/index.js");
    const auto css_mime = boson::core::ProtocolHandler::mime_type_for("/assets/index.css");

    BOSON_CHECK(html_mime.find("text/html") != std::string_view::npos);
    BOSON_CHECK(js_mime.find("javascript") != std::string_view::npos);
    BOSON_CHECK(css_mime.find("text/css") != std::string_view::npos);
}

void test_ts_generator() {
    const std::string dts = boson::ipc::TsGenerator::generate<
        boson::ipc::SystemController,
        boson::demo::AppController>();

    BOSON_CHECK(dts.find("export interface PingResult") != std::string::npos);
    BOSON_CHECK(dts.find("export interface SystemInfo") != std::string::npos);
    BOSON_CHECK(dts.find("export interface ComputePrimesRequest") != std::string::npos);
    BOSON_CHECK(dts.find("export interface ComputePrimesResult") != std::string::npos);
    BOSON_CHECK(dts.find("export interface BackdropRequest") != std::string::npos);
    BOSON_CHECK(dts.find("export interface NotifyRequest") != std::string::npos);
    BOSON_CHECK(dts.find("export interface FileDialogResponse") != std::string::npos);
    BOSON_CHECK(dts.find("export interface BosonApi") != std::string::npos);
    BOSON_CHECK(dts.find("\"Boson.start_drag\"") != std::string::npos);
    BOSON_CHECK(dts.find("\"Boson.set_backdrop\"") != std::string::npos);
    BOSON_CHECK(dts.find("\"Boson.notify\"") != std::string::npos);
    BOSON_CHECK(dts.find("\"Boson.open_file_dialog\"") != std::string::npos);
    BOSON_CHECK(dts.find("\"App.ping\"") != std::string::npos);
    BOSON_CHECK(dts.find("\"App.get_system_info\"") != std::string::npos);
    BOSON_CHECK(dts.find("\"App.compute_primes\"") != std::string::npos);
    BOSON_CHECK(dts.find("\"App.trigger_error\"") != std::string::npos);
    BOSON_CHECK(dts.find("Promise<") != std::string::npos);
}

void test_ui_marshal_tasks_and_backdrops() {
    std::mutex mu;
    std::condition_variable cv;
    int wake_count = 0;

    boson::exec::UiMarshal marshal([&]() {
        std::scoped_lock lock(mu);
        ++wake_count;
        cv.notify_all();
    });

    std::atomic<int> sum{0};
    std::jthread worker([&](std::stop_token) {
        const int result = marshal.invoke_sync([&]() {
            sum.fetch_add(21, std::memory_order_relaxed);
            return 42;
        });
        BOSON_CHECK(result == 42);
    });

    {
        std::unique_lock lock(mu);
        const bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&]() { return wake_count >= 1; });
        BOSON_CHECK(ok);
    }

    marshal.drain_on_ui_thread();
    worker.join();
    BOSON_CHECK(sum.load(std::memory_order_relaxed) == 21);

    BOSON_CHECK(boson::ipc::parse_backdrop_type("mica") == boson::core::BackdropType::Mica);
    BOSON_CHECK(boson::ipc::parse_backdrop_type("mica_alt") == boson::core::BackdropType::MicaAlt);
    BOSON_CHECK(boson::ipc::parse_backdrop_type("acrylic") == boson::core::BackdropType::Acrylic);
    BOSON_CHECK(boson::ipc::parse_backdrop_type("transparent") == boson::core::BackdropType::Transparent);
    BOSON_CHECK(boson::ipc::parse_backdrop_type("translucent") == boson::core::BackdropType::Translucent);
    BOSON_CHECK(boson::ipc::parse_backdrop_type("none") == boson::core::BackdropType::None);

    bool threw = false;
    try {
        (void)boson::ipc::parse_backdrop_type("invalid_backdrop");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    BOSON_CHECK(threw);
}

void test_browser_arguments() {
    using boson::core::EnvironmentConfig;
    using boson::core::build_browser_arguments;

    {
        EnvironmentConfig cfg;
        BOSON_CHECK(build_browser_arguments(cfg).empty());
    }
    {
        // Several feature switches (inline and in lists) collapse into one switch each, order preserved.
        EnvironmentConfig cfg;
        cfg.additional_browser_arguments = {
            L"--autoplay-policy=no-user-gesture-required",
            L"--enable-features=HardwareSecureDecryption,HardwareSecureDecryptionExperiment",
            L"--disable-gpu --enable-features=PlatformEncryptedDolbyVision",
            L"--disable-features=msSmartScreenProtection",
            L"--disable-gpu",
        };
        cfg.enable_features = {L"HardwareSecureDecryption", L"Foo:param/1"};
        cfg.disable_features = {L"EdgePlayReadyDrmWin10"};
        const std::wstring args = build_browser_arguments(cfg);
        BOSON_CHECK(args ==
            L"--autoplay-policy=no-user-gesture-required --disable-gpu "
            L"--enable-features=HardwareSecureDecryption,HardwareSecureDecryptionExperiment,PlatformEncryptedDolbyVision,Foo:param/1 "
            L"--disable-features=msSmartScreenProtection,EdgePlayReadyDrmWin10");
    }
    {
        // Disable wins when a feature is in both lists; draggable-regions flag adds its feature.
        EnvironmentConfig cfg;
        cfg.enable_features = {L"A,B", L"C"};
        cfg.disable_features = {L"B"};
        cfg.enable_draggable_regions = true;
        BOSON_CHECK(build_browser_arguments(cfg) == L"--enable-features=A,C,msWebView2EnableDraggableRegions --disable-features=B");
    }
    {
        // Quoted values survive splitting.
        EnvironmentConfig cfg;
        cfg.additional_browser_arguments = {L"--user-agent=\"Boson Test\" --lang=pt-PT"};
        BOSON_CHECK(build_browser_arguments(cfg) == L"--user-agent=\"Boson Test\" --lang=pt-PT");
    }
    BOSON_CHECK(boson::core::to_string(boson::core::ProcessKind::Gpu) == "gpu");
}

void test_bridge_origins_and_events() {
    using boson::ipc::Bridge;
    BOSON_CHECK(Bridge::origin_of("boson://app/index.html") == "boson://app");
    BOSON_CHECK(Bridge::origin_of("HTTP://LocalHost:5173/src/main.ts?x=1") == "http://localhost:5173");
    BOSON_CHECK(Bridge::origin_of("https://user:pw@example.com/a") == "https://example.com");
    BOSON_CHECK(Bridge::origin_of("about:blank").empty());
    BOSON_CHECK(Bridge::origin_of("data:text/html,hi").empty());

    const std::vector<std::string> allowed{"boson://app", "http://localhost:5173/"};
    BOSON_CHECK(Bridge::is_origin_allowed("boson://app/index.html", allowed));
    BOSON_CHECK(Bridge::is_origin_allowed("http://localhost:5173/", allowed));
    BOSON_CHECK(!Bridge::is_origin_allowed("https://evil.example/boson://app", allowed));
    BOSON_CHECK(!Bridge::is_origin_allowed("boson://application/", allowed));
    BOSON_CHECK(!Bridge::is_origin_allowed("about:blank", allowed));
    const std::vector<std::string> any{"*"};
    BOSON_CHECK(Bridge::is_origin_allowed("https://anything.example", any));

    const std::string envelope = boson::ipc::make_event_envelope("tabs:updated", R"({"count":3})");
    BOSON_CHECK(envelope == R"({"type":"event","event":"tabs:updated","data":{"count":3}})");
    BOSON_CHECK(boson::ipc::make_event_envelope("x", "") == R"({"type":"event","event":"x","data":null})");
    BOSON_CHECK(boson::ipc::make_event_envelope("a\"b", "1").find(R"("event":"a\"b")") != std::string::npos);
}

void test_ts_generator_events() {
    const std::string dts = boson::ipc::TsGenerator::generate_with_events<
        boson::demo::DemoEvents,
        boson::ipc::SystemController,
        boson::demo::AppController>();
    BOSON_CHECK(dts.find("export interface BosonEvents {") != std::string::npos);
    BOSON_CHECK(dts.find("\"app:tick\": TickEvent;") != std::string::npos);
    BOSON_CHECK(dts.find("\"window:state\": WindowStateEvent;") != std::string::npos);
    BOSON_CHECK(dts.find("export interface TickEvent") != std::string::npos);
    BOSON_CHECK(dts.find("\"Boson.set_maximize_button_rect\": (params: ElementRectRequest)") != std::string::npos);
    BOSON_CHECK(dts.find("\"Boson.go_back\"") != std::string::npos);

    // Without an event map the interface still exists so boson.ts always type-checks.
    const std::string plain = boson::ipc::TsGenerator::generate<boson::demo::AppController>();
    BOSON_CHECK(plain.find("export interface BosonEvents {\n}") != std::string::npos);
}

void test_command_line_quoting() {
    using boson::app::quote_argument;
    const std::vector<std::wstring> samples{
        L"plain",
        L"with space",
        L"",
        L"quote\"inside",
        L"trailing\\",
        L"C:\\Program Files\\App\\",
        L"back\\\\\"slash",
        L"--boson-wait-pid=1234",
        L"tab\there",
    };
    const std::wstring command_line = boson::app::build_command_line(L"C:\\Apps\\My App\\app.exe", samples);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(command_line.c_str(), &argc);
    BOSON_CHECK(argv != nullptr);
    if (argv != nullptr) {
        BOSON_CHECK(argc == static_cast<int>(samples.size()) + 1);
        BOSON_CHECK(std::wstring_view{argv[0]} == L"C:\\Apps\\My App\\app.exe");
        for (int i = 1; i < argc && i <= static_cast<int>(samples.size()); ++i) {
            BOSON_CHECK(std::wstring_view{argv[i]} == samples[static_cast<std::size_t>(i - 1)]);
        }
        LocalFree(argv);
    }
    BOSON_CHECK(quote_argument(L"simple") == L"simple");

    const std::vector<std::wstring> args{L"--x", L"--boson-wait-pid=4321"};
    BOSON_CHECK(boson::app::parse_wait_pid(args) == std::optional<std::uint32_t>{4321});
    const std::vector<std::wstring> bad{L"--boson-wait-pid=12a"};
    BOSON_CHECK(!boson::app::parse_wait_pid(bad).has_value());
    BOSON_CHECK(!boson::app::parse_wait_pid({}).has_value());
    BOSON_CHECK(boson::app::wait_for_previous_instance(std::chrono::milliseconds{10})); // no switch: no wait
    BOSON_CHECK(!boson::app::executable_path().empty());
}

void test_single_instance() {
    const std::wstring app_id = L"Boson.Tests." + std::to_wstring(GetCurrentProcessId());
    boson::app::SingleInstance primary(app_id);
    BOSON_CHECK(primary.is_primary());

    std::vector<std::wstring> received;
    std::wstring received_cwd;
    BOSON_CHECK(primary.listen([&](std::vector<std::wstring> args, std::wstring cwd) {
        received = std::move(args);
        received_cwd = std::move(cwd);
    }));

    boson::app::SingleInstance second(app_id);
    BOSON_CHECK(!second.is_primary());
    const std::vector<std::wstring> forwarded{L"boson://open?url=https%3A%2F%2Fexample.com", L"--new-tab", L"spaced arg"};
    BOSON_CHECK(second.forward_to_primary(forwarded));
    BOSON_CHECK(received == forwarded);
    BOSON_CHECK(!received_cwd.empty());
    BOSON_CHECK(!primary.forward_to_primary(forwarded)); // the primary never forwards to itself
}

void test_webview_not_ready() {
    boson::core::WebView webview;
    BOSON_CHECK(!webview.is_ready());
    BOSON_CHECK(webview.id() != 0);
    BOSON_CHECK(!webview.go_back());
    BOSON_CHECK(!webview.go_forward());
    BOSON_CHECK(!webview.can_go_back());
    BOSON_CHECK(!webview.reload());
    BOSON_CHECK(!webview.stop());
    BOSON_CHECK(!webview.navigate("https://example.com"));
    BOSON_CHECK(!webview.source().has_value());
    BOSON_CHECK(!webview.document_title().has_value());
    BOSON_CHECK(!webview.set_zoom_factor(1.5));
    BOSON_CHECK(!webview.zoom_factor().has_value());
    BOSON_CHECK(!webview.set_muted(true));
    BOSON_CHECK(!webview.is_muted().has_value());
    BOSON_CHECK(!webview.open_devtools());
    BOSON_CHECK(!webview.open_task_manager());
    BOSON_CHECK(!webview.resume());
    BOSON_CHECK(!webview.is_suspended().has_value());
    BOSON_CHECK(!webview.set_memory_usage_target(boson::core::MemoryUsageTarget::Low));
    BOSON_CHECK(!webview.execute_script("1", [](HRESULT, std::string) {}));
    BOSON_CHECK(!webview.remove_script_on_document_created("x"));
    BOSON_CHECK(!webview.post_web_message_as_json("{}"));
    BOSON_CHECK(!webview.focus());
    BOSON_CHECK(!webview.non_client_region_at(POINT{1, 1}).has_value());

    // Senders on a WebView that is not ready complete inline with set_error(HResultError).
    bool threw = false;
    try {
        (void)stdexec::sync_wait(webview.execute_script("1 + 1"));
    } catch (const boson::exec::HResultError& e) {
        threw = e.code() == E_NOT_VALID_STATE;
    }
    BOSON_CHECK(threw);
    threw = false;
    try {
        (void)stdexec::sync_wait(webview.try_suspend());
    } catch (const boson::exec::HResultError&) {
        threw = true;
    }
    BOSON_CHECK(threw);

    // Bounds/visibility are tracked before the controller exists.
    webview.set_bounds(RECT{10, 20, 110, 220});
    BOSON_CHECK(webview.bounds().left == 10 && webview.bounds().bottom == 220);
    webview.set_visible(false);
    BOSON_CHECK(!webview.is_visible());
    webview.close();
    BOSON_CHECK(webview.is_closed());
}

void pump_messages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void test_window_events() {
    using boson::core::WindowEventKind;
    boson::core::WindowConfig cfg;
    cfg.title = L"Boson test window";
    cfg.width = 640;
    cfg.height = 480;
    cfg.min_width = 400;
    cfg.min_height = 300;
    cfg.quit_on_destroy = false;
    boson::core::Window window(cfg);
    BOSON_CHECK(window.create(nullptr, SW_HIDE));
    BOSON_CHECK(window.hwnd() != nullptr);

    // CloseRequested is vetoable.
    int close_requests = 0;
    const auto veto = window.on(WindowEventKind::CloseRequested, [&](boson::core::WindowEvent& e) {
        ++close_requests;
        e.cancel = true;
    });
    SendMessageW(window.hwnd(), WM_CLOSE, 0, 0);
    BOSON_CHECK(close_requests == 1);
    BOSON_CHECK(IsWindow(window.hwnd()) != FALSE);

    // Minimum size is enforced through WM_GETMINMAXINFO, in DIPs.
    MINMAXINFO info{};
    SendMessageW(window.hwnd(), WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&info));
    BOSON_CHECK(info.ptMinTrackSize.x >= window.dip_to_px(400));
    BOSON_CHECK(info.ptMinTrackSize.y >= window.dip_to_px(300));

    // Move / resize / minimize / restore / maximize events.
    std::vector<WindowEventKind> seen;
    for (const auto kind : {WindowEventKind::Move, WindowEventKind::Resize, WindowEventKind::Minimize,
                            WindowEventKind::Restore, WindowEventKind::Maximize, WindowEventKind::DpiChanged}) {
        window.on(kind, [&seen](boson::core::WindowEvent& e) { seen.push_back(e.kind); });
    }
    const RECT bounds{100, 100, 100 + window.dip_to_px(500), 100 + window.dip_to_px(400)};
    window.set_bounds(bounds);
    BOSON_CHECK(window.bounds().left == 100 && window.bounds().top == 100);
    BOSON_CHECK(std::ranges::find(seen, WindowEventKind::Move) != seen.end());
    BOSON_CHECK(std::ranges::find(seen, WindowEventKind::Resize) != seen.end());

    window.show(SW_SHOWMINNOACTIVE);
    pump_messages();
    BOSON_CHECK(window.is_minimized());
    BOSON_CHECK(std::ranges::find(seen, WindowEventKind::Minimize) != seen.end());
    window.show(SW_SHOWNOACTIVATE);
    pump_messages();
    BOSON_CHECK(std::ranges::find(seen, WindowEventKind::Restore) != seen.end());

    // WM_DPICHANGED moves the window to the suggested rect and reports the new DPI.
    RECT suggested{50, 60, 50 + 700, 60 + 500};
    UINT reported_dpi = 0;
    window.on(WindowEventKind::DpiChanged, [&](boson::core::WindowEvent& e) { reported_dpi = e.dpi; });
    SendMessageW(window.hwnd(), WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested));
    BOSON_CHECK(reported_dpi == 144);
    BOSON_CHECK(window.bounds().left == 50 && window.bounds().right == 750);

    // Snap Layouts maximize-button region answers HTMAXBUTTON.
    window.set_maximize_button_rect(RECT{10, 0, 56, 32});
    POINT probe{20, 10};
    ClientToScreen(window.hwnd(), &probe);
    const LRESULT hit = SendMessageW(window.hwnd(), WM_NCHITTEST, 0, MAKELPARAM(probe.x, probe.y));
    BOSON_CHECK(hit == HTMAXBUTTON);
    window.set_maximize_button_rect(std::nullopt);
    BOSON_CHECK(SendMessageW(window.hwnd(), WM_NCHITTEST, 0, MAKELPARAM(probe.x, probe.y)) != HTMAXBUTTON);

    // Removing a listener lets WM_CLOSE through; quit_on_destroy = false posts no WM_QUIT.
    bool destroyed = false;
    window.on(WindowEventKind::Destroyed, [&](boson::core::WindowEvent&) { destroyed = true; });
    BOSON_CHECK(window.off(veto));
    SendMessageW(window.hwnd(), WM_CLOSE, 0, 0);
    BOSON_CHECK(destroyed);
    BOSON_CHECK(window.hwnd() == nullptr);
    MSG msg{};
    BOSON_CHECK(PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) == FALSE);
}

void test_ui_timer_and_reply_routing() {
    boson::core::UiTimer timer;
    int fired = 0;
    BOSON_CHECK(timer.start(std::chrono::milliseconds{10}, [&]() { ++fired; }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (fired == 0 && std::chrono::steady_clock::now() < deadline) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        pump_messages();
    }
    BOSON_CHECK(fired == 1);
    BOSON_CHECK(!timer.is_running());

    std::mutex mu;
    std::condition_variable cv;
    bool woken = false;
    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&]() {
        std::scoped_lock lock(mu);
        woken = true;
        cv.notify_all();
    });
    boson::exec::TaskRunner runner(scheduler, marshal);
    boson::demo::AppController app;
    boson::ipc::Router<boson::demo::AppController> router(app);

    std::string reply;
    router.dispatch_async(R"({"id":7,"method":"App.ping","params":null})", runner, [&](std::string r) { reply = std::move(r); });
    {
        std::unique_lock lock(mu);
        BOSON_CHECK(cv.wait_for(lock, std::chrono::seconds{5}, [&]() { return woken; }));
    }
    marshal.drain_on_ui_thread();
    BOSON_CHECK(reply.find(R"("id":7)") != std::string::npos);
    BOSON_CHECK(reply.find(R"("status":"success")") != std::string::npos);
}

}  // namespace

void test_scheduler_sizing() {
    const std::uint32_t def = boson::exec::Scheduler::default_thread_count();
    BOSON_CHECK(def >= 2);
    BOSON_CHECK(def <= boson::exec::Scheduler::kMaxDefaultThreads);
    BOSON_CHECK(def <= std::max(2u, std::thread::hardware_concurrency() / 2));

    boson::exec::Scheduler default_pool;
    BOSON_CHECK(default_pool.thread_count() == def);
    boson::exec::Scheduler explicit_pool(3);
    BOSON_CHECK(explicit_pool.thread_count() == 3);
    boson::exec::Scheduler clamped_pool(0);
    BOSON_CHECK(clamped_pool.thread_count() == 1);

    // The right-sized pool still runs work.
    std::atomic<int> ran{0};
    boson::exec::UiMarshal marshal([]() {});
    boson::exec::TaskRunner runner(default_pool, marshal);
    for (int i = 0; i < 64; ++i) {
        runner.spawn([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (ran.load(std::memory_order_relaxed) < 64 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    BOSON_CHECK(ran.load() == 64);
}

// ---- PE import tables of the running executable -----------------------------------------------------------

extern "C" IMAGE_DOS_HEADER __ImageBase;

[[nodiscard]] std::vector<std::string> pe_import_names(bool delay_load) {
    std::vector<std::string> names;
    const auto* base = reinterpret_cast<const std::byte*>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + __ImageBase.e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[delay_load ? IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT : IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) {
        return names;
    }
    const auto lower = [](const char* s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    if (delay_load) {
        const auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dir.VirtualAddress);
        for (; d->DllNameRVA != 0; ++d) {
            names.push_back(lower(reinterpret_cast<const char*>(base + d->DllNameRVA)));
        }
    } else {
        const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
        for (; d->Name != 0; ++d) {
            names.push_back(lower(reinterpret_cast<const char*>(base + d->Name)));
        }
    }
    return names;
}

void test_delay_load_policy() {
#if defined(BOSON_DELAYLOAD_ENABLED)
    const std::vector<std::string> imported = pe_import_names(false);
    const std::vector<std::string> delayed = pe_import_names(true);
    BOSON_CHECK(!imported.empty());
    const auto has = [](const std::vector<std::string>& v, std::string_view name) { return std::ranges::find(v, name) != v.end(); };
    // The rarely used system DLLs must never be ordinary imports of an executable that links boson_core ...
    for (const std::string_view dll : {"uiautomationcore.dll", "dcomp.dll", "winhttp.dll", "crypt32.dll", "wtsapi32.dll",
                                       "ws2_32.dll", "windowscodecs.dll", "bcrypt.dll", "wintrust.dll", "propsys.dll"}) {
        BOSON_CHECK(!has(imported, dll));
    }
    // ... dbghelp stays eager on purpose (the crash reporter must not load a DLL inside an exception filter) ...
    BOSON_CHECK(!has(delayed, "dbghelp.dll"));
    // ... and the delay-load table exists, so the check above is not vacuous.
    BOSON_CHECK(!delayed.empty());
#else
    std::cout << "[skip] test_delay_load_policy: built with BOSON_DELAYLOAD=OFF\n";
#endif
}

int main() {
    test_scheduler_sizing();
    test_delay_load_policy();
    test_ipc_router();
    test_async_exec_and_ui_marshal();
    test_inplace_vector();
    test_assets_and_protocol();
    test_ts_generator();
    test_ui_marshal_tasks_and_backdrops();
    test_browser_arguments();
    test_bridge_origins_and_events();
    test_ts_generator_events();
    test_command_line_quoting();
    test_single_instance();
    test_webview_not_ready();
    test_window_events();
    test_ui_timer_and_reply_routing();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }

    std::cout << "All Boson tests passed.\n";
    return 0;
}
