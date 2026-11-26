// Native platform pieces: dialogs (driven by IFileDialogEvents / task-dialog messages, never left open),
// message boxes, permission policy + persistence, web notifications -> native presenter, zoom, browser
// argument helpers, process metrics, locale / system preferences, session end and close-to-tray.
// The WebView2 part skips when no runtime is installed. Temp files live in a per-run folder that is removed.
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shobjidl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boson/boson.hpp>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_native.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define NT_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

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

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// ---- Dialog automation through a window timer (fires inside the dialog's modal loop) ------------------------

struct Automation {
    std::wstring title;                  // dialog caption to look for
    std::function<void(HWND)> action;    // run once on the dialog
    std::function<void()> before_action; // checks made while the dialog is open
    bool done = false;
};
Automation g_auto;

BOOL CALLBACK find_dialog(HWND hwnd, LPARAM l_param) {
    wchar_t cls[64]{};
    wchar_t text[256]{};
    GetClassNameW(hwnd, cls, 64);
    GetWindowTextW(hwnd, text, 256);
    if (std::wstring_view{cls} == L"#32770" && g_auto.title == text && IsWindowVisible(hwnd) != FALSE) {
        *reinterpret_cast<HWND*>(l_param) = hwnd;
        return FALSE;
    }
    return TRUE;
}

void CALLBACK automation_timer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    if (g_auto.done || !g_auto.action) {
        return;
    }
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), &find_dialog, reinterpret_cast<LPARAM>(&dialog));
    if (dialog == nullptr) {
        return;
    }
    g_auto.done = true;
    KillTimer(hwnd, id);
    if (g_auto.before_action) {
        g_auto.before_action();
    }
    g_auto.action(dialog);
}

void arm_automation(HWND timer_owner, std::wstring title, std::function<void(HWND)> action, std::function<void()> before = {}) {
    g_auto = Automation{.title = std::move(title), .action = std::move(action), .before_action = std::move(before), .done = false};
    SetTimer(timer_owner, 0xB050, 40, &automation_timer);
}

std::filesystem::path make_temp_root() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    auto root = std::filesystem::path(temp) / (L"boson-native-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    return root;
}

void touch(const std::filesystem::path& path) {
    std::ofstream(path, std::ios::binary) << "x";
}

constexpr std::string_view kPage = R"HTML(<!doctype html><html><head><title>Native</title></head><body>
<script>
window.__log = [];
window.__perm = null;
function reqPerm() { window.__perm = null; Notification.requestPermission().then(p => { window.__perm = p; }); return 0; }
function notify(tag) {
  const n = new Notification('Hello', { body: 'World', tag: tag });
  n.onshow = () => __log.push('show:' + tag);
  n.onclick = () => __log.push('click:' + tag);
  n.onclose = () => __log.push('close:' + tag);
  n.onerror = () => __log.push('error:' + tag);
  window['__n_' + tag] = n;
  return 0;
}
</script></body></html>)HTML";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/index.html") {
        return std::span<const std::byte>{reinterpret_cast<const std::byte*>(kPage.data()), kPage.size()};
    }
    return std::nullopt;
}

// ---- Headless unit checks -----------------------------------------------------------------------------------

void unit_checks(const std::filesystem::path& root) {
    namespace bn = boson::native;

    step("file filters");
    NT_CHECK(bn::filter_spec(bn::FileFilter{.name = "Text", .extensions = {"txt", ".md", "*.log"}}) == L"*.txt;*.md;*.log");
    NT_CHECK(bn::filter_spec(bn::FileFilter{.name = "All", .extensions = {"*"}}) == L"*.*");
    NT_CHECK(bn::filter_spec(bn::FileFilter{.name = "Empty", .extensions = {}}) == L"*.*");

    step("message box helpers");
    NT_CHECK(bn::resolve_cancel_id(bn::MessageBoxOptions{.buttons = {"Save", "Don't save", "Cancel"}}) == 2);
    NT_CHECK(bn::resolve_cancel_id(bn::MessageBoxOptions{.buttons = {"&Yes", "&No"}}) == 1);
    NT_CHECK(bn::resolve_cancel_id(bn::MessageBoxOptions{.buttons = {"A", "B"}}) == 0);
    NT_CHECK(bn::resolve_cancel_id(bn::MessageBoxOptions{.buttons = {"A", "B"}, .cancel_id = 1}) == 1);
    NT_CHECK(bn::parse_message_box_icon("question") == bn::MessageBoxIcon::Question);
    NT_CHECK(!bn::parse_message_box_icon("bogus").has_value());
    NT_CHECK(bn::task_dialog_available());

    step("browser argument helpers");
    {
        boson::core::EnvironmentConfig cfg;
        cfg.additional_browser_arguments = {L"--js-flags=--expose-gc", L"--max-active-webgl-contexts=4"};
        bn::browser_args::set_max_active_webgl_contexts(cfg, 32);
        bn::browser_args::add_js_flags(cfg, {L"--max-old-space-size=4096", L"--expose-gc"});
        bn::browser_args::disable_features(cfg, {L"msSmartScreenProtection", L"msSmartScreenProtection"});
        bn::browser_args::set_autoplay_policy(cfg, bn::browser_args::AutoplayPolicy::NoUserGestureRequired);
        bn::browser_args::ignore_gpu_blocklist(cfg);
        NT_CHECK(bn::browser_args::find_switch(cfg, L"max-active-webgl-contexts") == std::optional<std::wstring>{L"32"});
        NT_CHECK(bn::browser_args::js_flags(cfg) == L"--expose-gc --max-old-space-size=4096");
        NT_CHECK(bn::browser_args::find_switch(cfg, L"ignore-gpu-blocklist") == std::optional<std::wstring>{L""});
        NT_CHECK(!bn::browser_args::find_switch(cfg, L"max-active-webgl").has_value());
        NT_CHECK(cfg.disable_features.size() == 1);
        const std::wstring args = boson::core::build_browser_arguments(cfg);
        std::wcout << L"  browser arguments: " << args << L'\n';
        NT_CHECK(args.find(L"--max-active-webgl-contexts=32") != std::wstring::npos);
        NT_CHECK(args.find(L"--max-active-webgl-contexts=4") == std::wstring::npos);
        NT_CHECK(args.find(L"--js-flags=\"--expose-gc --max-old-space-size=4096\"") != std::wstring::npos);
        NT_CHECK(args.find(L"--disable-features=msSmartScreenProtection") != std::wstring::npos);
        NT_CHECK(args.find(L"--autoplay-policy=no-user-gesture-required") != std::wstring::npos);
        NT_CHECK(bn::browser_args::remove_switch(cfg, L"js-flags") == 1);
        NT_CHECK(bn::browser_args::js_flags(cfg).empty());
    }

    step("zoom levels");
    NT_CHECK(bn::next_zoom_level(1.0, 1) == 1.1);
    NT_CHECK(bn::next_zoom_level(1.0, -1) == 0.9);
    NT_CHECK(bn::next_zoom_level(5.0, 1) == 5.0);
    NT_CHECK(bn::next_zoom_level(0.25, -1) == 0.25);
    NT_CHECK(bn::next_zoom_level(1.05, 1) == 1.1);
    NT_CHECK(std::abs(bn::zoom_level_to_factor(1.0) - 1.2) < 1e-9);
    NT_CHECK(std::abs(bn::zoom_factor_to_level(1.44) - 2.0) < 1e-9);

    step("permission policy + persistence");
    {
        const auto file = root / L"permissions.json";
        using K = boson::core::PermissionKind;
        using D = bn::PermissionDecision;
        NT_CHECK(bn::parse_permission_kind("clipboard_read") == K::ClipboardRead);
        NT_CHECK(bn::to_string(K::PersistentStorage) == "persistent_storage");
        NT_CHECK(bn::parse_permission_decision("granted") == D::Allow);
        {
            bn::PermissionManager pm(file);
            int changes = 0;
            pm.set_change_callback([&changes]() { ++changes; });
            NT_CHECK(pm.resolve("https://a.example/x", K::Camera) == D::Ask);
            pm.set_fallback(D::Default);
            pm.set_default(K::ClipboardRead, D::Deny);
            NT_CHECK(pm.resolve("https://a.example/x", K::Camera) == D::Default);
            NT_CHECK(pm.resolve("https://a.example/x", K::ClipboardRead) == D::Deny);
            NT_CHECK(pm.set("HTTPS://A.Example:443/path?q", K::Camera, D::Allow));
            NT_CHECK(pm.set("https://b.example", K::Microphone, D::Deny));
            NT_CHECK(pm.set("*", K::Geolocation, D::Deny));
            NT_CHECK(!pm.set("about:blank", K::Camera, D::Allow));
            NT_CHECK(pm.resolve("https://a.example:443/other", K::Camera) == D::Allow);
            NT_CHECK(pm.resolve("https://c.example", K::Geolocation) == D::Deny);
            NT_CHECK(pm.rules().size() == 3);
            NT_CHECK(changes == 3);
            NT_CHECK(pm.reset("https://b.example/", K::Microphone));
            NT_CHECK(pm.rules().size() == 2);
            NT_CHECK(pm.set("https://b.example", K::Microphone, D::Ask)); // Ask forgets
            NT_CHECK(pm.rules().size() == 2);
        }
        {
            bn::PermissionManager reloaded(file);
            NT_CHECK(reloaded.rules().size() == 2);
            NT_CHECK(reloaded.rule("https://a.example:443", K::Camera) == std::optional<D>{D::Allow});
            NT_CHECK(reloaded.clear("https://a.example:443"));
            NT_CHECK(reloaded.rules().size() == 1);
            NT_CHECK(reloaded.clear());
            NT_CHECK(reloaded.rules().empty());
        }
        std::ofstream(file, std::ios::binary | std::ios::trunc) << "{ not json";
        bn::PermissionManager corrupt(file);
        NT_CHECK(corrupt.rules().empty());
        NT_CHECK(corrupt.set("https://d.example", K::Camera, D::Allow));
        bn::PermissionManager after(file);
        NT_CHECK(after.rules().size() == 1);
    }

    step("permission controller (Router, headless methods)");
    {
        bn::PermissionManager pm;
        boson::exec::UiMarshal marshal([]() {});
        boson::ipc::PermissionController ctl(pm, marshal);
        boson::ipc::Router<boson::ipc::PermissionController> router(ctl);
        const std::string set = router.dispatch_sync(
            R"({"id":1,"method":"Permissions.set","params":{"origin":"https://x.example","kind":"camera","decision":"allow"}})");
        NT_CHECK(contains(set, "\"ok\":true"));
        const std::string query = router.dispatch_sync(
            R"({"id":2,"method":"Permissions.query","params":{"origin":"https://x.example/a","kind":"camera"}})");
        NT_CHECK(contains(query, "\"decision\":\"allow\"") && contains(query, "\"remembered\":true"));
        const std::string list = router.dispatch_sync(R"({"id":3,"method":"Permissions.list"})");
        NT_CHECK(contains(list, "https://x.example"));
        const std::string bad = router.dispatch_sync(
            R"({"id":4,"method":"Permissions.set","params":{"origin":"https://x.example","kind":"teleport","decision":"allow"}})");
        NT_CHECK(contains(bad, "\"error\"") && contains(bad, "Unknown permission kind"));
    }

    step("process metrics");
    {
        bn::MetricsSampler sampler;
        const auto first = sampler.sample({});
        NT_CHECK(first.processes.size() == 1);
        NT_CHECK(first.processes[0].type == "app");
        NT_CHECK(first.processes[0].working_set_bytes > 0);
        NT_CHECK(first.processes[0].private_bytes > 0);
        NT_CHECK(first.logical_processors >= 1);
        const auto spin_until = std::chrono::steady_clock::now() + std::chrono::milliseconds{150};
        volatile std::uint64_t sink = 0;
        while (std::chrono::steady_clock::now() < spin_until) {
            sink = sink + 1;
        }
        const auto second = sampler.sample({});
        NT_CHECK(second.processes[0].cpu_percent > 0.0);
        NT_CHECK(second.processes[0].cpu_time_ms >= first.processes[0].cpu_time_ms);
        const boson::core::ProcessInfo bogus{.pid = 0x7FFFFFF0U, .kind = boson::core::ProcessKind::Renderer};
        const auto with_bogus = sampler.sample(std::span<const boson::core::ProcessInfo>{&bogus, 1});
        NT_CHECK(with_bogus.processes.size() == 2 && !with_bogus.processes[1].accessible);
        std::cout << "  self: ws=" << second.processes[0].working_set_bytes / 1024 << " KiB, private="
                  << second.processes[0].private_bytes / 1024 << " KiB, cpu=" << second.processes[0].cpu_percent << "%\n";
    }

    step("locale + system preferences");
    {
        const auto locale = bn::query_locale();
        std::cout << "  locale: user=" << locale.user_locale << " system=" << locale.system_locale
                  << " ui=" << locale.ui_language << " region=" << locale.region << " languages=" << locale.preferred_languages.size() << '\n';
        NT_CHECK(!locale.user_locale.empty());
        NT_CHECK(!locale.preferred_languages.empty());
        NT_CHECK(!locale.decimal_separator.empty());
        const auto prefs = bn::query_system_preferences();
        std::cout << "  prefs: dark=" << prefs.dark_mode << " high_contrast=" << prefs.high_contrast << " reduced_motion="
                  << prefs.reduced_motion << " accent=" << prefs.accent_color << " text_scale=" << prefs.text_scale_factor << '\n';
        NT_CHECK(prefs.accent_color.size() == 7 && prefs.accent_color[0] == '#');
        NT_CHECK(prefs.text_scale_factor >= 1.0);
        NT_CHECK(prefs.double_click_ms > 0);
    }

    step("hot-path timings");
    {
        using clock = std::chrono::steady_clock;
        bn::PermissionManager pm{std::filesystem::path{}};
        for (int i = 0; i < 64; ++i) {
            pm.set("https://host" + std::to_string(i) + ".example", boson::core::PermissionKind::Camera, bn::PermissionDecision::Allow);
        }
        constexpr int kResolves = 20000;
        int allowed = 0;
        auto t0 = clock::now();
        for (int i = 0; i < kResolves; ++i) {
            allowed += pm.resolve("https://HOST" + std::to_string(i % 64) + ".example/", boson::core::PermissionKind::Camera) ==
                               bn::PermissionDecision::Allow
                           ? 1
                           : 0;
        }
        const double resolve_ns = std::chrono::duration<double, std::nano>(clock::now() - t0).count() / kResolves;
        NT_CHECK(allowed == kResolves);
        constexpr int kQueries = 200;
        t0 = clock::now();
        std::size_t sink = 0;
        for (int i = 0; i < kQueries; ++i) {
            sink += bn::query_system_preferences().accent_color.size();
        }
        const double prefs_us = std::chrono::duration<double, std::micro>(clock::now() - t0).count() / kQueries;
        t0 = clock::now();
        for (int i = 0; i < kQueries; ++i) {
            sink += bn::query_locale().preferred_languages.size();
        }
        const double locale_us = std::chrono::duration<double, std::micro>(clock::now() - t0).count() / kQueries;
        NT_CHECK(sink > 0);
        std::cout << "  perf: permission resolve " << resolve_ns << " ns, query_system_preferences " << prefs_us
                  << " us, query_locale " << locale_us << " us\n";
    }
}

// ---- Window-level checks (no WebView) -----------------------------------------------------------------------

void window_checks(boson::core::Window& window, boson::exec::UiMarshal& marshal, boson::exec::TaskRunner& runner,
                   const std::filesystem::path& root) {
    namespace bn = boson::native;
    HWND owner = window.hwnd();

    step("message box: buttons, checkbox, default/cancel (TDM automation)");
    {
        bn::MessageBoxOptions options;
        options.title = "Boson native test";
        options.message = "Save changes?";
        options.detail = "Automated: never visible for long.";
        options.icon = bn::MessageBoxIcon::Warning;
        options.buttons = {"Save", "Don't save", "Cancel"};
        options.default_id = 0;
        options.checkbox_label = "Don't ask again";
        options.on_shown = [](HWND dialog) {
            PostMessageW(dialog, TDM_CLICK_VERIFICATION, TRUE, FALSE);
            PostMessageW(dialog, TDM_CLICK_BUTTON, bn::kMessageBoxButtonBaseId + 1, 0);
        };
        const auto picked = bn::show_message_box(owner, options);
        NT_CHECK(picked.response == 1);
        NT_CHECK(picked.checkbox_checked);
        NT_CHECK(!picked.cancelled);
        NT_CHECK(!picked.used_fallback);

        options.on_shown = [](HWND dialog) { PostMessageW(dialog, TDM_CLICK_BUTTON, IDCANCEL, 0); };
        options.checkbox_checked = true;
        const auto cancelled = bn::show_message_box(owner, options);
        NT_CHECK(cancelled.cancelled);
        NT_CHECK(cancelled.response == 2);
        NT_CHECK(cancelled.checkbox_checked);

        options.on_shown = [](HWND dialog) { PostMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0); PostMessageW(dialog, TDM_CLICK_BUTTON, bn::kMessageBoxButtonBaseId, 0); };
        std::optional<bn::MessageBoxResult> async_result;
        bn::show_message_box_async(owner, options, [&async_result](bn::MessageBoxResult r) { async_result = r; });
        NT_CHECK(!async_result.has_value()); // deferred to its own dispatch
        NT_CHECK(pump_until([&]() { return async_result.has_value(); }, std::chrono::seconds{10}));
        NT_CHECK(async_result.has_value() && async_result->response == 0);
    }

    step("file dialogs: open / multi / save / folder / cancel (IFileDialogEvents automation)");
    {
        const auto dir = root / L"pick";
        std::filesystem::create_directories(dir);
        touch(dir / L"a.txt");
        touch(dir / L"b.md");
        const auto ok_with = [](std::wstring name) {
            return [name](IFileDialog& dialog, HWND hwnd) {
                if (!name.empty()) {
                    dialog.SetFileName(name.c_str());
                }
                PostMessageW(hwnd, WM_COMMAND, IDOK, 0);
            };
        };

        bn::FileDialogOptions open;
        open.title = "Boson open test";
        open.default_path = dir.string();
        open.filters = {{.name = "Text", .extensions = {"txt"}}, {.name = "All files", .extensions = {"*"}}};
        open.on_shown = ok_with(L"a.txt");
        const auto one = bn::open_file_dialog(owner, open);
        NT_CHECK(!one.cancelled && one.paths.size() == 1);
        NT_CHECK(!one.paths.empty() && std::filesystem::path(one.paths[0]) == dir / L"a.txt");
        NT_CHECK(one.filter_index == 0);

        open.allow_multiple = true;
        open.filter_index = 1;
        open.on_shown = ok_with(L"\"a.txt\" \"b.md\"");
        const auto many = bn::open_file_dialog(owner, open);
        NT_CHECK(many.paths.size() == 2);
        NT_CHECK(many.filter_index == 1);

        bn::FileDialogOptions save;
        save.title = "Boson save test";
        save.default_path = (dir / L"draft.txt").string();
        save.filters = {{.name = "Text", .extensions = {"txt"}}};
        save.on_shown = ok_with(L"noext");
        const auto saved = bn::save_file_dialog(owner, save);
        NT_CHECK(!saved.cancelled && saved.paths.size() == 1);
        NT_CHECK(!saved.paths.empty() && std::filesystem::path(saved.paths[0]) == dir / L"noext.txt");

        bn::FileDialogOptions folder;
        folder.title = "Boson folder test";
        folder.default_path = dir.string();
        folder.on_shown = ok_with(L"");
        const auto picked_folder = bn::pick_folder_dialog(owner, folder);
        NT_CHECK(!picked_folder.cancelled && picked_folder.paths.size() == 1);
        NT_CHECK(!picked_folder.paths.empty() && std::filesystem::path(picked_folder.paths[0]) == dir);

        open.on_shown = [](IFileDialog&, HWND hwnd) { PostMessageW(hwnd, WM_COMMAND, IDCANCEL, 0); };
        const auto cancelled = bn::open_file_dialog(owner, open);
        NT_CHECK(cancelled.cancelled && cancelled.paths.empty() && SUCCEEDED(cancelled.hr));

        std::optional<bn::FileDialogResult> async_result;
        open.allow_multiple = false;
        open.filter_index = 0;
        open.on_shown = ok_with(L"a.txt");
        bn::show_file_dialog_async(bn::FileDialogKind::Open, owner, open, [&](bn::FileDialogResult r) { async_result = std::move(r); });
        NT_CHECK(pump_until([&]() { return async_result.has_value(); }, std::chrono::seconds{15}));
        NT_CHECK(async_result.has_value() && async_result->paths.size() == 1);
    }

    step("Dialog controller via Router + TaskRunner (pool thread waits, UI keeps pumping)");
    {
        boson::ipc::DialogController ctl(window, marshal);
        boson::ipc::Router<boson::ipc::DialogController> router(ctl);
        std::optional<std::string> reply;
        arm_automation(owner, L"Boson IPC box", [](HWND dialog) {
            PostMessageW(dialog, TDM_CLICK_VERIFICATION, TRUE, FALSE);
            PostMessageW(dialog, TDM_CLICK_BUTTON, boson::native::kMessageBoxButtonBaseId + 1, 0);
        });
        router.dispatch_async(
            R"({"id":7,"method":"Dialog.message_box","params":{"title":"Boson IPC box","message":"Proceed?","type":"question","buttons":["Yes","No"],"checkbox_label":"Remember"}})",
            runner, [&reply](std::string r) { reply = std::move(r); });
        NT_CHECK(pump_until([&]() { return reply.has_value(); }, std::chrono::seconds{20}));
        NT_CHECK(reply.has_value() && contains(*reply, "\"response\":1") && contains(*reply, "\"checkbox_checked\":true"));

        // A dialog launched from a UiMarshal batch must not hold back the rest of that batch.
        bool side_task_ran = false;
        bool side_task_seen_while_open = false;
        bool dialog_closed = false;
        arm_automation(owner, L"Boson batch box", [](HWND dialog) { PostMessageW(dialog, TDM_CLICK_BUTTON, IDCANCEL, 0); },
                       [&]() { side_task_seen_while_open = side_task_ran; });
        marshal.post_task([&]() {
            boson::native::MessageBoxOptions o;
            o.title = "Boson batch box";
            o.message = "Batch";
            boson::native::show_message_box_async(owner, o, [&dialog_closed](boson::native::MessageBoxResult) { dialog_closed = true; });
        });
        marshal.post_task([&side_task_ran]() { side_task_ran = true; });
        NT_CHECK(pump_until([&]() { return dialog_closed; }, std::chrono::seconds{20}));
        NT_CHECK(side_task_seen_while_open);

        reply.reset();
        arm_automation(owner, L"Boson IPC open", [](HWND dialog) { PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0); });
        router.dispatch_async(
            R"({"id":8,"method":"Dialog.open","params":{"title":"Boson IPC open","filters":[{"name":"Images","extensions":["png","jpg"]}],"multiple":true}})",
            runner, [&reply](std::string r) { reply = std::move(r); });
        NT_CHECK(pump_until([&]() { return reply.has_value(); }, std::chrono::seconds{20}));
        NT_CHECK(reply.has_value() && contains(*reply, "\"cancelled\":true"));

        const std::string bad = router.dispatch_sync(R"({"id":9,"method":"Dialog.message_box","params":{"type":"sparkly"}})");
        NT_CHECK(contains(bad, "Unsupported message box type"));
    }

    step("session end: query / block reason / end");
    {
        bn::SessionEndWatcher watcher(window);
        int queries = 0;
        std::optional<bool> ended;
        watcher.set_query_callback([&queries](bn::SessionEndWatcher::Query&) { ++queries; });
        watcher.set_end_callback([&ended](bool ending, const bn::SessionEndInfo&) { ended = ending; });
        NT_CHECK(SendMessageW(owner, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF) == TRUE);
        NT_CHECK(watcher.set_block_reason("Unsaved changes in 2 files"));
        std::array<wchar_t, 256> reason{};
        DWORD len = static_cast<DWORD>(reason.size());
        NT_CHECK(ShutdownBlockReasonQuery(owner, reason.data(), &len) != FALSE);
        NT_CHECK(std::wstring_view{reason.data()} == L"Unsaved changes in 2 files");
        NT_CHECK(SendMessageW(owner, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF) == FALSE);
        NT_CHECK(SendMessageW(owner, WM_QUERYENDSESSION, 0, static_cast<LPARAM>(ENDSESSION_CRITICAL)) == TRUE);
        watcher.set_query_callback([&queries](bn::SessionEndWatcher::Query& q) { ++queries; q.allow = true; });
        NT_CHECK(SendMessageW(owner, WM_QUERYENDSESSION, 0, 0) == TRUE);
        watcher.clear_block_reason();
        len = static_cast<DWORD>(reason.size());
        NT_CHECK(ShutdownBlockReasonQuery(owner, reason.data(), &len) == FALSE);
        NT_CHECK(queries == 4);
        SendMessageW(owner, WM_ENDSESSION, FALSE, 0);
        NT_CHECK(ended == std::optional<bool>{false});
        NT_CHECK(!watcher.session_ending());
    }

    step("close to tray");
    {
        boson::core::WindowConfig cfg;
        cfg.title = L"Boson close-to-tray test";
        cfg.width = 320;
        cfg.height = 200;
        cfg.quit_on_destroy = false;
        boson::core::Window tray_window(cfg);
        NT_CHECK(tray_window.create(nullptr, SW_SHOWNOACTIVATE));
        bn::TrayIcon tray(tray_window.hwnd());
        int shell_calls = 0;
        tray.set_shell_function([&shell_calls](DWORD, NOTIFYICONDATAW*) { ++shell_calls; return TRUE; });
        bn::CloseToTray ctt(tray_window, tray);
        std::vector<bool> hidden_events;
        ctt.set_hidden_callback([&hidden_events](bool hidden) { hidden_events.push_back(hidden); });
        ctt.install_tray_callback();
        NT_CHECK(tray.is_visible());
        PostMessageW(tray_window.hwnd(), WM_CLOSE, 0, 0);
        NT_CHECK(pump_until([&]() { return ctt.hidden(); }, std::chrono::seconds{5}));
        NT_CHECK(IsWindow(tray_window.hwnd()) != FALSE);
        NT_CHECK(!tray_window.is_visible());
        NT_CHECK(ctt.handle_tray_event(bn::TrayEvent{.kind = bn::TrayEventKind::DoubleClick, .position = {}}));
        NT_CHECK(tray_window.is_visible() && !ctt.hidden());
        NT_CHECK(!ctt.handle_tray_event(bn::TrayEvent{.kind = bn::TrayEventKind::RightClick, .position = {}}));
        NT_CHECK((hidden_events == std::vector<bool>{true, false}));
        HWND hwnd = tray_window.hwnd();
        ctt.quit();
        NT_CHECK(pump_until([&]() { return IsWindow(hwnd) == FALSE; }, std::chrono::seconds{5}));
        NT_CHECK(shell_calls > 0);
    }

    step("system preferences watcher (no change -> no callback)");
    {
        bn::SystemPreferencesWatcher watcher(window);
        int calls = 0;
        watcher.set_preferences_callback([&calls](const bn::SystemPreferences&) { ++calls; });
        watcher.set_locale_callback([&calls](const bn::LocaleInfo&) { ++calls; });
        SendMessageW(owner, WM_SETTINGCHANGE, 0, 0);
        NT_CHECK(calls == 0);
        NT_CHECK(!watcher.locale().user_locale.empty());
    }
}

// ---- WebView2 runtime checks --------------------------------------------------------------------------------

void runtime_checks(boson::core::Window& window, boson::exec::UiMarshal& marshal, boson::exec::TaskRunner& runner,
                    const std::filesystem::path& root) {
    namespace bn = boson::native;
    using K = boson::core::PermissionKind;
    using D = bn::PermissionDecision;

    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = (root / L"udf").wstring();
    bn::browser_args::add_js_flags(env_cfg, {L"--expose-gc"});
    bn::browser_args::set_max_active_webgl_contexts(env_cfg, 32);
    const auto env = boson::core::Environment::create(env_cfg);
    auto host = std::make_unique<boson::core::WebViewHost>(window, env);
    host->set_default_asset_lookup(&lookup);

    bn::PermissionManager permissions(root / L"perm-runtime.json");
    bn::WebNotificationHost notifications(marshal);
    std::vector<bn::WebNotification> presented;
    notifications.set_presenter([&presented](const bn::WebNotification& n) { presented.push_back(n); return true; });
    std::vector<std::string> host_events;
    notifications.set_event_callback([&host_events](const bn::WebNotification& n, bn::WebNotificationEvent e) {
        host_events.push_back(std::string(bn::to_string(e)) + ":" + n.tag);
    });

    bool navigated = false;
    boson::core::WebViewConfig cfg;
    cfg.initial_url = "boson://app/index.html";
    boson::core::WebView& wv = host->create(cfg);
    wv.events().on_navigation_completed = [&navigated](const boson::core::NavigationCompletedEvent& e) { navigated = e.is_success; };
    permissions.attach(wv);
    notifications.attach(wv, window.hwnd());
    std::vector<double> zoom_events;
    wv.events().on_zoom_factor_changed = [&zoom_events](double f) { zoom_events.push_back(f); };
    host->set_layout([&wv](boson::core::WebViewHost&, const boson::core::LayoutContext& ctx) {
        wv.set_bounds(RECT{0, 0, ctx.width(), ctx.height()});
    });

    step("runtime: WebView ready");
    NT_CHECK(pump_until([&]() { return wv.is_ready() && navigated; }, std::chrono::seconds{40}));
    NT_CHECK(pump_until([&]() { return wv.document_title() == std::optional<std::string>{"Native"}; }, std::chrono::seconds{10}));

    step("runtime: browser args reach the browser process (--js-flags=--expose-gc)");
    NT_CHECK(env->browser_arguments().find(L"--max-active-webgl-contexts=32") != std::wstring::npos);
    NT_CHECK(eval(wv, "typeof gc") == std::optional<std::string>{"\"function\""});

    step("runtime: zoom factor + ZoomFactorChanged");
    {
        // put_ZoomFactor does not raise ZoomFactorChanged (runtime behaviour); a user Ctrl+wheel does.
        NT_CHECK(wv.set_zoom_factor(1.5));
        NT_CHECK(std::abs(wv.zoom_factor().value_or(0.0) - 1.5) < 1e-6);
        HWND render = nullptr;
        EnumChildWindows(window.hwnd(), [](HWND child, LPARAM out) -> BOOL {
            wchar_t cls[64]{};
            GetClassNameW(child, cls, 64);
            if (std::wstring_view{cls} == L"Chrome_RenderWidgetHostHWND") {
                *reinterpret_cast<HWND*>(out) = child;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&render));
        if (render != nullptr) {
            RECT rc{};
            GetWindowRect(render, &rc);
            PostMessageW(render, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), MAKELPARAM((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2));
            const bool fired = pump_until([&]() { return !zoom_events.empty(); }, std::chrono::seconds{3});
            std::cout << "  user-style Ctrl+wheel zoom event: " << (fired ? "raised" : "not raised (synthetic input ignored)") << '\n';
            if (fired) {
                NT_CHECK(zoom_events.back() > 1.5);
            }
        }
        boson::ipc::ZoomController zoom(wv, marshal);
        boson::ipc::Router<boson::ipc::ZoomController> router(zoom);
        NT_CHECK(wv.set_zoom_factor(1.0));
        pump_for(std::chrono::milliseconds{300});
        const double before_zoom_in = wv.zoom_factor().value_or(1.0);
        std::optional<std::string> reply;
        router.dispatch_async(R"({"id":1,"method":"Zoom.zoom_in"})", runner, [&reply](std::string r) { reply = std::move(r); });
        NT_CHECK(pump_until([&]() { return reply.has_value(); }, std::chrono::seconds{10}));
        NT_CHECK(reply.has_value() && contains(*reply, "\"factor\":"));
        const double after_zoom_in = wv.zoom_factor().value_or(0.0);
        std::cout << "  Zoom.zoom_in: " << before_zoom_in << " -> " << after_zoom_in << '\n';
        NT_CHECK(std::abs(after_zoom_in - boson::native::next_zoom_level(before_zoom_in, 1)) < 1e-6);
        NT_CHECK(wv.set_zoom_factor(1.0));
        pump_for(std::chrono::milliseconds{100});
    }

    step("runtime: permission prompt hook (Ask -> prompt -> deny, no remember)");
    std::vector<bn::PermissionPrompt> prompts;
    permissions.set_prompt_handler([&prompts](const bn::PermissionPrompt& p) { prompts.push_back(p); });
    {
        NT_CHECK(eval(wv, "reqPerm()").has_value());
        NT_CHECK(pump_until([&]() { return !prompts.empty(); }, std::chrono::seconds{10}));
        NT_CHECK(!prompts.empty() && prompts[0].kind == K::Notifications && prompts[0].origin == "boson://app");
        NT_CHECK(permissions.pending().size() == 1);
        if (!prompts.empty()) {
            NT_CHECK(permissions.answer(prompts[0].id, D::Deny, false));
        }
        NT_CHECK(pump_until([&]() { return eval(wv, "window.__perm") == std::optional<std::string>{"\"denied\""}; }, std::chrono::seconds{10}));
        NT_CHECK(permissions.rules().empty());
        NT_CHECK(!permissions.answer(9999, D::Allow, false));
    }

    step("runtime: remembered Allow rule answers without a prompt");
    {
        NT_CHECK(permissions.set("boson://app", K::Notifications, D::Allow));
        NT_CHECK(wv.reload());
        navigated = false;
        NT_CHECK(pump_until([&]() { return navigated; }, std::chrono::seconds{10}));
        const std::size_t before = prompts.size();
        NT_CHECK(eval(wv, "reqPerm()").has_value());
        NT_CHECK(pump_until([&]() { return eval(wv, "window.__perm") == std::optional<std::string>{"\"granted\""}; }, std::chrono::seconds{10}));
        NT_CHECK(prompts.size() == before);
    }

    step("runtime: Web Notification -> native presenter, click and close round trip");
    {
        NT_CHECK(bn::WebNotificationHost::is_supported(wv));
        NT_CHECK(eval(wv, "notify('one')").has_value());
        NT_CHECK(pump_until([&]() { return !presented.empty(); }, std::chrono::seconds{10}));
        NT_CHECK(!presented.empty() && presented[0].title == "Hello" && presented[0].body == "World" && presented[0].tag == "one");
        NT_CHECK(!presented.empty() && presented[0].origin == "boson://app");
        NT_CHECK(pump_until([&]() { return contains(eval(wv, "__log.join(',')").value_or(""), "show:one"); }, std::chrono::seconds{5}));
        NT_CHECK(notifications.active_count() == 1);
        if (!presented.empty()) {
            NT_CHECK(notifications.click(presented[0].id));
        }
        NT_CHECK(pump_until([&]() { return contains(eval(wv, "__log.join(',')").value_or(""), "click:one"); }, std::chrono::seconds{5}));
        NT_CHECK(notifications.active_count() == 0);

        NT_CHECK(eval(wv, "notify('two')").has_value());
        NT_CHECK(pump_until([&]() { return notifications.active_count() == 1; }, std::chrono::seconds{10}));
        NT_CHECK(eval(wv, "__n_two.close(), 0").has_value());
        NT_CHECK(pump_until([&]() { return notifications.active_count() == 0; }, std::chrono::seconds{5}));

        NT_CHECK(eval(wv, "notify('three')").has_value());
        NT_CHECK(pump_until([&]() { return notifications.active_count() == 1; }, std::chrono::seconds{10}));
        NT_CHECK(eval(wv, "notify('three')").has_value()); // same tag replaces
        NT_CHECK(pump_until([&]() { return presented.size() == 4; }, std::chrono::seconds{10}));
        pump_for(std::chrono::milliseconds{200});
        NT_CHECK(notifications.active_count() == 1);
        std::string events;
        for (const auto& e : host_events) {
            events += e + ",";
        }
        std::cout << "  host events: " << events << '\n';
        NT_CHECK(contains(events, "shown:one,clicked:one,closed:one,"));
    }

    step("runtime: App metrics across the WebView2 process tree (Native.metrics via Router)");
    {
        const auto processes = bn::collect_webview_processes(std::span<const std::shared_ptr<boson::core::Environment>>{&env, 1});
        bool has_browser = false;
        bool has_renderer = false;
        for (const auto& p : processes) {
            has_browser = has_browser || p.kind == boson::core::ProcessKind::Browser;
            has_renderer = has_renderer || p.kind == boson::core::ProcessKind::Renderer;
        }
        NT_CHECK(has_browser && has_renderer);
        bn::MetricsSampler sampler;
        (void)sampler.sample(processes);
        constexpr int kRuns = 200;
        const auto t0 = std::chrono::steady_clock::now();
        bn::AppMetrics last;
        for (int i = 0; i < kRuns; ++i) {
            last = sampler.sample(processes);
        }
        const auto per_sample_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kRuns;
        NT_CHECK(last.processes.size() == processes.size() + 1);
        NT_CHECK(last.total_working_set_bytes > last.processes[0].working_set_bytes);
        std::cout << "  metrics: " << last.processes.size() << " processes, total ws=" << last.total_working_set_bytes / (1024 * 1024)
                  << " MiB, private=" << last.total_private_bytes / (1024 * 1024) << " MiB; sample() = " << per_sample_us << " us\n";

        boson::ipc::NativeController native_ctl(window, marshal, {env});
        boson::ipc::Router<boson::ipc::NativeController> router(native_ctl);
        std::optional<std::string> reply;
        router.dispatch_async(R"({"id":2,"method":"Native.metrics"})", runner, [&reply](std::string r) { reply = std::move(r); });
        NT_CHECK(pump_until([&]() { return reply.has_value(); }, std::chrono::seconds{10}));
        NT_CHECK(reply.has_value() && contains(*reply, "\"type\":\"browser\"") && contains(*reply, "\"type\":\"renderer\""));
        reply.reset();
        router.dispatch_async(R"({"id":3,"method":"Native.locale"})", runner, [&reply](std::string r) { reply = std::move(r); });
        NT_CHECK(pump_until([&]() { return reply.has_value(); }, std::chrono::seconds{10}));
        NT_CHECK(reply.has_value() && contains(*reply, "preferred_languages"));
        reply.reset();
        router.dispatch_async(R"({"id":4,"method":"Native.set_shutdown_block_reason","params":{"reason":"Unsaved"}})", runner,
                              [&reply](std::string r) { reply = std::move(r); });
        NT_CHECK(pump_until([&]() { return reply.has_value(); }, std::chrono::seconds{10}));
        NT_CHECK(reply.has_value() && contains(*reply, "\"ok\":true"));
        NT_CHECK(SendMessageW(window.hwnd(), WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF) == FALSE);
        native_ctl.session().clear_block_reason();
        reply.reset();
        router.dispatch_async(R"({"id":5,"method":"Native.hide_to_tray"})", runner, [&reply](std::string r) { reply = std::move(r); });
        NT_CHECK(pump_until([&]() { return reply.has_value(); }, std::chrono::seconds{10}));
        NT_CHECK(reply.has_value() && contains(*reply, "not configured"));
    }

    notifications.detach(wv);
    permissions.detach(wv);
    host.reset();
    bool shut = false;
    env->shutdown([&shut]() { shut = true; });
    pump_until([&]() { return shut; }, std::chrono::seconds{10});
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    const auto root = make_temp_root();
    unit_checks(root);
    std::cout << "unit checks: " << (g_checks - g_failures) << "/" << g_checks << " passed\n";

    {
        boson::core::WindowConfig window_cfg;
        window_cfg.title = L"Boson native test";
        window_cfg.width = 640;
        window_cfg.height = 480;
        window_cfg.quit_on_destroy = false;
        auto window = std::make_unique<boson::core::Window>(window_cfg);
        NT_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

        boson::exec::Scheduler scheduler(2);
        boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
        window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
        boson::exec::TaskRunner runner(scheduler, marshal);

        window_checks(*window, marshal, runner, root);

        LPWSTR version = nullptr;
        if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
            std::cout << "SKIP runtime checks: no WebView2 runtime installed\n";
        } else {
            std::wcout << L"WebView2 runtime " << version << L'\n';
            CoTaskMemFree(version);
            runtime_checks(*window, marshal, runner, root);
        }
        pump_for(std::chrono::milliseconds{100});
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    CoUninitialize();
    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
