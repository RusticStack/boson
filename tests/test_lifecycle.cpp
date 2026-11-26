// Lifecycle / start-up / shutdown: exit hooks, the lazily started Scheduler pool, app::quit() and
// Environment::shutdown(0) without waiting for the browser process, WebView::discard() and SleepPolicy::discard.
// The WebView2 part skips when no runtime is installed. Windows are shown with SW_SHOWNOACTIVATE.
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boson/boson.hpp>
#include <boson/shipping/webview2_runtime.hpp>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_lifecycle.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define LC_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                continue; // a quit() under test must not end the pump
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return true;
}

void pump_for(std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    pump_until([until]() { return std::chrono::steady_clock::now() >= until; }, duration + std::chrono::seconds{1});
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

std::size_t process_thread_count() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::size_t count = 0;
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok != FALSE; ok = Thread32Next(snapshot, &entry)) {
        count += entry.th32OwnerProcessID == pid ? 1 : 0;
    }
    CloseHandle(snapshot);
    return count;
}

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    static constexpr std::string_view kPage =
        "<!doctype html><html><head><title>Lifecycle</title></head><body>page</body></html>";
    if (path == "/index.html") {
        return std::as_bytes(std::span{kPage.data(), kPage.size()});
    }
    return std::nullopt;
}

void test_exit_hooks() {
    step("exit hooks run once, in reverse order, and survive a throwing hook");
    std::vector<int> order;
    boson::app::add_exit_hook([&]() { order.push_back(1); });
    boson::app::add_exit_hook([&]() { throw std::runtime_error("hook failure"); });
    boson::app::add_exit_hook([&]() { order.push_back(3); });
    boson::app::add_exit_hook({}); // ignored
    boson::app::run_exit_hooks();
    LC_CHECK((order == std::vector<int>{3, 1}));
    boson::app::run_exit_hooks();
    LC_CHECK((order == std::vector<int>{3, 1}));
}

void test_trace_disabled() {
    step("BOSON_TRACE unset: markers are disabled");
    LC_CHECK(!boson::trace::enabled());
    boson::trace::mark("test.marker"); // no table, no output
}

void test_lazy_scheduler() {
    step("Scheduler starts its pool on first use");
    const std::size_t before = process_thread_count();
    std::size_t idle_after_construct = 0;
    std::size_t running = 0;
    int value = 0;
    {
        boson::exec::Scheduler scheduler(6);
        idle_after_construct = process_thread_count();
        auto result = stdexec::sync_wait(stdexec::schedule(scheduler.get_scheduler()) | stdexec::then([]() { return 42; }));
        value = result ? std::get<0>(*result) : 0;
        running = process_thread_count();
    }
    LC_CHECK(idle_after_construct == before);
    LC_CHECK(running >= before + 6);
    LC_CHECK(value == 42);
    // unused scheduler: no threads at all, and destruction is trivial
    {
        boson::exec::Scheduler unused(8);
        LC_CHECK(process_thread_count() == before);
        LC_CHECK(unused.thread_count() == 8);
    }
}

struct Fixture {
    explicit Fixture(const fs::path& user_data) {
        boson::core::WindowConfig window_cfg;
        window_cfg.title = L"Boson lifecycle test";
        window_cfg.width = 480;
        window_cfg.height = 320;
        window_cfg.quit_on_destroy = false;
        window = std::make_unique<boson::core::Window>(window_cfg);
        created = window->create(nullptr, SW_SHOWNOACTIVATE);
        boson::core::EnvironmentConfig env_cfg;
        env_cfg.user_data_folder = user_data.wstring();
        env = boson::core::Environment::create(env_cfg);
        host = std::make_unique<boson::core::WebViewHost>(*window, env);
        host->set_default_asset_lookup(&lookup);
    }
    ~Fixture() {
        host.reset();
        window.reset();
    }
    std::unique_ptr<boson::core::Window> window;
    std::shared_ptr<boson::core::Environment> env;
    std::unique_ptr<boson::core::WebViewHost> host;
    bool created = false;
};

boson::core::WebView& open_page(Fixture& f, bool& nav_done) {
    boson::core::WebViewConfig cfg;
    cfg.initial_url = "boson://app/index.html";
    boson::core::WebView& wv = f.host->create(cfg);
    wv.events().on_navigation_completed = [&nav_done](const boson::core::NavigationCompletedEvent& e) { nav_done = e.is_success; };
    return wv;
}

void test_discard_and_sleep_policy(const fs::path& base) {
    step("WebView::discard(), wake on set_visible(true), SleepPolicy::discard");
    Fixture f(base / L"udf-discard");
    LC_CHECK(f.created);
    bool nav_done = false;
    boson::core::WebView& wv = open_page(f, nav_done);
    LC_CHECK(pump_until([&]() { return wv.is_ready() && nav_done; }));
    LC_CHECK(eval(wv, "document.title") == std::optional<std::string>("\"Lifecycle\""));
    LC_CHECK(eval(wv, "window.__state = 7; 1").has_value());

    wv.set_visible(false);
    LC_CHECK(wv.discard());
    LC_CHECK(wv.is_discarded());
    LC_CHECK(!wv.is_ready());
    LC_CHECK(!wv.discard()); // already discarded
    pump_for(std::chrono::milliseconds{300});
    LC_CHECK(!wv.is_ready()); // stays discarded while hidden (nothing recreates it behind our back)

    nav_done = false;
    wv.set_visible(true);
    LC_CHECK(!wv.is_discarded());
    LC_CHECK(pump_until([&]() { return wv.is_ready() && nav_done; }));
    LC_CHECK(eval(wv, "document.title") == std::optional<std::string>("\"Lifecycle\""));
    LC_CHECK(eval(wv, "typeof window.__state") == std::optional<std::string>("\"undefined\"")); // page state is gone

    // The policy path: hidden for 0 ms, discard instead of TrySuspend.
    int slept = 0;
    f.host->set_on_slept([&slept](boson::core::WebView&) { ++slept; });
    f.host->set_sleep_policy(boson::core::SleepPolicy{
        .hidden_timeout = std::chrono::milliseconds{0}, .check_interval = std::chrono::hours{1}, .discard = true});
    wv.set_visible(false);
    pump_for(std::chrono::milliseconds{20});
    f.host->check_sleep_now();
    LC_CHECK(slept == 1);
    LC_CHECK(wv.is_discarded());
    LC_CHECK(f.host->is_sleeping(wv));
    f.host->check_sleep_now(); // a sleeping WebView is not put to sleep again
    LC_CHECK(slept == 1);
    nav_done = false;
    wv.set_visible(true);
    LC_CHECK(pump_until([&]() { return wv.is_ready() && nav_done; }));
    LC_CHECK(!f.host->is_sleeping(wv));
    LC_CHECK(eval(wv, "document.title") == std::optional<std::string>("\"Lifecycle\""));

    // Shutting the environment down with a discarded WebView registered is fine.
    wv.set_visible(false);
    LC_CHECK(wv.discard());

    bool shut = false;
    f.env->shutdown([&]() { shut = true; });
    pump_until([&]() { return shut; }, std::chrono::seconds{15});
    LC_CHECK(shut);
}

void test_quit_without_waiting(const fs::path& base) {
    step("app::quit() and Environment::shutdown(0) do not wait for the browser process");
    Fixture f(base / L"udf-quit");
    LC_CHECK(f.created);
    bool nav_done = false;
    boson::core::WebView& wv = open_page(f, nav_done);
    LC_CHECK(pump_until([&]() { return wv.is_ready() && nav_done; }));

    // Drain a stale WM_QUIT, if any, then call quit(): the message is posted before quit() returns.
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
    }
    const auto start = std::chrono::steady_clock::now();
    boson::app::quit({f.env}, std::chrono::milliseconds{0}, 7);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const bool posted = PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE;
    LC_CHECK(posted);
    LC_CHECK(posted && msg.wParam == 7);
    LC_CHECK(!wv.is_ready()); // controller closed
    std::cout << "  quit(): " << elapsed_ms << " ms to WM_QUIT (was: up to the 3 s browser-exit timeout)\n";
    LC_CHECK(elapsed_ms < 1500.0);

    // The old behaviour is still available: a positive timeout waits for BrowserProcessExited.
    Fixture g(base / L"udf-quit-wait");
    bool nav2 = false;
    boson::core::WebView& wv2 = open_page(g, nav2);
    LC_CHECK(pump_until([&]() { return wv2.is_ready() && nav2; }));
    bool done = false;
    g.env->shutdown([&]() { done = true; }, std::chrono::seconds{10});
    LC_CHECK(!done); // the browser process had to be waited for
    LC_CHECK(pump_until([&]() { return done; }, std::chrono::seconds{15}));
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    const fs::path base = fs::temp_directory_path() /
        (L"boson-lifecycle-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(base);

    test_exit_hooks();
    test_trace_disabled();
    test_lazy_scheduler();

    if (boson::shipping::installed_webview2_version().has_value()) {
        test_discard_and_sleep_policy(base);
        test_quit_without_waiting(base);
    } else {
        std::cout << "[skip] no WebView2 runtime installed\n";
    }

    std::error_code ec;
    fs::remove_all(base, ec);
    CoUninitialize();
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
