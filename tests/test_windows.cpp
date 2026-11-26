// Window manager: windows by label, owned / modal windows, close veto with asynchronous confirmation,
// hide-to-tray, quit on last window, per-label state persistence, Window controller routing and emit_to(label).
// The WebView2 part (shared environment, process count, memory per window, IPC routing) skips when no runtime
// is installed. Windows are shown with SW_SHOWNOACTIVATE so the test does not steal the focus.
#include <windows.h>
#include <objbase.h>
#include <psapi.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boson/boson.hpp>

#include <exec/async_scope.hpp>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_windows.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define WT_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

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

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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

bool contains(const std::optional<std::string>& haystack, std::string_view needle) {
    return haystack.has_value() && haystack->find(needle) != std::string::npos;
}

std::filesystem::path temp_dir(std::wstring_view tag) {
    wchar_t buffer[MAX_PATH + 1]{};
    GetTempPathW(MAX_PATH, buffer);
    auto dir = std::filesystem::path(buffer) / (std::wstring(tag) + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    return dir;
}

boson::app::ManagedWindowConfig small_window(std::string label) {
    boson::app::ManagedWindowConfig config;
    config.label = std::move(label);
    config.window.title = L"Boson WM test - " + boson::core::utf8_to_wide(config.label);
    config.window.width = 420;
    config.window.height = 300;
    config.focus = false; // SW_SHOWNOACTIVATE
    return config;
}

struct EventLog {
    std::vector<std::pair<boson::app::WindowManagerEventKind, std::string>> entries;
    [[nodiscard]] int count(boson::app::WindowManagerEventKind kind, std::string_view label) const {
        int n = 0;
        for (const auto& [k, l] : entries) {
            n += (k == kind && l == label) ? 1 : 0;
        }
        return n;
    }
};

std::size_t count_kind(const std::vector<boson::core::ProcessInfo>& infos, boson::core::ProcessKind kind) {
    std::size_t n = 0;
    for (const auto& info : infos) {
        n += info.kind == kind ? 1 : 0;
    }
    return n;
}

std::uint64_t private_bytes(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return 0;
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    std::uint64_t bytes = 0;
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        bytes = counters.PrivateUsage;
    }
    CloseHandle(process);
    return bytes;
}

/// Private bytes of this process plus every process of the WebView2 environment.
std::uint64_t tree_private_bytes(const boson::core::Environment& env) {
    std::uint64_t total = private_bytes(GetCurrentProcessId());
    for (const auto& info : env.process_infos()) {
        total += private_bytes(info.pid);
    }
    return total;
}

double mib(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

constexpr std::string_view kPage =
    "<!doctype html><html><head><title>wm</title><script>"
    "window.__events=[];window.__responses=[];window.__pending={};window.__id=0;"
    "window.__count=(name)=>window.__events.filter(e=>e.event===name).length;"
    "window.__call=(id,method,params)=>chrome.webview.postMessage({id,method,params});"
    "window.__rpc=(method,params)=>new Promise(r=>{const id=100000+(++window.__id);window.__pending[id]=r;"
    "chrome.webview.postMessage({id,method,params});});"
    "if(window.chrome&&chrome.webview){chrome.webview.addEventListener('message',e=>{const d=e.data;"
    "if(d&&d.type==='event'){window.__events.push(d);"
    "if(d.event==='window:close-requested'&&window.__autoAnswer!==undefined){"
    "window.__call(900+window.__events.length,'Window.confirm_close',"
    "{label:'',request_id:d.data.request_id,allow:window.__autoAnswer});}}"
    "else if(d&&window.__pending[d.id]){const r=window.__pending[d.id];delete window.__pending[d.id];r(d);}"
    "else{window.__responses.push(d);}});}"
    "</script></head><body>window manager test</body></html>";

/// JSON of the response with this id once it arrived (polls the page).
std::optional<std::string> response(boson::core::WebView& webview, int id) {
    std::optional<std::string> out;
    const std::string js = "JSON.stringify(window.__responses.find(r=>r.id===" + std::to_string(id) + ")||null)";
    pump_until([&]() {
        out = eval(webview, js);
        return out.has_value() && *out != "\"null\"";
    }, std::chrono::seconds{10});
    return out;
}

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/index.html" || path == "/popout.html") {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(kPage.data()), kPage.size());
    }
    return std::nullopt;
}

// ---- Unit-level checks: no WebView2 needed ---------------------------------------------------------------

void test_labels_and_urls() {
    step("labels, URL resolution, capability window patterns");
    using boson::app::WindowManager;
    WT_CHECK(WindowManager::is_valid_label("main"));
    WT_CHECK(WindowManager::is_valid_label("workspace-1"));
    WT_CHECK(WindowManager::is_valid_label("orca:dashboard/2.x_y"));
    WT_CHECK(!WindowManager::is_valid_label(""));
    WT_CHECK(!WindowManager::is_valid_label("has space"));
    WT_CHECK(!WindowManager::is_valid_label("quote\""));
    WT_CHECK(!WindowManager::is_valid_label(std::string(65, 'a')));

    WindowManager wm({});
    WT_CHECK(wm.resolve_url("") == "boson://app/index.html");
    WT_CHECK(wm.resolve_url("/popout.html") == "boson://app/popout.html");
    WT_CHECK(wm.resolve_url("settings.html?tab=1") == "boson://app/settings.html?tab=1");
    WT_CHECK(wm.resolve_url("https://example.com/x") == "https://example.com/x");
    WindowManager dev({.dev_url = std::string("http://localhost:5173/")});
    WT_CHECK(dev.resolve_url("/popout.html") == "http://localhost:5173/popout.html");

    boson::security::CapabilityConfig cfg;
    cfg.capabilities.push_back(boson::security::Capability{
        .identifier = "windows",
        .description = {},
        .origins = {"boson://app"},
        .allow = {"Window.*"},
        .deny = {},
        .windows = {"settings", "workspace-*"},
    });
    const auto policy = boson::security::CapabilityPolicy::make_shared(cfg);
    WT_CHECK(policy.has_value());
    if (policy) {
        WT_CHECK((*policy)->may_create_window("settings", "boson://app/index.html"));
        WT_CHECK((*policy)->may_create_window("workspace-7", "boson://app/index.html"));
        WT_CHECK(!(*policy)->may_create_window("settings2", "boson://app/index.html"));
        WT_CHECK(!(*policy)->may_create_window("settings", "https://evil.example/"));
    }
    WT_CHECK(!boson::security::CapabilityPolicy::allow_all()->may_create_window("x", "boson://app/"));
    cfg.capabilities[0].windows = {"bad label"};
    WT_CHECK(!boson::security::CapabilityPolicy::compile(cfg).has_value());
    const auto parsed = boson::security::CapabilityPolicy::from_json(
        R"({"default_deny":true,"capabilities":[{"identifier":"w","origins":[],"allow":["Window.create"],"windows":["*"]}]})");
    WT_CHECK(parsed.has_value() && parsed->may_create_window("anything", "boson://app/"));
}

void test_create_find_modal_owned() {
    step("create 3 windows by label, find, owned + modal windows");
    boson::app::WindowManager wm({.quit_on_last_window_closed = false});
    EventLog log;
    wm.on_event([&log](const boson::app::WindowManagerEvent& e) { log.entries.emplace_back(e.kind, std::string(e.label)); });

    auto main = wm.create(small_window("main"));
    auto dashboard = wm.create(small_window("dashboard"));
    auto workspace = wm.create(small_window("workspace-1"));
    WT_CHECK(main.has_value() && dashboard.has_value() && workspace.has_value());
    if (!main || !dashboard || !workspace) {
        return;
    }
    WT_CHECK(wm.size() == 3);
    WT_CHECK(wm.labels() == (std::vector<std::string>{"main", "dashboard", "workspace-1"}));
    WT_CHECK(wm.find("dashboard") == *dashboard);
    WT_CHECK(wm.find_by_hwnd((*workspace)->hwnd()) == *workspace);
    WT_CHECK(wm.find("nope") == nullptr);
    WT_CHECK(IsWindowVisible((*main)->hwnd()) != FALSE);
    WT_CHECK(log.count(boson::app::WindowManagerEventKind::Created, "dashboard") == 1);
    WT_CHECK(log.count(boson::app::WindowManagerEventKind::Shown, "dashboard") == 1);
    WT_CHECK(!wm.create(small_window("main")).has_value());          // duplicate label
    WT_CHECK(!wm.create(small_window("bad label")).has_value());     // invalid label
    auto orphan = small_window("orphan");
    orphan.parent = "missing";
    WT_CHECK(!wm.create(orphan).has_value());                 // unknown parent
    auto modal_without_parent = small_window("m0");
    modal_without_parent.modal = true;
    WT_CHECK(!wm.create(modal_without_parent).has_value());

    // Owned window: Win32 owner is the parent.
    auto palette_cfg = small_window("palette");
    palette_cfg.parent = "main";
    auto palette = wm.create(palette_cfg);
    WT_CHECK(palette.has_value() && GetWindow((*palette)->hwnd(), GW_OWNER) == (*main)->hwnd());

    // skip_taskbar: owned by the invisible taskbar owner.
    auto tool_cfg = small_window("tool");
    tool_cfg.skip_taskbar = true;
    auto tool = wm.create(tool_cfg);
    WT_CHECK(tool.has_value());
    if (tool) {
        const HWND owner = GetWindow((*tool)->hwnd(), GW_OWNER);
        WT_CHECK(owner != nullptr && IsWindowVisible(owner) == FALSE);
    }

    // Modal: owner disabled while open, centered on the owner, re-enabled on close. Nested count.
    RECT main_rect{};
    GetWindowRect((*main)->hwnd(), &main_rect);
    auto dialog_cfg = small_window("dialog");
    dialog_cfg.parent = "main";
    dialog_cfg.modal = true;
    dialog_cfg.window.width = 240;
    dialog_cfg.window.height = 160;
    auto dialog = wm.create(dialog_cfg);
    WT_CHECK(dialog.has_value());
    if (dialog) {
        WT_CHECK(IsWindowEnabled((*main)->hwnd()) == FALSE);
        WT_CHECK(GetWindow((*dialog)->hwnd(), GW_OWNER) == (*main)->hwnd());
        RECT d{};
        GetWindowRect((*dialog)->hwnd(), &d);
        const int dx = ((d.left + d.right) / 2) - ((main_rect.left + main_rect.right) / 2);
        const int dy = ((d.top + d.bottom) / 2) - ((main_rect.top + main_rect.bottom) / 2);
        WT_CHECK(std::abs(dx) <= 2 && std::abs(dy) <= 2);
        WT_CHECK((*dialog)->is_modal());

        auto nested_cfg = small_window("dialog2");
        nested_cfg.parent = "main";
        nested_cfg.modal = true;
        auto nested = wm.create(nested_cfg);
        WT_CHECK(nested.has_value());
        WT_CHECK(wm.focus("main")); // hands the focus to a modal child
        WT_CHECK(wm.close("dialog"));
        WT_CHECK(pump_until([&]() { return wm.find("dialog") == nullptr; }, std::chrono::seconds{5}));
        WT_CHECK(IsWindowEnabled((*main)->hwnd()) == FALSE); // dialog2 still open
        WT_CHECK(wm.destroy("dialog2"));
        WT_CHECK(IsWindowEnabled((*main)->hwnd()) != FALSE);
        WT_CHECK(pump_until([&]() { return log.count(boson::app::WindowManagerEventKind::Closed, "dialog2") == 1; }, std::chrono::seconds{5}));
        WT_CHECK(log.count(boson::app::WindowManagerEventKind::Closed, "dialog") == 1);
    }

    // Window events: move / resize / maximize / minimize / restore.
    log.entries.clear();
    (*dashboard)->window().set_bounds(RECT{120, 140, 620, 540});
    WT_CHECK(log.count(boson::app::WindowManagerEventKind::Moved, "dashboard") >= 1);
    WT_CHECK(log.count(boson::app::WindowManagerEventKind::Resized, "dashboard") >= 1);
    ShowWindow((*dashboard)->hwnd(), SW_MAXIMIZE);
    WT_CHECK(log.count(boson::app::WindowManagerEventKind::Maximized, "dashboard") == 1);
    ShowWindow((*dashboard)->hwnd(), SW_RESTORE);
    WT_CHECK(log.count(boson::app::WindowManagerEventKind::Restored, "dashboard") == 1);
    ShowWindow((*dashboard)->hwnd(), SW_SHOWMINNOACTIVE);
    WT_CHECK(log.count(boson::app::WindowManagerEventKind::Minimized, "dashboard") == 1);
    ShowWindow((*dashboard)->hwnd(), SW_SHOWNOACTIVATE);
    WT_CHECK(log.count(boson::app::WindowManagerEventKind::Restored, "dashboard") == 2);
    WT_CHECK(wm.hide("dashboard") && IsWindowVisible((*dashboard)->hwnd()) == FALSE);
    WT_CHECK(log.count(boson::app::WindowManagerEventKind::Hidden, "dashboard") == 1);
    WT_CHECK(wm.show("dashboard", false) && IsWindowVisible((*dashboard)->hwnd()) != FALSE);

    // Destroying an owner destroys its owned windows too.
    WT_CHECK(wm.destroy("main"));
    WT_CHECK(pump_until([&]() { return wm.find("palette") == nullptr && wm.find("main") == nullptr; }, std::chrono::seconds{5}));
    WT_CHECK(wm.size() == 3);
    wm.destroy_all();
    WT_CHECK(pump_until([&]() { return wm.windows().empty(); }, std::chrono::seconds{5}));
}

void test_close_veto() {
    step("close veto: prevent, deferred confirm, close guard, timeout, close_async, hide_on_close");
    boson::app::WindowManager wm({.quit_on_last_window_closed = false});
    bool prevent = true;
    std::optional<std::uint64_t> deferred;
    wm.on_close_requested([&](boson::app::CloseRequest& request) {
        if (request.label() == "veto" && prevent) {
            request.prevent();
        }
        if (request.label() == "ask") {
            request.defer();
            deferred = request.id();
        }
    });
    std::optional<std::uint64_t> guard_request;
    wm.on_event([&](const boson::app::WindowManagerEvent& e) {
        if (e.kind == boson::app::WindowManagerEventKind::CloseRequested) {
            guard_request = e.close_request_id;
        }
    });

    WT_CHECK(wm.create(small_window("veto")).has_value());
    WT_CHECK(wm.close("veto"));
    pump_for(std::chrono::milliseconds{100});
    WT_CHECK(wm.find("veto") != nullptr);
    prevent = false;
    WT_CHECK(wm.close("veto"));
    WT_CHECK(pump_until([&]() { return wm.find("veto") == nullptr; }, std::chrono::seconds{5}));

    // Deferred: a C++ handler asks the user asynchronously.
    WT_CHECK(wm.create(small_window("ask")).has_value());
    SendMessageW(wm.find("ask")->hwnd(), WM_CLOSE, 0, 0); // the X button / Alt+F4 path
    WT_CHECK(deferred.has_value() && wm.find("ask") != nullptr && wm.find("ask")->is_closing());
    WT_CHECK(!wm.resolve_close("ask", *deferred + 100, true)); // wrong id
    WT_CHECK(wm.resolve_close("ask", *deferred, false));       // "Cancel"
    WT_CHECK(wm.find("ask") != nullptr && !wm.find("ask")->is_closing());
    deferred.reset();
    SendMessageW(wm.find("ask")->hwnd(), WM_CLOSE, 0, 0);
    WT_CHECK(deferred.has_value() && wm.resolve_close("ask", *deferred, true)); // "Close anyway"
    WT_CHECK(pump_until([&]() { return wm.find("ask") == nullptr; }, std::chrono::seconds{5}));

    // Close guard (the page answers through Window.confirm_close).
    WT_CHECK(wm.create(small_window("guarded")).has_value());
    WT_CHECK(wm.set_close_guard("guarded", true));
    SendMessageW(wm.find("guarded")->hwnd(), WM_CLOSE, 0, 0);
    WT_CHECK(guard_request.has_value() && wm.find("guarded") != nullptr);
    WT_CHECK(wm.resolve_close("guarded", *guard_request, true));
    WT_CHECK(pump_until([&]() { return wm.find("guarded") == nullptr; }, std::chrono::seconds{5}));

    // Unanswered guard with a timeout closes anyway.
    auto timed_cfg = small_window("timed");
    timed_cfg.close_confirm_timeout = std::chrono::milliseconds{150};
    WT_CHECK(wm.create(timed_cfg).has_value());
    WT_CHECK(wm.set_close_guard("timed", true));
    const auto timed_start = std::chrono::steady_clock::now();
    WT_CHECK(wm.close("timed"));
    WT_CHECK(pump_until([&]() { return wm.find("timed") == nullptr; }, std::chrono::seconds{5}));
    WT_CHECK(ms_since(timed_start) >= 140.0);

    // close_async (stdexec sender): false when vetoed, true when closed.
    prevent = true;
    WT_CHECK(wm.create(small_window("veto")).has_value());
    std::optional<bool> async_result;
    ::exec::async_scope scope;
    scope.spawn(wm.close_async("veto") | stdexec::then([&](bool closed) noexcept { async_result = closed; }) |
                stdexec::upon_error([&](std::exception_ptr) noexcept { async_result = false; }));
    WT_CHECK(pump_until([&]() { return async_result.has_value(); }, std::chrono::seconds{5}));
    WT_CHECK(async_result == std::optional<bool>{false});
    prevent = false;
    async_result.reset();
    scope.spawn(wm.close_async("veto") | stdexec::then([&](bool closed) noexcept { async_result = closed; }) |
                stdexec::upon_error([&](std::exception_ptr) noexcept { async_result = false; }));
    WT_CHECK(pump_until([&]() { return async_result.has_value(); }, std::chrono::seconds{5}));
    WT_CHECK(async_result == std::optional<bool>{true});
    stdexec::sync_wait(scope.on_empty());

    // Hidden-to-tray: close hides, destroy closes.
    auto tray_cfg = small_window("tray");
    tray_cfg.hide_on_close = true;
    WT_CHECK(wm.create(tray_cfg).has_value());
    WT_CHECK(wm.close("tray"));
    pump_for(std::chrono::milliseconds{100});
    WT_CHECK(wm.find("tray") != nullptr && IsWindowVisible(wm.find("tray")->hwnd()) == FALSE);
    WT_CHECK(wm.show("tray", false) && IsWindowVisible(wm.find("tray")->hwnd()) != FALSE);
    WT_CHECK(wm.destroy("tray"));
    WT_CHECK(pump_until([&]() { return wm.find("tray") == nullptr; }, std::chrono::seconds{5}));
}

void test_quit_on_last_window() {
    step("quit when the last window closes (configurable)");
    int last_closed = 0;
    boson::app::WindowManager wm({.on_last_window_closed = [&last_closed]() { ++last_closed; }});
    WT_CHECK(wm.create(small_window("a")).has_value());
    WT_CHECK(wm.create(small_window("b")).has_value());
    WT_CHECK(wm.destroy("a"));
    pump_for(std::chrono::milliseconds{50});
    WT_CHECK(last_closed == 0);
    WT_CHECK(wm.close("b"));
    WT_CHECK(pump_until([&]() { return last_closed == 1; }, std::chrono::seconds{5}));

    // A hidden (tray) window still counts as open.
    auto tray_cfg = small_window("tray");
    tray_cfg.hide_on_close = true;
    WT_CHECK(wm.create(tray_cfg).has_value());
    WT_CHECK(wm.close("tray"));
    pump_for(std::chrono::milliseconds{100});
    WT_CHECK(last_closed == 1);
    // destroy_all() never runs the last-window action.
    wm.destroy_all();
    pump_for(std::chrono::milliseconds{100});
    WT_CHECK(last_closed == 1 && wm.windows().empty());
    wm.set_quit_on_last_window_closed(false);
    WT_CHECK(wm.create(small_window("c")).has_value());
    WT_CHECK(wm.destroy("c"));
    pump_for(std::chrono::milliseconds{100});
    WT_CHECK(last_closed == 1);
}

void test_state_persistence() {
    step("window state persistence per label");
    const auto dir = temp_dir(L"boson-wm-state-");
    const RECT a_rect{80, 90, 80 + 500, 90 + 360};
    const RECT b_rect{260, 210, 260 + 430, 210 + 330};
    {
        boson::app::WindowManager wm({.quit_on_last_window_closed = false, .state_directory = dir});
        auto a_cfg = small_window("alpha");
        a_cfg.persist_state = true;
        auto b_cfg = small_window("beta");
        b_cfg.persist_state = true;
        auto a = wm.create(a_cfg);
        auto b = wm.create(b_cfg);
        WT_CHECK(a.has_value() && b.has_value());
        if (a && b) {
            (*a)->window().set_bounds(a_rect);
            (*b)->window().set_bounds(b_rect);
        }
        WT_CHECK(wm.close("alpha"));
        WT_CHECK(wm.close("beta"));
        WT_CHECK(pump_until([&]() { return wm.windows().empty(); }, std::chrono::seconds{5}));
    }
    WT_CHECK(std::filesystem::exists(dir / L"window-state-alpha.json"));
    WT_CHECK(std::filesystem::exists(dir / L"window-state-beta.json"));
    {
        boson::app::WindowManager wm({.quit_on_last_window_closed = false, .state_directory = dir});
        auto a_cfg = small_window("alpha");
        a_cfg.persist_state = true;
        auto b_cfg = small_window("beta");
        b_cfg.persist_state = true;
        auto a = wm.create(a_cfg);
        auto b = wm.create(b_cfg);
        WT_CHECK(a.has_value() && b.has_value());
        if (a && b) {
            const RECT ra = (*a)->window().bounds();
            const RECT rb = (*b)->window().bounds();
            const auto close_to = [](const RECT& x, const RECT& y) {
                return std::abs(x.left - y.left) <= 2 && std::abs(x.top - y.top) <= 2 && std::abs(x.right - y.right) <= 2 &&
                       std::abs(x.bottom - y.bottom) <= 2;
            };
            WT_CHECK(close_to(ra, a_rect));
            WT_CHECK(close_to(rb, b_rect));
            WT_CHECK((*a)->state_keeper() != nullptr);
        }
        wm.destroy_all();
        pump_until([&]() { return wm.windows().empty(); }, std::chrono::seconds{5});
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_performance() {
    step("performance: create / destroy, label lookup");
    boson::app::WindowManager wm({.quit_on_last_window_closed = false});
    constexpr int kWindows = 20;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kWindows; ++i) {
        auto cfg = small_window("perf-" + std::to_string(i));
        cfg.visible = false;
        WT_CHECK(wm.create(cfg).has_value());
    }
    const double create_ms = ms_since(start) / kWindows;
    constexpr int kLookups = 1'000'000;
    start = std::chrono::steady_clock::now();
    std::size_t found = 0;
    for (int i = 0; i < kLookups; ++i) {
        found += wm.find(i % 2 == 0 ? std::string_view{"perf-3"} : std::string_view{"perf-19"}) != nullptr ? 1 : 0;
    }
    const double lookup_ns = ms_since(start) * 1e6 / kLookups;
    WT_CHECK(found == static_cast<std::size_t>(kLookups));
    start = std::chrono::steady_clock::now();
    wm.destroy_all();
    WT_CHECK(pump_until([&]() { return wm.windows().empty(); }, std::chrono::seconds{5}));
    const double destroy_ms = ms_since(start) / kWindows;
    std::cout << std::fixed << std::setprecision(3) << "  [perf] create (no WebView, hidden): " << create_ms
              << " ms/window; destroy+cleanup: " << destroy_ms << " ms/window; find(label) among " << kWindows << ": "
              << lookup_ns << " ns\n";
}

// ---- WebView2 part ---------------------------------------------------------------------------------------

struct PageSet {
    boson::app::WindowManager& wm;
    [[nodiscard]] boson::core::WebView* webview(std::string_view label) const {
        boson::app::ManagedWindow* w = wm.find(label);
        return w != nullptr ? w->main_webview() : nullptr;
    }
    [[nodiscard]] bool loaded(std::string_view label) const {
        boson::core::WebView* wv = webview(label);
        return wv != nullptr && wv->is_ready() && eval(*wv, "typeof window.__call") == std::optional<std::string>{"\"function\""};
    }
    std::optional<std::string> js(std::string_view label, std::string_view code) const {
        boson::core::WebView* wv = webview(label);
        return wv != nullptr ? eval(*wv, code) : std::nullopt;
    }
};

boson::app::ManagedWindowConfig page_window(std::string label) {
    auto cfg = small_window(std::move(label));
    boson::core::WebViewConfig webview;
    webview.initial_url = "boson://app/index.html";
    cfg.webview = std::move(webview);
    return cfg;
}

void test_webview_windows() {
    {
        LPWSTR version = nullptr;
        if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
            std::cout << "SKIP (WebView2 part): no WebView2 runtime installed\n";
            return;
        }
        std::wcout << L"WebView2 runtime " << version << L'\n';
        CoTaskMemFree(version);
    }
    step("WebView2: shared environment, IPC routing by label");
    const auto user_data = temp_dir(L"boson-wm-udf-");

    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = user_data.wstring();
    boson::core::WebViewConfig template_webview;
    template_webview.initial_url = "boson://app/index.html";

    auto policy = boson::security::CapabilityPolicy::make_shared(boson::security::CapabilityConfig{
        .default_deny = false,
        .capabilities = {boson::security::Capability{
            .identifier = "popouts", .description = {}, .origins = {"boson://app"}, .allow = {}, .deny = {}, .windows = {"popout-*"}}},
    });
    WT_CHECK(policy.has_value());

    boson::app::WindowManager wm({
        .environment_config = env_cfg,
        .asset_lookup = &lookup,
        .default_webview = template_webview,
        .quit_on_last_window_closed = false,
    });
    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&wm]() { wm.wake_ui(); });
    wm.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    // Lazy environment: WebView-less windows never start a browser process.
    WT_CHECK(wm.create(small_window("native-only")).has_value());
    WT_CHECK(!wm.has_environment());
    WT_CHECK(wm.destroy("native-only"));

    PageSet pages{wm};
    const auto t_first = std::chrono::steady_clock::now();
    WT_CHECK(wm.create(page_window("main")).has_value());
    WT_CHECK(pump_until([&]() { return pages.loaded("main"); }, std::chrono::seconds{30}));
    const double first_ms = ms_since(t_first);
    const auto env = wm.environment();
    WT_CHECK(env != nullptr && env->is_ready());
    pump_for(std::chrono::milliseconds{1500}); // let the process set settle before measuring
    const auto infos_one = env->process_infos();
    const std::uint64_t mem_one = tree_private_bytes(*env);

    const auto t_more = std::chrono::steady_clock::now();
    WT_CHECK(wm.create(page_window("dashboard")).has_value());
    WT_CHECK(wm.create(page_window("workspace-1")).has_value());
    WT_CHECK(pump_until([&]() { return pages.loaded("dashboard") && pages.loaded("workspace-1"); }, std::chrono::seconds{30}));
    const double more_ms = ms_since(t_more) / 2.0;
    pump_for(std::chrono::milliseconds{1500});
    const auto infos_three = env->process_infos();
    const std::uint64_t mem_three = tree_private_bytes(*env);

    step("single shared environment: one browser process for three windows");
    WT_CHECK(env->webview_count() == 3);
    WT_CHECK(count_kind(infos_one, boson::core::ProcessKind::Browser) == 1);
    WT_CHECK(count_kind(infos_three, boson::core::ProcessKind::Browser) == 1);
    {
        UINT32 pids[3]{};
        int i = 0;
        for (const char* label : {"main", "dashboard", "workspace-1"}) {
            boson::core::WebView* wv = pages.webview(label);
            WT_CHECK(wv != nullptr && wv->environment() == env);
            if (wv != nullptr && wv->core_webview() != nullptr) {
                wv->core_webview()->get_BrowserProcessId(&pids[i]);
            }
            ++i;
        }
        WT_CHECK(pids[0] != 0 && pids[0] == pids[1] && pids[1] == pids[2]);
    }
    const double per_window_mib = (mib(mem_three) - mib(mem_one)) / 2.0;
    std::cout << std::fixed << std::setprecision(1) << "  [perf] processes: 1 window = " << infos_one.size() << " ("
              << count_kind(infos_one, boson::core::ProcessKind::Browser) << " browser, "
              << count_kind(infos_one, boson::core::ProcessKind::Renderer) << " renderer), 3 windows = " << infos_three.size()
              << " (" << count_kind(infos_three, boson::core::ProcessKind::Browser) << " browser, "
              << count_kind(infos_three, boson::core::ProcessKind::Renderer) << " renderer)\n"
              << "  [perf] private bytes (app + WebView2 tree): 1 window = " << mib(mem_one) << " MiB, 3 windows = " << mib(mem_three)
              << " MiB, per extra window = " << per_window_mib << " MiB\n"
              << "  [perf] first window to page loaded (incl. browser start) = " << first_ms
              << " ms, extra window to page loaded = " << more_ms << " ms\n";

    step("label script injected into every WebView");
    WT_CHECK(pages.js("dashboard", "window.__BOSON_WINDOW__.label") == std::optional<std::string>{"\"dashboard\""});

    // Bridge + Router with the Window controller and a sender-routed SystemController. The controllers are built
    // first; set_bridge() attaches the three existing WebViews with their window labels.
    boson::ipc::WindowController window_controller(wm, marshal, {.policy = policy ? *policy : nullptr});
    boson::ipc::SystemController system(marshal, boson::ipc::sender_resolver(wm));
    boson::ipc::Router<boson::ipc::WindowController, boson::ipc::SystemController> router(window_controller, system);
    boson::ipc::Bridge bridge(runner, marshal, router);
    wm.set_bridge(&bridge);
    for (const char* label : {"main", "dashboard", "workspace-1"}) {
        boson::core::WebView* wv = pages.webview(label);
        WT_CHECK(wv != nullptr && bridge.is_attached(wv->id()) && bridge.window_label_of(wv->id()) == label);
    }

    step("Bridge::emit_to(label) reaches only that window; emit reaches all");
    bridge.emit_to(std::string_view{"dashboard"}, "test:ping", boson::ipc::WindowLabelEvent{.label = "to-dashboard"});
    WT_CHECK(pump_until([&]() { return pages.js("dashboard", "window.__count('test:ping')") == std::optional<std::string>{"1"}; }));
    pump_for(std::chrono::milliseconds{300});
    WT_CHECK(pages.js("main", "window.__count('test:ping')") == std::optional<std::string>{"0"});
    WT_CHECK(pages.js("workspace-1", "window.__count('test:ping')") == std::optional<std::string>{"0"});
    bridge.emit("test:all", boson::ipc::WindowLabelEvent{.label = "everyone"});
    WT_CHECK(pump_until([&]() {
        return pages.js("main", "window.__count('test:all')") == std::optional<std::string>{"1"} &&
               pages.js("dashboard", "window.__count('test:all')") == std::optional<std::string>{"1"} &&
               pages.js("workspace-1", "window.__count('test:all')") == std::optional<std::string>{"1"};
    }));

    step("Window.emit from a page to another window by label");
    pages.js("main", "window.__call(1,'Window.emit',{target:'workspace-1',event:'test:hello',payload:{n:42}});1");
    WT_CHECK(contains(response(*pages.webview("main"), 1), "\\\"status\\\":\\\"success\\\""));
    WT_CHECK(pump_until([&]() { return pages.js("workspace-1", "window.__count('test:hello')") == std::optional<std::string>{"1"}; }));
    WT_CHECK(pages.js("workspace-1", "window.__events.find(e=>e.event==='test:hello').data.n") == std::optional<std::string>{"42"});
    WT_CHECK(pages.js("dashboard", "window.__count('test:hello')") == std::optional<std::string>{"0"});
    pages.js("main", "window.__call(2,'Window.emit',{target:'*',event:'window:closed',payload:null});1");
    WT_CHECK(contains(response(*pages.webview("main"), 2), "reserved"));

    step("SenderScope routing: Window.current / Boson.* act on the calling window");
    pages.js("dashboard", "window.__call(3,'Window.current',{});1");
    WT_CHECK(contains(response(*pages.webview("dashboard"), 3), "\\\"label\\\":\\\"dashboard\\\""));
    pages.js("dashboard", "window.__call(4,'Boson.set_title',{title:'set from dashboard'});1");
    WT_CHECK(contains(response(*pages.webview("dashboard"), 4), "success"));
    pump_for(std::chrono::milliseconds{100});
    const auto title_of = [&](std::string_view label) {
        wchar_t buffer[256]{};
        GetWindowTextW(wm.find(label)->hwnd(), buffer, 256);
        return std::wstring(buffer);
    };
    WT_CHECK(title_of("dashboard") == L"set from dashboard");
    WT_CHECK(title_of("main") == L"Boson WM test - main");
    pages.js("workspace-1", "window.__call(5,'Window.set_bounds',{label:'dashboard',width:500,height:350});1");
    WT_CHECK(contains(response(*pages.webview("workspace-1"), 5), "\\\"width\\\":500"));

    step("Window.create: capability allow-list for labels and URLs");
    pages.js("main", "window.__call(6,'Window.create',{label:'evil',url:'popout.html'});1");
    WT_CHECK(contains(response(*pages.webview("main"), 6), "permission denied"));
    WT_CHECK(wm.find("evil") == nullptr);
    pages.js("main", "window.__call(7,'Window.create',{label:'popout-2',url:'https://example.com/'});1");
    WT_CHECK(contains(response(*pages.webview("main"), 7), "not allowed"));
    pages.js("main", "window.__call(8,'Window.create',{label:'popout-1',url:'popout.html',title:'Pop-out',width:360,height:240,focus:false,parent:'main'});1");
    WT_CHECK(contains(response(*pages.webview("main"), 8), "\\\"label\\\":\\\"popout-1\\\""));
    WT_CHECK(wm.find("popout-1") != nullptr && GetWindow(wm.find("popout-1")->hwnd(), GW_OWNER) == wm.find("main")->hwnd());
    WT_CHECK(pump_until([&]() { return pages.loaded("popout-1"); }, std::chrono::seconds{30}));
    WT_CHECK(pump_until([&]() { return pages.js("dashboard", "window.__events.some(e=>e.event==='window:created'&&e.data.label==='popout-1')") ==
                                      std::optional<std::string>{"true"}; }));
    pages.js("popout-1", "window.__call(9,'Window.list',{});1");
    const auto listed = response(*pages.webview("popout-1"), 9);
    WT_CHECK(contains(listed, "\\\"main\\\"") && contains(listed, "\\\"dashboard\\\"") && contains(listed, "\\\"workspace-1\\\"") &&
             contains(listed, "\\\"popout-1\\\""));
    WT_CHECK(env->webview_count() == 4);
    WT_CHECK(count_kind(env->process_infos(), boson::core::ProcessKind::Browser) == 1);

#ifdef BOSON_WINDOWS_SELFTEST_JS
    step("frontend windows.ts + Tauri / Electron / Wails window shims (bundled self-test in 'main')");
    {
        std::ifstream in(BOSON_WINDOWS_SELFTEST_JS, std::ios::binary);
        const std::string bundle((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        WT_CHECK(!bundle.empty());
        pages.js("main", bundle + "\n;1");
        std::optional<std::string> state;
        WT_CHECK(pump_until([&]() {
            state = pages.js("main", "window.__wst&&window.__wst.done?JSON.stringify(window.__wst):''");
            return state.has_value() && *state != "\"\"";
        }, std::chrono::seconds{60}));
        std::cout << "  shim self-test: " << state.value_or("?") << '\n';
        WT_CHECK(contains(state, "\\\"failed\\\":0"));
        WT_CHECK(contains(state, "\\\"passed\\\":22"));
        WT_CHECK(pump_until([&]() { return pages.js("dashboard", "window.__count('tauri:to')") == std::optional<std::string>{"2"}; }));
        WT_CHECK(title_of("dashboard") == L"wails title");
        WT_CHECK(wm.find("popout-js") == nullptr && wm.find("popout-tauri") == nullptr);
        WT_CHECK(wm.find("popout-electron") != nullptr && !IsWindowVisible(wm.find("popout-electron")->hwnd()));
        WT_CHECK(pump_until([&]() { return pages.loaded("popout-electron"); }, std::chrono::seconds{30}));
        pages.js("main", "window.__wstElectronSend();1");
        WT_CHECK(pump_until([&]() { return pages.js("popout-electron", "window.__count('ipc:chan')") == std::optional<std::string>{"1"}; }));
        WT_CHECK(pages.js("popout-electron", "JSON.stringify(window.__events.find(e=>e.event==='ipc:chan').data)") ==
                 std::optional<std::string>{"\"[1,2]\""});
        WT_CHECK(wm.destroy("popout-electron"));
    }
#else
    std::cout << "  (shim self-test not bundled: bun not found at configure time)\n";
#endif

    step("JS close guard: window:close-requested + Window.confirm_close");
    pages.js("workspace-1", "window.__autoAnswer=false;window.__call(10,'Window.set_close_guard',{label:'',enabled:true});1");
    WT_CHECK(contains(response(*pages.webview("workspace-1"), 10), "success"));
    WT_CHECK(wm.close("workspace-1"));
    WT_CHECK(pump_until([&]() { return pages.js("workspace-1", "window.__count('window:close-requested')") == std::optional<std::string>{"1"}; }));
    WT_CHECK(pump_until([&]() { return wm.find("workspace-1") != nullptr && !wm.find("workspace-1")->is_closing(); }, std::chrono::seconds{10}));
    pump_for(std::chrono::milliseconds{200});
    WT_CHECK(wm.find("workspace-1") != nullptr);
    pages.js("workspace-1", "window.__autoAnswer=true;1");
    WT_CHECK(wm.close("workspace-1"));
    WT_CHECK(pump_until([&]() { return wm.find("workspace-1") == nullptr; }, std::chrono::seconds{10}));
    WT_CHECK(pump_until([&]() { return pages.js("main", "window.__events.some(e=>e.event==='window:closed'&&e.data.label==='workspace-1')") ==
                                      std::optional<std::string>{"true"}; }));
    WT_CHECK(!bridge.is_attached(0));

    step("performance: emit_to(label), Window.current round trip");
    {
        pages.js("dashboard", "window.__events=[];1");
        constexpr int kEmits = 2000;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kEmits; ++i) {
            bridge.emit_to(std::string_view{"dashboard"}, "perf:x", boson::ipc::WindowLabelEvent{.label = "p"});
        }
        const double enqueue_us = ms_since(start) * 1000.0 / kEmits;
        const auto drain_start = std::chrono::steady_clock::now();
        marshal.drain_on_ui_thread();
        const double deliver_us = ms_since(drain_start) * 1000.0 / kEmits;
        WT_CHECK(pump_until([&]() { return pages.js("dashboard", "window.__count('perf:x')") == std::optional<std::string>{std::to_string(kEmits)}; },
                            std::chrono::seconds{20}));
        const double total_ms = ms_since(start);
        pages.js("dashboard",
                 "window.__rtt=undefined;(async()=>{for(let i=0;i<20;i++){await window.__rpc('Window.current',{});}"
                 "const t=performance.now();for(let i=0;i<200;i++){await window.__rpc('Window.current',{});}"
                 "window.__rtt=(performance.now()-t)/200;})();1");
        std::optional<std::string> rtt;
        WT_CHECK(pump_until([&]() {
            rtt = pages.js("dashboard", "window.__rtt===undefined?'':String(window.__rtt)");
            return rtt.has_value() && *rtt != "\"\"";
        }, std::chrono::seconds{30}));
        std::cout << std::fixed << std::setprecision(2) << "  [perf] emit_to(label): " << enqueue_us << " us/event to enqueue (serialize + post), "
                  << deliver_us << " us/event to route + PostWebMessageAsJson, " << kEmits << " events received by the page in "
                  << total_ms << " ms; Window.current round trip = " << rtt.value_or("?") << " ms\n";
    }

    step("teardown");
    wm.set_bridge(nullptr);
    wm.destroy_all();
    WT_CHECK(pump_until([&]() { return wm.windows().empty(); }, std::chrono::seconds{10}));
    bool shut = false;
    env->shutdown([&shut]() { shut = true; });
    pump_until([&]() { return shut; }, std::chrono::seconds{10});
    std::error_code ec;
    std::filesystem::remove_all(user_data, ec);
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    test_labels_and_urls();
    test_create_find_modal_owned();
    test_close_veto();
    test_quit_on_last_window();
    test_state_persistence();
    test_performance();
    test_webview_windows();
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
