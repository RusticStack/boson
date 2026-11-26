// boson_ipc_bench: native IPC hot-path micro-benchmark (not a test; run a Release build by hand).
//
// Measures allocations (global operator new override, all threads), bytes, wake-ups and time per call for
//   - Router::dispatch_sync (parse + route + invoke + serialize), small and large payloads
//   - Bridge::on_web_message front half (origin check + capability header peek), via a no-op dispatch callback
//   - Router::dispatch_async -> TaskRunner (stdexec pool) -> UiMarshal -> UI-thread drain (this thread plays the UI)
//   - Bridge::emit event path (envelope + marshal queue + drain)
//   - UTF-8 <-> UTF-16 conversion helpers
// Output is one JSON object on stdout so runs can be diffed.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>
#if __has_include(<boson/core/text_convert.hpp>)
#include <boson/core/text_convert.hpp>
#define BOSON_HAS_TEXT_CONVERT 1
#endif

namespace {
std::atomic<std::uint64_t> g_allocs{0};
std::atomic<std::uint64_t> g_bytes{0};
std::atomic<bool> g_counting{false};
} // namespace

void* operator new(std::size_t size) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocs.fetch_add(1, std::memory_order_relaxed);
        g_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace bench {

struct PingRequest {
    std::uint64_t seq = 0;
};
struct PingResponse {
    std::uint64_t seq = 0;
};
struct DataRequest {
    std::string data;
};
struct DataResponse {
    std::string data;
};

class Ctl {
public:
    BOSON_CONTROLLER("Bench");
    [[nodiscard]] PingResponse ping(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] DataResponse echo(DataRequest req) const { return DataResponse{.data = std::move(req.data)}; }
    void nothing() const noexcept {}
};

/// 32 methods: measures method lookup in a wide controller.
class Wide {
public:
    BOSON_CONTROLLER("Wide");
    [[nodiscard]] PingResponse m00(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m01(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m02(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m03(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m04(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m05(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m06(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m07(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m08(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m09(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m10(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m11(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m12(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m13(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m14(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m15(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m16(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m17(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m18(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m19(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m20(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m21(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m22(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m23(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m24(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m25(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m26(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m27(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m28(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m29(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m30(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
    [[nodiscard]] PingResponse m31(PingRequest req) const noexcept { return PingResponse{.seq = req.seq}; }
};

} // namespace bench

template <>
struct glz::meta<bench::Wide> {
    using T = bench::Wide;
    static constexpr auto value = glz::object(
        "m00", &T::m00,
        "m01", &T::m01,
        "m02", &T::m02,
        "m03", &T::m03,
        "m04", &T::m04,
        "m05", &T::m05,
        "m06", &T::m06,
        "m07", &T::m07,
        "m08", &T::m08,
        "m09", &T::m09,
        "m10", &T::m10,
        "m11", &T::m11,
        "m12", &T::m12,
        "m13", &T::m13,
        "m14", &T::m14,
        "m15", &T::m15,
        "m16", &T::m16,
        "m17", &T::m17,
        "m18", &T::m18,
        "m19", &T::m19,
        "m20", &T::m20,
        "m21", &T::m21,
        "m22", &T::m22,
        "m23", &T::m23,
        "m24", &T::m24,
        "m25", &T::m25,
        "m26", &T::m26,
        "m27", &T::m27,
        "m28", &T::m28,
        "m29", &T::m29,
        "m30", &T::m30,
        "m31", &T::m31);
};

template <>
struct glz::meta<bench::Ctl> {
    using T = bench::Ctl;
    static constexpr auto value = glz::object("ping", &T::ping, "echo", &T::echo, "nothing", &T::nothing);
};

namespace {

using Clock = std::chrono::steady_clock;

struct Sample {
    double ns_per_op = 0;
    double allocs_per_op = 0;
    double bytes_per_op = 0;
    double wakes_per_op = 0;
};

struct Counters {
    std::uint64_t allocs = 0;
    std::uint64_t bytes = 0;
};

void begin_counting() {
    g_allocs.store(0);
    g_bytes.store(0);
    g_counting.store(true);
}
Counters end_counting() {
    g_counting.store(false);
    return {g_allocs.load(), g_bytes.load()};
}

double median(std::vector<double> v) {
    std::ranges::sort(v);
    return v[v.size() / 2];
}

// Runs fn(n) repeatedly (5 timed repetitions, one allocation-counted repetition) and returns medians.
std::atomic<std::uint64_t>* g_wake_counter = nullptr;

template <typename Fn>
Sample measure(std::size_t n, Fn&& fn) {
    fn(n / 10 + 1); // warm-up
    std::vector<double> ns;
    for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = Clock::now();
        fn(n);
        const auto t1 = Clock::now();
        ns.push_back(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
                     static_cast<double>(n));
    }
    const std::uint64_t w0 = g_wake_counter != nullptr ? g_wake_counter->load() : 0;
    begin_counting();
    fn(n);
    const Counters c = end_counting();
    const std::uint64_t w1 = g_wake_counter != nullptr ? g_wake_counter->load() : 0;
    return Sample{.ns_per_op = median(ns),
                  .allocs_per_op = static_cast<double>(c.allocs) / static_cast<double>(n),
                  .bytes_per_op = static_cast<double>(c.bytes) / static_cast<double>(n),
                  .wakes_per_op = static_cast<double>(w1 - w0) / static_cast<double>(n)};
}

std::string g_json;
bool g_first = true;

void report(std::string_view name, const Sample& s) {
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "%s\"%.*s\":{\"ns_per_op\":%.1f,\"allocs_per_op\":%.2f,\"bytes_per_op\":%.1f,\"wakes_per_op\":%.3f}",
                  g_first ? "" : ",", static_cast<int>(name.size()), name.data(), s.ns_per_op, s.allocs_per_op,
                  s.bytes_per_op, s.wakes_per_op);
    g_first = false;
    g_json += buf;
    std::fprintf(stderr, "%-34s %10.1f ns/op %8.2f allocs/op %12.1f B/op %6.3f wakes/op\n", std::string{name}.c_str(),
                 s.ns_per_op, s.allocs_per_op, s.bytes_per_op, s.wakes_per_op);
}

std::string make_ping(std::uint64_t id) {
    return std::string{"{\"id\":"} + std::to_string(id) + ",\"method\":\"Bench.ping\",\"params\":{\"seq\":" + std::to_string(id) + "}}";
}
std::string make_echo(std::uint64_t id, std::size_t bytes) {
    return std::string{"{\"id\":"} + std::to_string(id) + ",\"method\":\"Bench.echo\",\"params\":{\"data\":\"" + std::string(bytes, 'x') + "\"}}";
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t scale = argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 1;
    bench::Ctl ctl;
    boson::ipc::Router<bench::Ctl> router(ctl);
    const std::string ping = make_ping(12345);
    const std::string echo_1k = make_echo(12345, 1024);
    const std::string echo_1m = make_echo(12345, 1024 * 1024);
    const std::string nothing = R"({"id":9,"method":"Bench.nothing","params":{}})";
    const std::string unknown = R"({"id":9,"method":"Bench.nope","params":{}})";

    // --- Router::dispatch_sync ------------------------------------------------------------------------------
    std::size_t sink = 0;
    report("sync_ping", measure(200000 * scale, [&](std::size_t n) {
               for (std::size_t i = 0; i < n; ++i) sink += router.dispatch_sync(ping).size();
           }));
    report("sync_nothing", measure(200000 * scale, [&](std::size_t n) {
               for (std::size_t i = 0; i < n; ++i) sink += router.dispatch_sync(nothing).size();
           }));
    report("sync_unknown", measure(100000 * scale, [&](std::size_t n) {
               for (std::size_t i = 0; i < n; ++i) sink += router.dispatch_sync(unknown).size();
           }));
    report("sync_echo_1KiB", measure(50000 * scale, [&](std::size_t n) {
               for (std::size_t i = 0; i < n; ++i) sink += router.dispatch_sync(echo_1k).size();
           }));
    report("sync_echo_1MiB", measure(60 * scale, [&](std::size_t n) {
               for (std::size_t i = 0; i < n; ++i) sink += router.dispatch_sync(echo_1m).size();
           }));

    bench::Wide wide_ctl;
    boson::ipc::Router<bench::Ctl, bench::Wide> wide_router(ctl, wide_ctl);
    const std::string wide_last = R"({"id":12345,"method":"Wide.m31","params":{"seq":12345}})";
    report("sync_ping_wide_m31", measure(200000 * scale, [&](std::size_t n) {
               for (std::size_t i = 0; i < n; ++i) sink += wide_router.dispatch_sync(wide_last).size();
           }));

    // --- UTF-8 <-> UTF-16 --------------------------------------------------------------------------------------
    {
        const std::wstring wide_1k = boson::core::utf8_to_wide(echo_1k);
        const std::wstring wide_1m = boson::core::utf8_to_wide(echo_1m);
        report("utf8_to_wide_64B", measure(500000 * scale, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i) sink += boson::core::utf8_to_wide(std::string_view{ping}).size();
               }));
        report("utf8_to_wide_1MiB", measure(60 * scale, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i) sink += boson::core::utf8_to_wide(echo_1m).size();
               }));
#ifdef BOSON_HAS_TEXT_CONVERT
        {
            std::wstring reuse_w;
            std::string reuse_u;
            report("utf8_to_wide_into_1MiB_reuse", measure(200 * scale, [&](std::size_t n) {
                       for (std::size_t i = 0; i < n; ++i) {
                           boson::core::utf8_to_wide_into(echo_1m, reuse_w);
                           sink += reuse_w.size();
                       }
                   }));
            report("wide_to_utf8_into_1MiB_reuse", measure(200 * scale, [&](std::size_t n) {
                       for (std::size_t i = 0; i < n; ++i) {
                           boson::core::wide_to_utf8_into(wide_1m, reuse_u);
                           sink += reuse_u.size();
                       }
                   }));
        }
#endif
        report("wide_to_utf8_1KiB", measure(100000 * scale, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i) sink += boson::core::wide_to_utf8(wide_1k).size();
               }));
        report("wide_to_utf8_1MiB", measure(60 * scale, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i) sink += boson::core::wide_to_utf8(wide_1m).size();
               }));
    }

    // --- Async path: Bridge front half, dispatch_async + pool + marshal ------------------------------------------
    boson::exec::Scheduler scheduler(4);
    HANDLE wake_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<std::uint64_t> wakes{0};
    boson::exec::UiMarshal marshal([&]() {
        wakes.fetch_add(1, std::memory_order_relaxed);
        ::SetEvent(wake_event);
    });
    boson::exec::TaskRunner runner(scheduler, marshal);

    // Pumps the "UI thread" (this thread) until done reaches target.
    std::atomic<std::uint64_t> done{0};
    const auto pump_until = [&](std::uint64_t target) {
        while (done.load(std::memory_order_acquire) < target) {
            ::WaitForSingleObject(wake_event, 2);
            marshal.drain_on_ui_thread();
        }
    };

    boson::core::WebView webview; // never initialised: post_web_message_as_json returns false at once
    {
        // Bridge::on_web_message front half: origin check + capability header peek; dispatch is a no-op.
        boson::ipc::Bridge front(runner, marshal, [](std::string_view, boson::ipc::Bridge::ReplyFn) {});
        boson::security::CapabilityConfig cap_cfg;
        cap_cfg.default_deny = true;
        boson::security::Capability cap;
        cap.identifier = "all";
        cap.allow = {"Bench.*"};
        cap_cfg.capabilities.push_back(cap);
        auto policy = boson::security::CapabilityPolicy::compile(std::move(cap_cfg));
        boson::ipc::BridgeAttachOptions opts;
        if (policy) {
            opts.capabilities = std::make_shared<const boson::security::CapabilityPolicy>(std::move(*policy));
        }
        front.attach(webview, std::move(opts));
        auto cb = front.make_message_callback();
        report("bridge_front_capability", measure(200000 * scale, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i) cb(ping, "boson://app/index.html");
               }));
        front.attach(webview, {});
        cb = front.make_message_callback();
        report("bridge_front_plain", measure(200000 * scale, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i) cb(ping, "boson://app/index.html");
               }));
        front.detach(webview);
    }

    g_wake_counter = &wakes;
    // Full async path through Router::dispatch_async with a counting reply (stands in for Bridge::deliver).
    const auto async_roundtrip = [&](std::string_view json, std::size_t n, std::size_t window) {
        const std::uint64_t base = done.load();
        std::uint64_t sent = 0;
        while (done.load(std::memory_order_acquire) - base < n) {
            while (sent < n && sent - (done.load(std::memory_order_acquire) - base) < window) {
                router.dispatch_async(json, runner, [&done](std::string) { done.fetch_add(1, std::memory_order_release); });
                ++sent;
            }
            ::WaitForSingleObject(wake_event, 2);
            marshal.drain_on_ui_thread();
        }
    };
    report("async_ping_sequential", measure(20000 * scale, [&](std::size_t k) { async_roundtrip(ping, k, 1); }));
    report("async_ping_window1000", measure(20000 * scale, [&](std::size_t k) { async_roundtrip(ping, k, 1000); }));
    report("async_echo_1MiB_sequential", measure(40 * scale, [&](std::size_t k) { async_roundtrip(echo_1m, k, 1); }));

    // --- Events: Bridge::emit -> marshal -> drain (delivery to a never-initialised WebView is a no-op) ---------------
    {
        boson::ipc::Bridge events(runner, marshal, router);
        events.attach(webview);
        const std::size_t n = 100000 * scale;
        const Sample s = measure(n, [&](std::size_t k) {
            for (std::size_t i = 0; i < k; ++i) {
                events.emit_json(boson::ipc::Bridge::kAllWebViews, "bench:tick", R"({"n":1,"label":"tick"})");
            }
            while (marshal.pending_count() != 0) {
                marshal.drain_on_ui_thread();
            }
        });
        report("emit_json_burst", s);
        events.detach(webview);
    }

    std::printf("{%s,\"sink\":%zu}\n", g_json.c_str(), sink);
    ::CloseHandle(wake_event);
    return 0;
}
