// Shell integration tests: taskbar, jump lists, toasts + activation, power, auto-launch, controllers.
// Every registry value, shortcut and jump-list file created here uses a per-process unique name and is
// removed before exit. Windows are shown with SW_SHOWNOACTIVATE only.
#include <windows.h>
#include <objbase.h>
#include <shlobj_core.h>
#include <NotificationActivationCallback.h>
#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_shell_integration.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define SH_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{15}) {
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

/// Runs fn on a worker thread (like the router's pool thread) while the UI thread keeps pumping.
template <typename Fn>
auto off_ui(Fn&& fn) {
    using R = std::invoke_result_t<Fn>;
    std::optional<R> result;
    std::atomic<bool> done{false};
    std::thread worker([&]() {
        result.emplace(fn());
        done.store(true);
    });
    pump_until([&]() { return done.load(); }, std::chrono::seconds{30});
    worker.join();
    return std::move(*result);
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

std::wstring unique_suffix() {
    return std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64() % 100000);
}

bool xml_parses(const std::string& xml) {
    using Microsoft::WRL::ComPtr;
    using Microsoft::WRL::Wrappers::HStringReference;
    ComPtr<IInspectable> inspectable;
    if (FAILED(RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), &inspectable))) {
        return false;
    }
    ComPtr<ABI::Windows::Data::Xml::Dom::IXmlDocumentIO> io;
    if (FAILED(inspectable.As(&io))) {
        return false;
    }
    const std::wstring wide = boson::core::utf8_to_wide(xml);
    return SUCCEEDED(io->LoadXml(HStringReference(wide.c_str(), static_cast<unsigned int>(wide.size())).Get()));
}

std::set<std::filesystem::path> list_dir(const std::filesystem::path& dir) {
    std::set<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        out.insert(entry.path());
    }
    return out;
}

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw != nullptr) {
        out = raw;
    }
    CoTaskMemFree(raw);
    return out;
}

bool reg_value_exists(std::wstring_view key, std::wstring_view name) {
    const std::wstring k(key);
    const std::wstring n(name);
    return RegGetValueW(HKEY_CURRENT_USER, k.c_str(), n.c_str(), RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool reg_key_exists(const std::wstring& key) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(h);
    return true;
}

template <typename Router>
std::string rpc(Router& router, std::string_view method, std::string_view params = "null") {
    const std::string request = std::string(R"({"id":1,"method":")") + std::string(method) + R"(","params":)" + std::string(params) + "}";
    return off_ui([&]() { return router.dispatch_sync(request); });
}

// ---- headless unit tests -----------------------------------------------------------------------------

void test_pure_helpers() {
    step("pure helpers: AUMID, GUID, argv, colors, XML");
    SH_CHECK(boson::shell::is_valid_app_user_model_id(L"Company.Product"));
    SH_CHECK(!boson::shell::is_valid_app_user_model_id(L""));
    SH_CHECK(!boson::shell::is_valid_app_user_model_id(L"has space"));
    SH_CHECK(!boson::shell::is_valid_app_user_model_id(std::wstring(129, L'a')));

    GUID guid{};
    SH_CHECK(SUCCEEDED(CoCreateGuid(&guid)));
    const std::wstring text = boson::shell::guid_to_string(guid);
    SH_CHECK(text.size() == 38 && text.front() == L'{');
    SH_CHECK(boson::shell::parse_guid(text) == std::optional<GUID>{guid});
    SH_CHECK(boson::shell::parse_guid(text.substr(1, 36)) == std::optional<GUID>{guid});
    SH_CHECK(!boson::shell::parse_guid(L"not-a-guid").has_value());

    const std::vector<std::wstring> launched{L"--foo", L"-toastactivated"};
    const std::vector<std::wstring> normal{L"--foo"};
    SH_CHECK(boson::shell::was_launched_by_toast(launched));
    SH_CHECK(!boson::shell::was_launched_by_toast(normal));

    SH_CHECK(boson::ipc::shell_detail::parse_color("#102030", 0) == RGB(0x10, 0x20, 0x30));
    SH_CHECK(boson::ipc::shell_detail::parse_color("ffFFff", 0) == RGB(255, 255, 255));
    SH_CHECK(boson::ipc::shell_detail::parse_color("#12", 7) == 7);
    SH_CHECK(boson::ipc::shell_detail::parse_color("#zz0000", 7) == 7);

    SH_CHECK(boson::shell::xml_escape(R"(<a & "b" 'c'>)") == "&lt;a &amp; &quot;b&quot; &apos;c&apos;&gt;");
    SH_CHECK(boson::shell::xml_escape(std::string("x\x01y")) == "xy");
    SH_CHECK(boson::shell::to_image_uri("C:\\img dir\\a#1.png") == "file:///C:/img%20dir/a%231.png");
    SH_CHECK(boson::shell::to_image_uri("https://x/y.png") == "https://x/y.png");
    SH_CHECK(boson::shell::to_image_uri("\\\\server\\share\\a.png") == "file://server/share/a.png");

    boson::shell::ToastContent content;
    content.title = "Build <done>";
    content.body = "All & good";
    content.attribution = "via Boson";
    content.app_logo = "C:\\logo.png";
    content.app_logo_circle = true;
    content.hero_image = "C:\\hero.png";
    content.inline_image = "https://example.com/i.png";
    content.launch_arguments = "action=open&id=7";
    content.inputs.push_back({.id = "reply", .title = "", .placeholder = "Type a reply", .default_value = "", .options = {}});
    content.inputs.push_back({.id = "choice", .title = "Pick", .placeholder = "", .default_value = "b",
                              .options = {{.id = "a", .content = "A"}, {.id = "b", .content = "B"}}});
    content.buttons.push_back({.content = "Send", .arguments = "action=reply", .activation = boson::shell::ToastActivationType::Background,
                               .input_id = "reply", .image = "", .context_menu = false});
    content.buttons.push_back({.content = "Docs", .arguments = "https://example.com", .activation = boson::shell::ToastActivationType::Protocol,
                               .input_id = "", .image = "", .context_menu = true});
    content.progress = boson::shell::ToastProgress{.title = "Download", .value = 0.5, .value_label = "", .status = "Working"};
    content.scenario = boson::shell::ToastScenario::Reminder;
    content.duration = boson::shell::ToastDuration::Long;
    content.silent = true;
    const std::string xml = boson::shell::build_toast_xml(content);
    SH_CHECK(contains(xml, R"(launch="action=open&amp;id=7")"));
    SH_CHECK(contains(xml, R"(scenario="reminder")"));
    SH_CHECK(contains(xml, R"(duration="long")"));
    SH_CHECK(contains(xml, "<text>Build &lt;done&gt;</text>"));
    SH_CHECK(contains(xml, R"(placement="attribution")"));
    SH_CHECK(contains(xml, R"(placement="appLogoOverride" src="file:///C:/logo.png" hint-crop="circle")"));
    SH_CHECK(contains(xml, R"(placement="hero")"));
    SH_CHECK(contains(xml, R"(value="{progressValue}")"));
    SH_CHECK(contains(xml, R"(<input id="reply" type="text" placeHolderContent="Type a reply"/>)"));
    SH_CHECK(contains(xml, R"(type="selection")"));
    SH_CHECK(contains(xml, R"(hint-inputId="reply")"));
    SH_CHECK(contains(xml, R"(activationType="background")"));
    SH_CHECK(contains(xml, R"(placement="contextMenu")"));
    SH_CHECK(contains(xml, R"(<audio silent="true"/>)"));
    SH_CHECK(xml_parses(xml));

    boson::shell::ToastContent minimal;
    minimal.title = "Hi";
    minimal.sound = "ms-winsoundevent:Notification.Reminder";
    minimal.loop_sound = true;
    const std::string minimal_xml = boson::shell::build_toast_xml(minimal);
    SH_CHECK(contains(minimal_xml, R"(<audio src="ms-winsoundevent:Notification.Reminder" loop="true"/>)"));
    SH_CHECK(!contains(minimal_xml, "<actions>"));
    SH_CHECK(xml_parses(minimal_xml));

    // Controller request conversion + validation.
    boson::ipc::ToastRequest req;
    req.title = "t";
    req.scenario = "incoming_call";
    req.buttons.resize(6);
    bool threw = false;
    try {
        (void)boson::ipc::shell_detail::to_toast_content(req);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    SH_CHECK(threw);
    req.buttons.resize(1);
    req.buttons[0].activation = "background";
    SH_CHECK(boson::ipc::shell_detail::to_toast_content(req).scenario == boson::shell::ToastScenario::IncomingCall);
    req.duration = "forever";
    threw = false;
    try {
        (void)boson::ipc::shell_detail::to_toast_content(req);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    SH_CHECK(threw);
}

void test_icons() {
    step("icons: badge, stock, glyph, file");
    const auto badge = boson::shell::Icon::badge(L"12");
    SH_CHECK(badge.valid());
    ICONINFO info{};
    if (badge.valid() && GetIconInfo(badge.get(), &info)) {
        BITMAP bmp{};
        GetObjectW(info.hbmColor, sizeof(bmp), &bmp);
        SH_CHECK(bmp.bmWidth == boson::shell::default_small_icon_size());
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
    }
    SH_CHECK(boson::shell::Icon::stock(L"warning").valid());
    SH_CHECK(!boson::shell::Icon::stock(L"no-such-icon").valid());
    SH_CHECK(boson::shell::Icon::glyph(0xE768, RGB(255, 255, 255), 32).valid());
    SH_CHECK(boson::shell::Icon::from_file(boson::app::executable_path()).valid() ||
             boson::shell::Icon::from_file(L"C:\\Windows\\System32\\shell32.dll,3").valid());
    SH_CHECK(boson::shell::Icon::from_file(L"C:\\Windows\\System32\\shell32.dll,3").valid());

    boson::shell::Icon moved = boson::shell::Icon::badge(L"!");
    boson::shell::Icon target = std::move(moved);
    SH_CHECK(target.valid() && !moved.valid()); // NOLINT(bugprone-use-after-move)

    SH_CHECK(boson::ipc::shell_detail::make_icon(boson::ipc::IconSpec{}) == nullptr);
    SH_CHECK(boson::ipc::shell_detail::make_icon(boson::ipc::IconSpec{.badge = "3"}) != nullptr);
    bool threw = false;
    try {
        (void)boson::ipc::shell_detail::make_icon(boson::ipc::IconSpec{.file = "C:\\does\\not\\exist.ico"});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SH_CHECK(threw);
}

void test_power_queries() {
    step("power: status, idle time, session, blockers");
    const auto status = boson::shell::power_status();
    std::cout << "  on_ac=" << status.on_ac << " battery=" << status.has_battery
              << " percent=" << status.battery_percent.value_or(-1) << " saver=" << status.battery_saver << '\n';
    if (status.battery_percent.has_value()) {
        SH_CHECK(*status.battery_percent >= 0 && *status.battery_percent <= 100);
    }
    SH_CHECK(boson::shell::user_idle_time().count() >= 0);
    const auto locked = boson::shell::is_session_locked();
    SH_CHECK(locked.has_value());
    std::cout << "  session locked=" << locked.value_or(false) << " idle_ms=" << boson::shell::user_idle_time().count() << '\n';

    {
        boson::shell::PowerSaveBlocker system(boson::shell::PowerSaveBlockerType::PreventAppSuspension, L"Boson shell test");
        SH_CHECK(system.is_active());
        boson::shell::PowerSaveBlocker display(boson::shell::PowerSaveBlockerType::PreventDisplaySleep, L"Boson shell test");
        SH_CHECK(display.is_active());
    }
    boson::shell::PowerSaveBlockers blockers;
    const auto a = blockers.start(boson::shell::PowerSaveBlockerType::PreventAppSuspension, L"Boson shell test a");
    const auto b = blockers.start(boson::shell::PowerSaveBlockerType::PreventDisplaySleep, L"Boson shell test b");
    SH_CHECK(a != 0 && b != 0 && a != b);
    SH_CHECK(blockers.is_started(a) && blockers.is_started(b));
    SH_CHECK(blockers.size() == 2);
    SH_CHECK(blockers.stop(a));
    SH_CHECK(!blockers.stop(a));
    SH_CHECK(!blockers.is_started(a) && blockers.is_started(b));
    blockers.stop_all();
    SH_CHECK(blockers.size() == 0 && !blockers.is_started(b));
}

void test_auto_launch() {
    step("auto-launch: Run key + StartupApproved");
    const std::wstring name = L"BosonShellTest." + unique_suffix();
    SH_CHECK(!boson::shell::auto_launch_status(name).registered);
    SH_CHECK(!boson::shell::enable_auto_launch({.name = L"", .executable = {}, .arguments = {}, .override_user_disabled = true}));

    boson::shell::AutoLaunchOptions options;
    options.name = name;
    options.arguments = {L"--hidden", L"C:\\path with spaces\\x", L"quote\"d"};
    SH_CHECK(boson::shell::enable_auto_launch(options));
    auto status = boson::shell::auto_launch_status(name);
    SH_CHECK(status.registered && status.enabled && !status.disabled_by_user);
    SH_CHECK(boson::shell::is_auto_launch_enabled(name));
    SH_CHECK(status.executable == boson::app::executable_path());
    SH_CHECK(status.arguments == options.arguments);

    // Simulate the user disabling it in Task Manager (odd first byte).
    {
        HKEY key = nullptr;
        const std::wstring path(boson::shell::kStartupApprovedKey);
        SH_CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS);
        const BYTE disabled[12]{0x03};
        RegSetValueExW(key, name.c_str(), 0, REG_BINARY, disabled, sizeof(disabled));
        RegCloseKey(key);
    }
    status = boson::shell::auto_launch_status(name);
    SH_CHECK(status.registered && status.disabled_by_user && !status.enabled);
    options.override_user_disabled = false;
    SH_CHECK(boson::shell::enable_auto_launch(options));
    SH_CHECK(boson::shell::auto_launch_status(name).disabled_by_user);
    options.override_user_disabled = true;
    SH_CHECK(boson::shell::enable_auto_launch(options));
    SH_CHECK(boson::shell::auto_launch_status(name).enabled);

    // Through the controller / router.
    boson::ipc::AutoLaunchController controller(name);
    boson::ipc::Router<boson::ipc::AutoLaunchController> router(controller);
    const std::string enabled = router.dispatch_sync(R"({"id":1,"method":"AutoLaunch.enable","params":{"args":["--tray"]}})");
    SH_CHECK(contains(enabled, R"("enabled":true)") && contains(enabled, R"("args":["--tray"])"));
    const std::string disabled = router.dispatch_sync(R"({"id":2,"method":"AutoLaunch.disable","params":null})");
    SH_CHECK(contains(disabled, R"("registered":false)"));

    SH_CHECK(boson::shell::disable_auto_launch(name));
    SH_CHECK(!reg_value_exists(boson::shell::kRunKey, name));
    SH_CHECK(!reg_value_exists(boson::shell::kStartupApprovedKey, name));
}

// ---- tests with a window --------------------------------------------------------------------------

void test_taskbar(boson::core::Window& window) {
    step("taskbar: progress, overlay, thumbnail toolbar, clicks, re-init");
    boson::shell::Taskbar taskbar(window);
    SH_CHECK(taskbar.is_available());
    SH_CHECK(taskbar.set_progress_state(boson::shell::TaskbarProgressState::Indeterminate));
    SH_CHECK(taskbar.set_progress(0.25));
    SH_CHECK(taskbar.progress_state() == boson::shell::TaskbarProgressState::Normal);
    SH_CHECK(taskbar.set_progress_state(boson::shell::TaskbarProgressState::Paused));
    SH_CHECK(taskbar.set_progress(3, 4));
    SH_CHECK(taskbar.progress_state() == boson::shell::TaskbarProgressState::Paused);
    SH_CHECK(taskbar.progress_value() > 0.74 && taskbar.progress_value() < 0.76);
    SH_CHECK(taskbar.set_progress_state(boson::shell::TaskbarProgressState::Error));
    SH_CHECK(!taskbar.set_progress(std::numeric_limits<double>::quiet_NaN()));
    SH_CHECK(taskbar.clear_progress());
    SH_CHECK(taskbar.progress_value() == 0.0);

    SH_CHECK(taskbar.set_overlay_icon(std::make_shared<const boson::shell::Icon>(boson::shell::Icon::badge(L"5")), L"5 unread"));
    SH_CHECK(taskbar.has_overlay_icon());
    SH_CHECK(taskbar.clear_overlay_icon());
    SH_CHECK(!taskbar.has_overlay_icon());

    const auto play = std::make_shared<const boson::shell::Icon>(boson::shell::Icon::glyph(0xE768));
    const auto next = std::make_shared<const boson::shell::Icon>(boson::shell::Icon::glyph(0xE893));
    std::vector<std::uint32_t> clicks;
    taskbar.set_thumbnail_click_callback([&](std::uint32_t id) { clicks.push_back(id); });
    SH_CHECK(!taskbar.set_thumbnail_buttons({{.id = 1}, {.id = 1}})); // duplicate ids
    SH_CHECK(!taskbar.set_thumbnail_buttons(std::vector<boson::shell::ThumbnailButton>(8)));
    SH_CHECK(taskbar.set_thumbnail_buttons({
        {.id = 10, .icon = play, .tooltip = L"Play"},
        {.id = 20, .icon = next, .tooltip = L"Next"},
    }));
    SendMessageW(window.hwnd(), WM_COMMAND, MAKEWPARAM(boson::shell::Taskbar::slot_command_id(1), THBN_CLICKED), 0);
    SendMessageW(window.hwnd(), WM_COMMAND, MAKEWPARAM(boson::shell::Taskbar::slot_command_id(0), THBN_CLICKED), 0);
    SendMessageW(window.hwnd(), WM_COMMAND, MAKEWPARAM(boson::shell::Taskbar::slot_command_id(5), THBN_CLICKED), 0); // unused slot
    SH_CHECK((clicks == std::vector<std::uint32_t>{20, 10}));
    SH_CHECK(taskbar.update_thumbnail_button({.id = 20, .icon = next, .tooltip = L"Skip", .enabled = false}));
    SH_CHECK(!taskbar.update_thumbnail_button({.id = 99}));
    SH_CHECK(taskbar.thumbnail_buttons()[1].tooltip == L"Skip");
    SH_CHECK(taskbar.set_thumbnail_buttons({{.id = 30, .icon = play, .tooltip = L"Only"}})); // replace after add
    SH_CHECK(taskbar.set_thumbnail_tooltip(L"Boson shell test"));
    SH_CHECK(taskbar.set_thumbnail_clip(RECT{0, 0, 100, 80}));
    SH_CHECK(!taskbar.set_thumbnail_clip(RECT{10, 10, 5, 5}));
    SH_CHECK(taskbar.set_thumbnail_clip(std::nullopt));

    // Explorer restart: the shell broadcasts TaskbarButtonCreated again; state is re-applied.
    SH_CHECK(taskbar.set_progress(0.5));
    const auto before = taskbar.button_created_count();
    SendMessageW(window.hwnd(), boson::shell::Taskbar::taskbar_button_created_message(), 0, 0);
    SH_CHECK(taskbar.button_created_count() == before + 1);
    SH_CHECK(taskbar.is_available());
    SH_CHECK(taskbar.progress_value() == 0.5 && taskbar.thumbnail_buttons().size() == 1);
    SH_CHECK(taskbar.set_thumbnail_buttons({}));

    SH_CHECK(taskbar.flash({.count = 1, .until_foreground = false, .caption = false, .tray = true, .interval = std::chrono::milliseconds{50}}));
    pump_for(std::chrono::milliseconds{150});
    SH_CHECK(taskbar.stop_flash());
    SH_CHECK(!boson::shell::flash_window(nullptr));
}

void test_power_monitor(boson::core::Window& window) {
    step("power monitor: synthetic WM_POWERBROADCAST / WTS messages");
    boson::shell::PowerMonitor monitor(window);
    SH_CHECK(monitor.suspend_resume_registered());
    SH_CHECK(monitor.session_registered());
    SH_CHECK(monitor.power_setting_registrations() >= 3);
    std::cout << "  power setting registrations: " << monitor.power_setting_registrations() << '\n';

    std::vector<boson::shell::PowerEventKind> kinds;
    const auto id = monitor.on([&](const boson::shell::PowerEvent& e) { kinds.push_back(e.kind); });
    pump_for(std::chrono::milliseconds{100}); // initial power-setting notifications
    kinds.clear();

    const HWND hwnd = window.hwnd();
    SendMessageW(hwnd, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    SH_CHECK(monitor.is_suspended());
    SendMessageW(hwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    SendMessageW(hwnd, WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0); // second resume message: no duplicate
    SH_CHECK(!monitor.is_suspended());

    struct Setting {
        GUID guid;
        DWORD length;
        DWORD value;
    };
    const auto send_setting = [&](const GUID& guid, DWORD value) {
        static_assert(offsetof(POWERBROADCAST_SETTING, Data) == offsetof(Setting, value));
        Setting s{guid, sizeof(DWORD), value};
        SendMessageW(hwnd, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&s));
    };
    constexpr GUID kDisplay{0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
    constexpr GUID kAcDc{0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};
    constexpr GUID kSaver{0xe00958c0, 0xc213, 0x4ace, {0xac, 0x77, 0xfe, 0xcc, 0xed, 0x2e, 0xee, 0xa5}};
    send_setting(kDisplay, 1);
    send_setting(kDisplay, 0);
    SH_CHECK(monitor.display_state() == boson::shell::DisplayState::Off);
    send_setting(kDisplay, 2);
    send_setting(kDisplay, 1);
    SH_CHECK(monitor.display_state() == boson::shell::DisplayState::On);
    const bool was_ac = monitor.last_status().on_ac;
    send_setting(kAcDc, was_ac ? PoDc : PoAc);
    SH_CHECK(monitor.last_status().on_ac == !was_ac);
    send_setting(kAcDc, was_ac ? PoAc : PoDc);
    const bool saver = monitor.last_status().battery_saver;
    send_setting(kSaver, saver ? 0 : 1);
    send_setting(kSaver, saver ? 1 : 0);

    const bool locked = monitor.is_session_locked();
    SendMessageW(hwnd, WM_WTSSESSION_CHANGE, locked ? WTS_SESSION_UNLOCK : WTS_SESSION_LOCK, 0);
    SendMessageW(hwnd, WM_WTSSESSION_CHANGE, locked ? WTS_SESSION_LOCK : WTS_SESSION_UNLOCK, 0);
    SH_CHECK(monitor.is_session_locked() == locked);

    using K = boson::shell::PowerEventKind;
    const std::vector<K> expected{
        K::Suspend, K::Resume,
        K::DisplayStateChanged, K::DisplayStateChanged, K::DisplayStateChanged,
        K::PowerSourceChanged, K::PowerSourceChanged,
        K::BatterySaverChanged, K::BatterySaverChanged,
        locked ? K::SessionUnlocked : K::SessionLocked, locked ? K::SessionLocked : K::SessionUnlocked,
    };
    // The first display message only seeds the state when no real notification arrived before it.
    std::vector<K> filtered = kinds;
    SH_CHECK(filtered == expected || (filtered.size() + 1 == expected.size()));
    if (filtered != expected) {
        std::cout << "  power events seen: " << filtered.size() << " (expected " << expected.size() << ")\n";
    }
    SH_CHECK(monitor.off(id));
    SH_CHECK(!monitor.off(id));
}

struct JumpListFiles {
    std::filesystem::path automatic;
    std::filesystem::path custom;
    std::set<std::filesystem::path> automatic_before;
    std::set<std::filesystem::path> custom_before;
};

void remove_new_files(const std::filesystem::path& dir, const std::set<std::filesystem::path>& before) {
    for (const auto& p : list_dir(dir)) {
        if (!before.contains(p)) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }
}

void test_jump_list(const std::wstring& app_id) {
    step("jump list: tasks, categories, recent documents, clear");
    const auto recent = known_folder(FOLDERID_Recent);
    JumpListFiles files{recent / L"AutomaticDestinations", recent / L"CustomDestinations", {}, {}};
    files.automatic_before = list_dir(files.automatic);
    files.custom_before = list_dir(files.custom);

    boson::shell::JumpListSpec spec;
    spec.tasks = {
        {.title = L"New window", .description = L"Open a new window", .arguments = L"--new-window", .icon_path = L"C:\\Windows\\System32\\shell32.dll", .icon_index = 3},
        {.separator = true},
        {.title = L"Settings", .arguments = L"--settings"},
    };
    spec.categories = {{.name = L"Pinned projects", .items = {{.title = L"Project A", .arguments = L"--open A"}}}};
    spec.show_recent = true;
    const auto result = off_ui([&]() { return boson::shell::set_jump_list(spec, app_id); });
    std::cout << "  jump list hr=0x" << std::hex << static_cast<unsigned long>(result.hr) << std::dec
              << " max_slots=" << result.max_slots << " rejected=" << result.rejected_categories.size() << '\n';
    SH_CHECK(result.ok);
    SH_CHECK(result.max_slots > 0);
    SH_CHECK(!boson::shell::set_jump_list(spec, L"bad id with spaces").ok);

    // Recent document attributed to this AppUserModelID.
    const std::filesystem::path temp = std::filesystem::temp_directory_path() / (L"boson-shell-test-" + unique_suffix() + L".txt");
    {
        HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        SH_CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    SH_CHECK(off_ui([&]() { return boson::shell::add_recent_document(temp.wstring(), app_id); }));
    SH_CHECK(!boson::shell::add_recent_document(L""));
    pump_for(std::chrono::milliseconds{300});
    SH_CHECK(off_ui([&]() { return boson::shell::clear_recent_documents(app_id); }));
    SH_CHECK(off_ui([&]() { return boson::shell::clear_jump_list(app_id); }));

    // Cleanup: the Recent-items shortcut SHAddToRecentDocs created, and any destination files of the test id.
    pump_for(std::chrono::milliseconds{300});
    std::error_code ec;
    // The Recent shortcut is named after the display name, which drops the extension when it is hidden.
    std::filesystem::remove(recent / (temp.filename().wstring() + L".lnk"), ec);
    std::filesystem::remove(recent / (temp.stem().wstring() + L".lnk"), ec);
    std::filesystem::remove(temp, ec);
    remove_new_files(files.automatic, files.automatic_before);
    remove_new_files(files.custom, files.custom_before);
    SH_CHECK(!std::filesystem::exists(recent / (temp.filename().wstring() + L".lnk")));
    SH_CHECK(!std::filesystem::exists(recent / (temp.stem().wstring() + L".lnk")));
}

std::wstring lower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch));
    return s;
}

void test_toasts(boson::core::Window& window, boson::exec::UiMarshal& marshal, const std::wstring& app_id) {
    step("toast: AUMID registration, shortcut, COM activator, show/update/remove");
    SH_CHECK(boson::shell::ToastNotifier::is_supported());

    GUID clsid{};
    SH_CHECK(SUCCEEDED(CoCreateGuid(&clsid)));
    const std::wstring folder = L"BosonShellTest." + unique_suffix();
    const std::wstring shortcut_name = L"Boson Shell Test";

    SH_CHECK(boson::shell::register_app_user_model_id({
        .app_user_model_id = app_id,
        .display_name = L"Boson Shell Test",
        .icon_path = {},
        .icon_background_color = L"FF202020",
        .activator_clsid = clsid,
    }));
    SH_CHECK(boson::shell::is_app_user_model_id_registered(app_id));
    SH_CHECK(boson::shell::register_toast_com_server(clsid));
    const auto command = boson::shell::toast_com_server_command(clsid);
    SH_CHECK(command.has_value() && contains(boson::core::wide_to_utf8(*command), "-ToastActivated"));

    SH_CHECK(boson::shell::create_start_menu_shortcut({
        .name = shortcut_name,
        .folder = folder,
        .app_user_model_id = app_id,
        .toast_activator_clsid = clsid,
        .target = {},
        .arguments = {},
        .description = L"Boson shell integration test",
        .icon_path = {},
        .icon_index = 0,
        .working_directory = {},
    }));
    const auto lnk = boson::shell::start_menu_shortcut_path(shortcut_name, folder);
    SH_CHECK(lnk.has_value() && std::filesystem::exists(*lnk));
    if (lnk.has_value()) {
        const auto info = boson::shell::read_shortcut(*lnk);
        SH_CHECK(info.has_value());
        if (info.has_value()) {
            SH_CHECK(info->app_user_model_id == app_id);
            SH_CHECK(info->toast_activator_clsid == std::optional<GUID>{clsid});
            SH_CHECK(lower(info->target) == lower(boson::app::executable_path()));
        }
    }
    SH_CHECK(!boson::shell::start_menu_shortcut_path(L"..\\evil").has_value());
    SH_CHECK(!boson::shell::start_menu_shortcut_path(L"x", L"..\\..").has_value());

    // COM activator: the shell's call path is CoCreateInstance(CLSCTX_LOCAL_SERVER) + Activate.
    std::optional<boson::shell::ToastActivation> received;
    boson::shell::ToastActivator activator(clsid, [&](boson::shell::ToastActivation a) { received = std::move(a); });
    SH_CHECK(activator.is_registered());
    const HRESULT activate_hr = off_ui([&]() {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT hr = E_FAIL;
        {
            Microsoft::WRL::ComPtr<INotificationActivationCallback> cb;
            hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&cb));
            if (SUCCEEDED(hr)) {
                const NOTIFICATION_USER_INPUT_DATA data[] = {{L"reply", L"héllo"}, {L"choice", L"b"}};
                hr = cb->Activate(app_id.c_str(), L"action=reply&id=7", data, 2);
            }
        }
        if (SUCCEEDED(init)) CoUninitialize();
        return hr;
    });
    SH_CHECK(SUCCEEDED(activate_hr));
    SH_CHECK(received.has_value());
    if (received.has_value()) {
        SH_CHECK(received->app_user_model_id == app_id);
        SH_CHECK(received->arguments == "action=reply&id=7");
        SH_CHECK(received->user_input["reply"] == "héllo");
        SH_CHECK(received->user_input["choice"] == "b");
    }

    // Real toasts (silent).
    boson::shell::ToastNotifier notifier(app_id);
    SH_CHECK(notifier.app_user_model_id() == app_id);
    const auto setting = notifier.setting();
    std::cout << "  toast setting=" << static_cast<int>(setting) << '\n';
    std::vector<boson::shell::ToastEvent> events;
    std::mutex events_mutex;
    notifier.set_event_callback([&](const boson::shell::ToastEvent& e) {
        std::scoped_lock lock(events_mutex);
        events.push_back(e);
    });
    boson::shell::ToastContent content;
    content.title = "Boson shell test";
    content.body = "Automated test toast — it removes itself.";
    content.silent = true;
    content.tag = "shell-test";
    content.group = "boson-tests";
    content.buttons.push_back({.content = "Reply", .arguments = "reply", .activation = boson::shell::ToastActivationType::Background,
                               .input_id = "reply", .image = "", .context_menu = false});
    content.inputs.push_back({.id = "reply", .title = "", .placeholder = "Reply", .default_value = "", .options = {}});
    content.progress = boson::shell::ToastProgress{.title = "Test", .value = 0.1, .value_label = "", .status = "Running"};
    content.expires_after = std::chrono::seconds{120};
    const auto shown = notifier.show(content);
    std::cout << "  toast show hr=0x" << std::hex << static_cast<unsigned long>(shown.hr) << std::dec << '\n';
    const bool toasts_usable = setting == boson::shell::ToastSetting::Enabled || setting == boson::shell::ToastSetting::Unknown;
    if (toasts_usable) {
        SH_CHECK(shown.ok);
        SH_CHECK(shown.tag == "shell-test" && shown.group == "boson-tests");
        SH_CHECK(notifier.active_count() == 1);
        SH_CHECK(notifier.update_progress("shell-test", "boson-tests", {.title = "Test", .value = 0.8, .value_label = "80%", .status = "Almost"}));
        pump_for(std::chrono::milliseconds{300});
        SH_CHECK(notifier.hide("shell-test", "boson-tests"));
        pump_until([&]() {
            std::scoped_lock lock(events_mutex);
            return !events.empty();
        }, std::chrono::seconds{5});
        {
            std::scoped_lock lock(events_mutex);
            SH_CHECK(!events.empty() && events[0].kind == boson::shell::ToastEventKind::Dismissed &&
                     events[0].reason == boson::shell::ToastDismissReason::ApplicationHidden && events[0].tag == "shell-test");
        }
        SH_CHECK(notifier.remove("shell-test", "boson-tests"));
        const auto auto_tag = notifier.show(boson::shell::ToastContent{.title = "Boson second test toast", .silent = true});
        SH_CHECK(auto_tag.ok && auto_tag.tag.starts_with("boson-") && auto_tag.group == "boson");
    } else {
        std::cout << "  toasts disabled for this user/app: show() checks skipped\n";
    }
    boson::shell::ToastContent too_many;
    too_many.buttons.resize(6);
    SH_CHECK(notifier.show(too_many).hr == E_INVALIDARG);
    SH_CHECK(notifier.clear_all());

    // NotificationController through the router, with balloon fallback when no AUMID is usable.
    {
        boson::ipc::NotificationController controller(window, marshal, app_id);
        boson::ipc::Router<boson::ipc::NotificationController> router(controller);
        const std::string support = rpc(router, "Notification.support");
        SH_CHECK(contains(support, R"("supported":true)"));
        if (toasts_usable) {
            const std::string res = rpc(router, "Notification.show",
                R"({"title":"Boson controller toast","body":"x","silent":true,"tag":"ctl","group":"boson-tests","scenario":"default"})");
            SH_CHECK(contains(res, R"("ok":true)") && contains(res, R"("used_fallback":false)"));
            SH_CHECK(contains(rpc(router, "Notification.remove", R"({"tag":"ctl","group":"boson-tests"})"), R"("ok":true)"));
        }
        const std::string bad = rpc(router, "Notification.show", R"({"title":"x","scenario":"nope"})");
        SH_CHECK(contains(bad, R"("status":"error")"));
        SH_CHECK(contains(rpc(router, "Notification.clear_all"), R"("ok":true)"));
    }
    {
        // No AUMID: toast fails, the balloon fallback (Shell_NotifyIcon) is used.
        boson::ipc::NotificationController controller(window, marshal, L"");
        boson::ipc::Router<boson::ipc::NotificationController> router(controller);
        const bool process_has_id = boson::shell::app_user_model_id().has_value();
        if (!process_has_id) {
            const std::string res = rpc(router, "Notification.show", R"({"title":"Boson fallback","body":"balloon","silent":true})");
            SH_CHECK(contains(res, R"("used_fallback":true)"));
            const std::string no_fb = rpc(router, "Notification.show", R"({"title":"x","fallback":false})");
            SH_CHECK(contains(no_fb, R"("ok":false)"));
        }
    }

    // Cleanup.
    SH_CHECK(boson::shell::remove_start_menu_shortcut(shortcut_name, folder));
    if (lnk.has_value()) {
        SH_CHECK(!std::filesystem::exists(*lnk));
        SH_CHECK(!std::filesystem::exists(std::filesystem::path(*lnk).parent_path()));
    }
    SH_CHECK(boson::shell::unregister_toast_com_server(clsid));
    SH_CHECK(!boson::shell::toast_com_server_command(clsid).has_value());
    SH_CHECK(boson::shell::unregister_app_user_model_id(app_id));
    SH_CHECK(!boson::shell::is_app_user_model_id_registered(app_id));
    SH_CHECK(!reg_key_exists(L"Software\\Classes\\CLSID\\" + boson::shell::guid_to_string(clsid)));
}

void test_controllers(boson::core::Window& window, boson::exec::UiMarshal& marshal) {
    step("Taskbar / Power controllers through the router");
    boson::ipc::TaskbarController taskbar(window, marshal);
    boson::ipc::PowerController power(window);
    boson::ipc::Router<boson::ipc::TaskbarController, boson::ipc::PowerController> router(taskbar, power);

    SH_CHECK(contains(rpc(router, "Taskbar.set_progress", R"({"state":"normal","value":0.4})"), R"("ok":true)"));
    SH_CHECK(native_taskbar(taskbar).progress_value() > 0.39 && native_taskbar(taskbar).progress_value() < 0.41);
    SH_CHECK(contains(rpc(router, "Taskbar.set_progress", R"({"state":"bogus"})"), R"("status":"error")"));
    SH_CHECK(contains(rpc(router, "Taskbar.set_overlay", R"({"icon":{"badge":"9"},"description":"9 new"})"), R"("ok":true)"));
    SH_CHECK(native_taskbar(taskbar).has_overlay_icon());
    SH_CHECK(contains(rpc(router, "Taskbar.set_overlay", R"({"icon":{"stock":"warning"},"description":"warn"})"), R"("ok":true)"));
    SH_CHECK(contains(rpc(router, "Taskbar.clear_overlay"), R"("ok":true)"));
    SH_CHECK(!native_taskbar(taskbar).has_overlay_icon());
    SH_CHECK(contains(rpc(router, "Taskbar.set_thumbnail_buttons",
        R"({"buttons":[{"id":1,"icon":{"glyph":59240},"tooltip":"Play"},{"id":2,"icon":{"glyph":59539},"tooltip":"Next"}]})"), R"("ok":true)"));
    SH_CHECK(native_taskbar(taskbar).thumbnail_buttons().size() == 2);
    SH_CHECK(contains(rpc(router, "Taskbar.set_thumbnail_clip", R"({"x":0,"y":0,"width":200,"height":100})"), R"("ok":true)"));
    SH_CHECK(contains(rpc(router, "Taskbar.set_thumbnail_clip", R"({})"), R"("ok":true)"));
    SH_CHECK(contains(rpc(router, "Taskbar.set_thumbnail_tooltip", R"({"tooltip":"Hi"})"), R"("ok":true)"));
    SH_CHECK(contains(rpc(router, "Taskbar.flash", R"({"count":1,"caption":false})"), R"("ok":true)"));
    SH_CHECK(contains(rpc(router, "Taskbar.stop_flash"), R"("ok":true)"));
    SH_CHECK(contains(rpc(router, "Taskbar.set_thumbnail_buttons", R"({"buttons":[]})"), R"("ok":true)"));
    SH_CHECK(contains(rpc(router, "Taskbar.clear_progress"), R"("ok":true)"));

    const std::string status = rpc(router, "Power.status");
    SH_CHECK(contains(status, R"("status":"success")") && contains(status, R"("display":)") && contains(status, R"("idle_ms":)"));
    SH_CHECK(contains(rpc(router, "Power.idle_state", R"({"threshold_seconds":100000})"), R"("state":")"));
    const std::string started = rpc(router, "Power.start_blocker", R"({"type":"prevent_display_sleep","reason":"test"})");
    SH_CHECK(contains(started, R"("started":true)"));
    SH_CHECK(native_power_blockers(power).size() == 1);
    SH_CHECK(contains(rpc(router, "Power.start_blocker", R"({"type":"nope"})"), R"("status":"error")"));
    SH_CHECK(contains(rpc(router, "Power.is_blocker_started", R"({"id":1})"), R"("started":true)"));
    SH_CHECK(contains(rpc(router, "Power.stop_blocker", R"({"id":1})"), R"("ok":true)"));
    SH_CHECK(contains(rpc(router, "Power.stop_blocker", R"({"id":1})"), R"("ok":false)"));
    SH_CHECK(native_power_blockers(power).size() == 0);
}

// ---- end to end through a WebView: events + RPC -----------------------------------------------------

const std::string kPage =
    "<!doctype html><html><head><title>Shell</title><script>window.__events=[];window.__responses=[];"
    "chrome.webview.addEventListener('message',e=>{const d=e.data;if(d&&d.type==='event'){window.__events.push(d);}"
    "else{window.__responses.push(d);}});</script></head><body>shell</body></html>";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/index.html") {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(kPage.data()), kPage.size());
    }
    return std::nullopt;
}

std::optional<std::string> eval(boson::core::WebView& webview, std::string_view js) {
    bool done = false;
    std::optional<std::string> result;
    if (!webview.execute_script(js, [&](HRESULT hr, std::string value) {
            if (SUCCEEDED(hr)) result = std::move(value);
            done = true;
        })) {
        return std::nullopt;
    }
    pump_until([&]() { return done; }, std::chrono::seconds{10});
    return result;
}

void test_webview_events(boson::core::Window& window, boson::exec::UiMarshal& marshal, boson::exec::TaskRunner& runner) {
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
        std::cout << "SKIP webview events: no WebView2 runtime\n";
        return;
    }
    CoTaskMemFree(version);
    step("WebView: power:* and taskbar:thumbnail-click events, Power.status RPC from JS");

    const std::filesystem::path user_data = std::filesystem::temp_directory_path() / (L"boson-shell-it-" + unique_suffix());
    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = user_data.wstring();
    const auto env = boson::core::Environment::create(env_cfg);
    {
        boson::core::WebViewHost host(window, env);
        host.set_default_asset_lookup(&lookup);
        bool nav = false;
        boson::core::WebViewConfig cfg;
        cfg.initial_url = "boson://app/index.html";
        boson::core::WebView& webview = host.create(cfg);
        webview.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { nav = e.is_success; };
        SH_CHECK(pump_until([&]() { return webview.is_ready() && nav; }, std::chrono::seconds{30}));

        boson::ipc::TaskbarController taskbar(window, marshal);
        boson::ipc::PowerController power(window);
        boson::ipc::Router<boson::ipc::TaskbarController, boson::ipc::PowerController> router(taskbar, power);
        boson::ipc::Bridge bridge(runner, marshal, router);
        bridge.attach(webview);
        connect_events(taskbar, bridge);
        connect_events(power, bridge);

        const bool locked = native_power_monitor(power).is_session_locked();
        SendMessageW(window.hwnd(), WM_WTSSESSION_CHANGE, locked ? WTS_SESSION_UNLOCK : WTS_SESSION_LOCK, 0);
        SendMessageW(window.hwnd(), WM_WTSSESSION_CHANGE, locked ? WTS_SESSION_LOCK : WTS_SESSION_UNLOCK, 0);
        SH_CHECK(pump_until([&]() {
            const auto n = eval(webview, "window.__events.filter(e=>e.event.startsWith('power:')).length");
            return n.has_value() && *n == "2";
        }, std::chrono::seconds{10}));
        const auto first = eval(webview, "JSON.stringify(window.__events[0])");
        SH_CHECK(first.has_value() && contains(*first, locked ? "power:unlock" : "power:lock") && contains(*first, "on_ac"));

        SH_CHECK(native_taskbar(taskbar).set_thumbnail_buttons({{.id = 77, .icon = nullptr, .tooltip = L"x"}}));
        SendMessageW(window.hwnd(), WM_COMMAND, MAKEWPARAM(boson::shell::Taskbar::slot_command_id(0), THBN_CLICKED), 0);
        SH_CHECK(pump_until([&]() {
            const auto n = eval(webview, "JSON.stringify(window.__events.filter(e=>e.event==='taskbar:thumbnail-click'))");
            return n.has_value() && contains(*n, "77");
        }, std::chrono::seconds{10}));

        SH_CHECK(eval(webview, "chrome.webview.postMessage({id:3,method:'Power.status',params:{}}); 1").has_value());
        SH_CHECK(pump_until([&]() {
            const auto r = eval(webview, "JSON.stringify(window.__responses)");
            return r.has_value() && contains(*r, "battery_saver");
        }, std::chrono::seconds{10}));
        native_taskbar(taskbar).set_thumbnail_buttons({});

        bool shut = false;
        env->shutdown([&]() { shut = true; });
        pump_until([&]() { return shut; }, std::chrono::seconds{10});
    }
    std::error_code ec;
    std::filesystem::remove_all(user_data, ec);
}

} // namespace

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }

    test_pure_helpers();
    test_icons();
    test_power_queries();
    test_auto_launch();

    boson::core::WindowConfig cfg;
    cfg.title = L"Boson shell integration test";
    cfg.width = 480;
    cfg.height = 320;
    cfg.quit_on_destroy = false;
    auto window = std::make_unique<boson::core::Window>(cfg);
    SH_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));
    pump_for(std::chrono::milliseconds{300}); // let the taskbar button appear

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
    window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    // Fallback path first, while the process has no explicit AppUserModelID.
    const std::wstring app_id = L"Boson.ShellTest." + unique_suffix();
    test_taskbar(*window);
    test_power_monitor(*window);
    test_controllers(*window, marshal);
    test_toasts(*window, marshal, app_id);

    step("set_app_user_model_id");
    SH_CHECK(!boson::shell::set_app_user_model_id(L"bad id"));
    SH_CHECK(boson::shell::set_app_user_model_id(app_id));
    SH_CHECK(boson::shell::app_user_model_id() == std::optional<std::wstring>{app_id});
    SH_CHECK(boson::shell::set_window_app_user_model_id(window->hwnd(), app_id));
    SH_CHECK(boson::shell::set_window_app_user_model_id(window->hwnd(), L""));
    test_jump_list(app_id);

    test_webview_events(*window, marshal, runner);

    // Windows records per-app notification settings for any AUMID that showed a toast.
    RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings\\" + app_id).c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, (L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings\\" + app_id).c_str());

    window.reset();
    std::cout << (g_checks - g_failures) << "/" << g_checks << " shell integration checks passed\n";
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
