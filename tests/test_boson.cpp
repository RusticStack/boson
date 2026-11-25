#include <atomic>
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

#include <glaze/glaze.hpp>

#include <boson/assets/asset_registry.hpp>
#include <boson/core/inplace_vector.hpp>
#include <boson/core/protocol.hpp>
#include <boson/exec/scheduler.hpp>
#include <boson/exec/task_runner.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/protocol_types.hpp>
#include <boson/ipc/router.hpp>
#include <boson/ipc/ts_generator.hpp>

#include "../examples/demo/app_controller.hpp"

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
    const std::string dts = boson::ipc::TsGenerator::generate<boson::demo::AppController>();

    BOSON_CHECK(dts.find("export interface PingResult") != std::string::npos);
    BOSON_CHECK(dts.find("export interface SystemInfo") != std::string::npos);
    BOSON_CHECK(dts.find("export interface ComputePrimesRequest") != std::string::npos);
    BOSON_CHECK(dts.find("export interface ComputePrimesResult") != std::string::npos);
    BOSON_CHECK(dts.find("export interface BosonApi") != std::string::npos);
    BOSON_CHECK(dts.find("\"App.ping\"") != std::string::npos);
    BOSON_CHECK(dts.find("\"App.get_system_info\"") != std::string::npos);
    BOSON_CHECK(dts.find("\"App.compute_primes\"") != std::string::npos);
    BOSON_CHECK(dts.find("\"App.trigger_error\"") != std::string::npos);
    BOSON_CHECK(dts.find("Promise<") != std::string::npos);
}

}  // namespace

int main() {
    test_ipc_router();
    test_async_exec_and_ui_marshal();
    test_inplace_vector();
    test_assets_and_protocol();
    test_ts_generator();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }

    std::cout << "All Boson tests passed.\n";
    return 0;
}
