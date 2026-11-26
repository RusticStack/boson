// Integration tests against the installed WebView2 runtime. Skips (exit 0) when no runtime is available.
#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

#include <exec/async_scope.hpp>

namespace it {

struct EchoRequest {
    std::string text;
};

struct EchoResult {
    std::string text;
    std::uint32_t thread_id = 0;
};

class EchoController {
public:
    BOSON_CONTROLLER("Test");

    [[nodiscard]] EchoResult echo(EchoRequest req) const {
        return EchoResult{.text = req.text + "!", .thread_id = static_cast<std::uint32_t>(GetCurrentThreadId())};
    }
};

} // namespace it

using it::EchoController;
using it::EchoResult;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_integration.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define IT_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

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

constexpr std::string_view kPageScript =
    "<script>window.__events=[];window.__responses=[];window.__moves=0;"
    "document.addEventListener('mousemove',()=>{window.__moves++;});"
    "if(window.chrome&&chrome.webview){chrome.webview.addEventListener('message',e=>{const d=e.data;"
    "if(d&&d.type==='event'){window.__events.push(d);}else{window.__responses.push(d);}});}</script>";

const std::string kIndex = std::string("<!doctype html><html><head><title>Page A</title>") + std::string(kPageScript) + "</head><body>A</body></html>";
const std::string kSecond = std::string("<!doctype html><html><head><title>Second</title>") + std::string(kPageScript) + "</head><body>B</body></html>";
const std::string kPopup = std::string("<!doctype html><html><head><title>Popup</title>") + std::string(kPageScript) + "</head><body>P</body></html>";
const std::string kDrag = std::string("<!doctype html><html><head><title>Drag</title><style>html,body{margin:0;height:100%}"
    "#bar{app-region:drag;-webkit-app-region:drag;position:fixed;left:0;top:0;width:100%;height:60px;background:#333}</style>") +
    std::string(kPageScript) + "</head><body><div id=bar></div></body></html>";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    const auto as_bytes = [](const std::string& s) {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
    };
    if (path == "/index.html") return as_bytes(kIndex);
    if (path == "/second.html") return as_bytes(kSecond);
    if (path == "/popup.html") return as_bytes(kPopup);
    if (path == "/drag.html") return as_bytes(kDrag);
    return std::nullopt;
}

std::size_t count_kind(const std::vector<boson::core::ProcessInfo>& infos, boson::core::ProcessKind kind) {
    std::size_t n = 0;
    for (const auto& info : infos) {
        n += info.kind == kind ? 1 : 0;
    }
    return n;
}

bool contains(const std::optional<std::string>& haystack, std::string_view needle) {
    return haystack.has_value() && haystack->find(needle) != std::string::npos;
}

} // namespace

template <>
struct glz::meta<it::EchoController> {
    using T = it::EchoController;
    static constexpr auto value = glz::object("echo", &T::echo);
};

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }

    {
        LPWSTR version = nullptr;
        if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
            std::cout << "SKIP: no WebView2 runtime installed\n";
            return 0;
        }
        std::wcout << L"WebView2 runtime " << version << L'\n';
        CoTaskMemFree(version);
    }

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path user_data = std::filesystem::path(temp) / (L"boson-it-" + std::to_wstring(GetCurrentProcessId()));

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson integration test";
    window_cfg.width = 900;
    window_cfg.height = 640;
    window_cfg.quit_on_destroy = false;
    window_cfg.always_on_top = true; // WindowFromPoint checks below need the window unobscured
    auto window = std::make_unique<boson::core::Window>(window_cfg);
    IT_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
    window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = user_data.wstring();
    env_cfg.additional_browser_arguments = {L"--autoplay-policy=no-user-gesture-required"};
    env_cfg.enable_features = {L"msWebView2EnableDraggableRegions"};
    const auto env = boson::core::Environment::create(env_cfg);

    auto host = std::make_unique<boson::core::WebViewHost>(*window, env);
    host->set_default_asset_lookup(&lookup);

    // ---- #2 / #3: two WebViews on one environment, explicit layout -------------------------------------
    step("create two WebViews sharing one environment");
    bool a_nav = false;
    bool b_nav = false;
    std::string a_title;
    bool a_history = false;
    std::string a_source;

    boson::core::WebViewConfig a_cfg;
    a_cfg.initial_url = "boson://app/index.html";
    boson::core::WebView& a = host->create(a_cfg);
    a.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { a_nav = e.is_success; };
    a.events().on_title_changed = [&](std::string_view t) { a_title = t; };
    a.events().on_history_changed = [&](bool, bool) { a_history = true; };
    a.events().on_source_changed = [&](std::string_view uri, bool) { a_source = uri; };

    boson::core::WebViewConfig b_cfg;
    b_cfg.initial_url = "boson://app/second.html";
    boson::core::WebView& b = host->create(b_cfg);
    b.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { b_nav = e.is_success; };

    host->set_layout([&a, &b](boson::core::WebViewHost&, const boson::core::LayoutContext& ctx) {
        const int header = ctx.px(40);
        a.set_bounds(RECT{0, 0, ctx.width(), header});
        b.set_bounds(RECT{0, header, ctx.width(), ctx.height()});
    });

    IT_CHECK(pump_until([&]() { return a.is_ready() && b.is_ready() && a_nav && b_nav; }));
    IT_CHECK(env->is_ready());
    IT_CHECK(env->webview_count() == 2);

    {
        const auto infos = env->process_infos();
        IT_CHECK(count_kind(infos, boson::core::ProcessKind::Browser) == 1);
        IT_CHECK(count_kind(infos, boson::core::ProcessKind::Renderer) >= 1);
        UINT32 pid_a = 0;
        UINT32 pid_b = 0;
        a.core_webview()->get_BrowserProcessId(&pid_a);
        b.core_webview()->get_BrowserProcessId(&pid_b);
        IT_CHECK(pid_a != 0 && pid_a == pid_b);
        IT_CHECK(env->browser_process_id() == std::optional<std::uint32_t>{pid_a});
        std::cout << "  browser pid " << pid_a << ", " << infos.size() << " processes\n";
    }
    {
        const RECT client = window->client_rect();
        IT_CHECK(a.bounds().bottom == window->dip_to_px(40));
        IT_CHECK(b.bounds().top == window->dip_to_px(40) && b.bounds().bottom == client.bottom);
        b.set_bounds_dip(RECT{0, 100, 400, 300});
        IT_CHECK(b.bounds().right == window->dip_to_px(400) && b.bounds().top == window->dip_to_px(100));
        host->relayout();
        IT_CHECK(b.bounds().top == window->dip_to_px(40));
    }

    // ---- #6 / #5: events, scripts, navigation ----------------------------------------------------------
    step("events, script execution, navigation");
    IT_CHECK(pump_until([&]() { return a_title == "Page A"; }, std::chrono::seconds{5}));
    IT_CHECK(a.document_title() == std::optional<std::string>{"Page A"});
    IT_CHECK(contains(a.source(), "boson://app/index.html"));
    IT_CHECK(eval(a, "1 + 1") == std::optional<std::string>{"2"});

    std::optional<std::string> sender_result;
    bool sender_failed = false;
    ::exec::async_scope scope;
    scope.spawn(
        a.execute_script("3 * 3") |
        stdexec::then([&](std::string r) noexcept { sender_result = std::move(r); }) |
        stdexec::upon_error([&](std::exception_ptr) noexcept { sender_failed = true; }));
    IT_CHECK(pump_until([&]() { return sender_result.has_value() || sender_failed; }));
    stdexec::sync_wait(scope.on_empty());
    IT_CHECK(sender_result == std::optional<std::string>{"9"});

    std::optional<std::string> script_id;
    IT_CHECK(a.add_script_on_document_created("window.__injected = 7;", [&](HRESULT hr, std::string id) {
        if (SUCCEEDED(hr)) script_id = std::move(id);
    }));
    IT_CHECK(pump_until([&]() { return script_id.has_value(); }));

    a_nav = false;
    IT_CHECK(a.navigate("boson://app/second.html"));
    IT_CHECK(pump_until([&]() { return a_nav && a_source.find("second.html") != std::string::npos; }));
    IT_CHECK(a_history);
    IT_CHECK(a.can_go_back());
    IT_CHECK(!a.can_go_forward());
    IT_CHECK(eval(a, "window.__injected") == std::optional<std::string>{"7"});
    IT_CHECK(script_id.has_value() && a.remove_script_on_document_created(*script_id));
    a_nav = false;
    IT_CHECK(a.go_back());
    IT_CHECK(pump_until([&]() { return a_nav && a_source.find("index.html") != std::string::npos; }));
    IT_CHECK(a.can_go_forward());
    IT_CHECK(a.set_zoom_factor(1.25));
    IT_CHECK(a.zoom_factor().has_value() && *a.zoom_factor() > 1.2);
    IT_CHECK(a.set_zoom_factor(1.0));
    IT_CHECK(a.set_muted(true));
    IT_CHECK(a.is_muted() == std::optional<bool>{true});
    IT_CHECK(a.set_muted(false));
    IT_CHECK(a.is_playing_audio() == std::optional<bool>{false});
    IT_CHECK(a.contains_fullscreen_element() == std::optional<bool>{false});
    IT_CHECK(a.apply_settings(boson::core::WebViewSettings{
        .default_context_menus_enabled = false,
        .zoom_control_enabled = false,
        .browser_accelerator_keys_enabled = false,
        .password_autosave_enabled = false,
        .general_autofill_enabled = false,
        .user_agent = L"BosonIntegration/1.0",
    }));
    IT_CHECK(eval(a, "navigator.userAgent") == std::optional<std::string>{"\"BosonIntegration/1.0\""});

    bool nav_cancelled = false;
    a.events().on_navigation_starting = [&](boson::core::NavigationStartingEvent& e) {
        if (e.uri.find("blocked.example") != std::string::npos) {
            e.cancel = true;
            nav_cancelled = true;
        }
    };
    a.navigate("https://blocked.example/");
    IT_CHECK(pump_until([&]() { return nav_cancelled; }, std::chrono::seconds{5}));
    pump_for(std::chrono::milliseconds{200});
    IT_CHECK(contains(a.source(), "index.html"));
    a.events().on_navigation_starting = {};

    // ---- #4: bridge responses, events from a worker thread, origin checks -------------------------------
    step("bridge: RPC replies, emit from worker thread, origin filtering");
    EchoController echo;
    boson::ipc::Router<EchoController> router(echo);
    auto bridge = std::make_unique<boson::ipc::Bridge>(runner, marshal, router);
    bridge->attach(a);
    IT_CHECK(eval(a, "chrome.webview.postMessage({id:5,method:'Test.echo',params:{text:'hi'}}); 1").has_value());
    IT_CHECK(pump_until([&]() { return contains(eval(a, "window.__responses.length"), "1"); }, std::chrono::seconds{10}));
    IT_CHECK(contains(eval(a, "JSON.stringify(window.__responses[0])"), "hi!"));

    runner.spawn([&bridge]() { bridge->emit("test:event", EchoResult{.text = "from-worker", .thread_id = GetCurrentThreadId()}); });
    IT_CHECK(pump_until([&]() { return contains(eval(a, "window.__events.length"), "1"); }, std::chrono::seconds{10}));
    IT_CHECK(eval(a, "window.__events[0].event") == std::optional<std::string>{"\"test:event\""});
    IT_CHECK(contains(eval(a, "JSON.stringify(window.__events[0])"), "from-worker"));

    boson::ipc::BridgeAttachOptions untrusted;
    untrusted.allowed_origins = {"https://example.com"};
    bridge->attach(b, untrusted);
    IT_CHECK(eval(b, "chrome.webview.postMessage({id:9,method:'Test.echo',params:{text:'nope'}}); 1").has_value());
    pump_for(std::chrono::milliseconds{600});
    IT_CHECK(eval(b, "window.__responses.length") == std::optional<std::string>{"0"});

    bridge->emit_to(a.id(), "only:a", EchoResult{.text = "a"});
    pump_for(std::chrono::milliseconds{300});
    IT_CHECK(contains(eval(a, "window.__events.length"), "2"));
    IT_CHECK(eval(b, "window.__events.filter(e => e.event === 'only:a').length") == std::optional<std::string>{"0"});

    // ---- #7: suspension keeps state -------------------------------------------------------------------
    step("suspend / resume / memory target");
    IT_CHECK(eval(b, "window.__keep = 42") == std::optional<std::string>{"42"});
    b.set_visible(false);
    IT_CHECK(b.set_memory_usage_target(boson::core::MemoryUsageTarget::Low));
    std::optional<bool> suspended;
    IT_CHECK(b.try_suspend([&](HRESULT hr, bool ok) { suspended = SUCCEEDED(hr) && ok; }));
    IT_CHECK(pump_until([&]() { return suspended.has_value(); }));
    IT_CHECK(suspended == std::optional<bool>{true});
    IT_CHECK(b.is_suspended() == std::optional<bool>{true});
    b.set_visible(true);
    IT_CHECK(b.memory_usage_target() == boson::core::MemoryUsageTarget::Normal);
    IT_CHECK(pump_until([&]() { return b.is_suspended() == std::optional<bool>{false}; }, std::chrono::seconds{5}));
    IT_CHECK(eval(b, "window.__keep") == std::optional<std::string>{"42"});

    // SleepPolicy puts a hidden, silent WebView to sleep.
    bool slept = false;
    host->set_on_slept([&](boson::core::WebView& w) { slept = &w == &b; });
    host->set_sleep_policy(boson::core::SleepPolicy{
        .hidden_timeout = std::chrono::milliseconds{50},
        .check_interval = std::chrono::milliseconds{100},
    });
    b.set_visible(false);
    IT_CHECK(pump_until([&]() { return slept; }, std::chrono::seconds{10}));
    IT_CHECK(host->is_sleeping(b));
    IT_CHECK(b.is_suspended() == std::optional<bool>{true});
    // Woken (shown) and hidden again before the next check: it must become eligible for sleep again instead of
    // staying marked as sleeping forever.
    b.set_visible(true);
    IT_CHECK(!host->is_sleeping(b));
    slept = false;
    b.set_visible(false); // no message pumped in between, so no sleep check ran
    IT_CHECK(!host->is_sleeping(b));
    IT_CHECK(pump_until([&]() { return slept; }, std::chrono::seconds{10}));
    IT_CHECK(host->is_sleeping(b));
    IT_CHECK(b.is_suspended() == std::optional<bool>{true});
    host->set_sleep_policy(std::nullopt);
    b.set_visible(true);
    host->check_sleep_now();
    IT_CHECK(pump_until([&]() { return b.is_suspended() == std::optional<bool>{false}; }, std::chrono::seconds{5}));

    // ---- #6: NewWindowRequested redirected into a new Boson WebView ----------------------------------
    step("NewWindowRequested -> new WebView");
    boson::core::WebView* popup = nullptr;
    std::string popup_uri;
    std::shared_ptr<boson::core::NewWindowDeferral> deferral;
    bool popup_completed = false;
    a.events().on_new_window_requested = [&](boson::core::NewWindowRequest& req) {
        popup_uri = req.uri();
        deferral = std::make_shared<boson::core::NewWindowDeferral>(req.defer());
        boson::core::WebViewConfig cfg;
        cfg.initial_url.clear(); // a NewWindow target must not have navigated
        popup = &host->create(cfg, boson::core::WebViewCallbacks{
            .on_ready = [&]() {
                if (deferral && deferral->pending() && popup != nullptr) {
                    popup_completed = deferral->complete_with(*popup);
                }
            },
        });
    };
    IT_CHECK(eval(a, "window.open('boson://app/popup.html'); 1").has_value());
    IT_CHECK(pump_until([&]() { return popup != nullptr && popup_completed && contains(popup->source(), "popup.html"); }));
    IT_CHECK(popup_uri.find("popup.html") != std::string::npos);
    if (popup != nullptr) {
        IT_CHECK(pump_until([&]() { return popup->document_title() == std::optional<std::string>{"Popup"}; }, std::chrono::seconds{10}));
        bool close_requested = false;
        popup->events().on_window_close_requested = [&]() { close_requested = true; };
        IT_CHECK(eval(*popup, "window.close(); 1").has_value());
        IT_CHECK(pump_until([&]() { return close_requested; }, std::chrono::seconds{5}));
        host->destroy(*popup);
        popup = nullptr;
        pump_for(std::chrono::milliseconds{100});
        IT_CHECK(host->webviews().size() == 2);
    }

    // Diagnostic for the Snap Layouts overlay: are the WebView HWNDs owned by another thread?
    {
        struct Ctx {
            DWORD ui_thread;
            int foreign = 0;
            int own = 0;
        } ctx{GetCurrentThreadId()};
        EnumChildWindows(window->hwnd(), [](HWND child, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            (GetWindowThreadProcessId(child, nullptr) == c->ui_thread ? c->own : c->foreign)++;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        std::cout << "  child HWNDs: " << ctx.own << " on UI thread, " << ctx.foreign << " on other threads\n";
        IT_CHECK(ctx.foreign > 0);
    }

    // Snap Layouts: over the registered maximize-button rect, the topmost window under the pointer must be
    // Boson's caption overlay (above the WebView HWNDs), answering HTMAXBUTTON.
    step("Snap Layouts maximize-button hit testing over a WebView");
    {
        const RECT bounds_a = a.bounds();
        const RECT max_rect{bounds_a.right - window->dip_to_px(92), 0, bounds_a.right - window->dip_to_px(46), window->dip_to_px(32)};
        window->set_maximize_button_rect(max_rect);
        pump_for(std::chrono::milliseconds{100});
        POINT inside{(max_rect.left + max_rect.right) / 2, (max_rect.top + max_rect.bottom) / 2};
        POINT outside{window->dip_to_px(20), window->dip_to_px(20)};
        ClientToScreen(window->hwnd(), &inside);
        ClientToScreen(window->hwnd(), &outside);
        const HWND hit_inside = WindowFromPoint(inside);
        const HWND hit_outside = WindowFromPoint(outside);
        wchar_t cls_in[128]{};
        wchar_t cls_out[128]{};
        GetClassNameW(hit_inside, cls_in, 128);
        GetClassNameW(hit_outside, cls_out, 128);
        std::wcout << L"  WindowFromPoint inside max rect: " << cls_in << L", outside: " << cls_out << L'\n';
        const auto overlay = static_cast<HWND>(GetPropW(window->hwnd(), L"BosonCaptionOverlay"));
        IT_CHECK(overlay != nullptr && hit_inside == overlay);
        IT_CHECK(hit_outside != overlay);
        IT_CHECK(SendMessageW(overlay, WM_NCHITTEST, 0, MAKELPARAM(inside.x, inside.y)) == HTMAXBUTTON);
        bool pressed_seen = false;
        const auto press_listener = window->on(boson::core::WindowEventKind::MaximizeButtonPress,
            [&](boson::core::WindowEvent& e) { pressed_seen = pressed_seen || e.flag; });
        SendMessageW(overlay, WM_NCLBUTTONDOWN, HTMAXBUTTON, MAKELPARAM(inside.x, inside.y));
        SendMessageW(overlay, WM_NCLBUTTONUP, HTMAXBUTTON, MAKELPARAM(inside.x, inside.y));
        IT_CHECK(pressed_seen);
        IT_CHECK(window->is_maximized());
        window->maximize_or_restore();
        pump_for(std::chrono::milliseconds{200});
        IT_CHECK(!window->is_maximized());
        window->off(press_listener);
        window->set_maximize_button_rect(max_rect); // re-place after the size change
        IT_CHECK(SendMessageW(window->hwnd(), WM_NCHITTEST, 0, MAKELPARAM(inside.x, inside.y)) == HTMAXBUTTON);
        // A WebView raised later must not cover the overlay.
        b.bring_to_front();
        a.bring_to_front();
        IT_CHECK(WindowFromPoint(inside) == overlay);
        window->set_maximize_button_rect(std::nullopt);
        IT_CHECK(WindowFromPoint(inside) != overlay);
    }

    // ---- #10 / #9: composition hosting, input forwarding, app-region --------------------------------
    step("composition hosting + CSS app-region");
    boson::core::WindowConfig comp_cfg;
    comp_cfg.title = L"Boson composition test";
    comp_cfg.width = 700;
    comp_cfg.height = 500;
    comp_cfg.quit_on_destroy = false;
    auto comp_window = std::make_unique<boson::core::Window>(comp_cfg);
    IT_CHECK(comp_window->create(nullptr, SW_SHOWNOACTIVATE));
    auto comp_host = std::make_unique<boson::core::WebViewHost>(*comp_window, env, boson::core::HostingMode::Composition);
    IT_CHECK(comp_host->hosting() == boson::core::HostingMode::Composition);
    comp_host->set_default_asset_lookup(&lookup);
    bool d_nav = false;
    boson::core::WebViewConfig d_cfg;
    d_cfg.initial_url = "boson://app/drag.html";
    d_cfg.settings.non_client_region_support_enabled = true;
    boson::core::WebView& d = comp_host->create(d_cfg);
    d.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { d_nav = e.is_success; };
    IT_CHECK(pump_until([&]() { return d.is_ready() && d_nav; }));
    IT_CHECK(d.composition_controller() != nullptr);
    IT_CHECK(eval(d, "document.title") == std::optional<std::string>{"\"Drag\""});
    IT_CHECK(d.set_opacity(0.9f));
    IT_CHECK(d.set_corner_radius(8.0f));
    IT_CHECK(d.cursor() != nullptr);

    const bool caption = pump_until([&]() {
        return d.non_client_region_at(POINT{20, 20}) == std::optional{COREWEBVIEW2_NON_CLIENT_REGION_KIND_CAPTION};
    }, std::chrono::seconds{10});
    IT_CHECK(caption);
    IT_CHECK(d.non_client_region_at(POINT{20, 200}) == std::optional{COREWEBVIEW2_NON_CLIENT_REGION_KIND_CLIENT});
    {
        POINT top{20, 20};
        POINT body{20, 200};
        ClientToScreen(comp_window->hwnd(), &top);
        ClientToScreen(comp_window->hwnd(), &body);
        IT_CHECK(SendMessageW(comp_window->hwnd(), WM_NCHITTEST, 0, MAKELPARAM(top.x, top.y)) == HTCAPTION);
        IT_CHECK(SendMessageW(comp_window->hwnd(), WM_NCHITTEST, 0, MAKELPARAM(body.x, body.y)) == HTCLIENT);
    }
    for (int i = 0; i < 5; ++i) {
        SendMessageW(comp_window->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(100 + i * 5, 200));
    }
    IT_CHECK(pump_until([&]() {
        const auto moves = eval(d, "window.__moves");
        return moves.has_value() && *moves != "0";
    }, std::chrono::seconds{5}));

    // ---- #2: recreate with new browser arguments, in place ------------------------------------------
    step("Environment::recreate with new browser args");
    const auto old_pid = env->browser_process_id();
    IT_CHECK(old_pid.has_value());
    std::optional<bool> recreated;
    boson::core::EnvironmentConfig new_cfg = env_cfg;
    new_cfg.additional_browser_arguments.push_back(L"--disable-gpu");
    new_cfg.disable_features = {L"HardwareSecureDecryption"};
    const auto t0 = std::chrono::steady_clock::now();
    env->recreate(new_cfg, [&](bool ok) { recreated = ok; });
    IT_CHECK(pump_until([&]() { return recreated.has_value() && a.is_ready() && b.is_ready() && d.is_ready(); }, std::chrono::seconds{30}));
    std::cout << "  recreate took "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
              << " ms\n";
    IT_CHECK(recreated == std::optional<bool>{true});
    IT_CHECK(SUCCEEDED(env->creation_result()));
    IT_CHECK(env->browser_arguments().find(L"--disable-gpu") != std::wstring::npos);
    const auto new_pid = env->browser_process_id();
    IT_CHECK(new_pid.has_value() && new_pid != old_pid);
    IT_CHECK(count_kind(env->process_infos(), boson::core::ProcessKind::Browser) == 1);
    IT_CHECK(pump_until([&]() { return contains(a.source(), "index.html") && contains(b.source(), "second.html"); }, std::chrono::seconds{10}));
    if (old_pid.has_value()) {
        HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, *old_pid);
        IT_CHECK(old == nullptr || WaitForSingleObject(old, 0) == WAIT_OBJECT_0);
        if (old != nullptr) CloseHandle(old);
    }
    IT_CHECK(eval(a, "1 + 41") == std::optional<std::string>{"42"});

    // ---- #11: graceful shutdown waits for the browser process --------------------------------------
    step("graceful shutdown");
    bool shut_down = false;
    env->shutdown([&]() { shut_down = true; });
    IT_CHECK(pump_until([&]() { return shut_down; }, std::chrono::seconds{10}));
    IT_CHECK(!a.is_ready() && !b.is_ready() && !d.is_ready());
    if (new_pid.has_value()) {
        HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, *new_pid);
        IT_CHECK(proc == nullptr || WaitForSingleObject(proc, 2000) == WAIT_OBJECT_0);
        if (proc != nullptr) CloseHandle(proc);
    }

    bridge.reset();
    comp_host.reset();
    host.reset();
    comp_window.reset();
    window.reset();
    std::error_code ec;
    std::filesystem::remove_all(user_data, ec);

    std::cout << (g_checks - g_failures) << "/" << g_checks << " integration checks passed\n";
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
