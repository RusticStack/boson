// Menus, accelerators, tray and MenuController tests. Windows are created with SW_SHOWNOACTIVATE; popup menus
// are driven by posting keys to the menu window from a thread timer (no SendInput / focus stealing). The
// WebView2 section skips when no runtime is installed. The tray tests use a fake Shell_NotifyIconW backend.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

using boson::menu::Accelerator;
using boson::menu::AcceleratorTable;
using boson::menu::KeyModifiers;
using boson::menu::Menu;
using boson::menu::MenuBar;
using boson::menu::MenuBarMode;
using boson::menu::MenuBarOptions;
using boson::menu::MenuClick;
using boson::menu::MenuItem;
using boson::menu::MenuItemType;
using boson::menu::MenuRole;
using boson::menu::MenuSource;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_menu.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define MT_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

void pump_once() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        pump_once();
    }
    return true;
}

void pump_for(std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    pump_until([until]() { return std::chrono::steady_clock::now() >= until; }, duration + std::chrono::seconds{1});
}

/// Thread-local keyboard state override (SetKeyboardState only affects this thread's GetKeyState view).
class HeldModifiers {
public:
    explicit HeldModifiers(std::initializer_list<int> keys) {
        GetKeyboardState(m_saved.data());
        std::array<BYTE, 256> state = m_saved;
        for (const int vk : keys) {
            state[static_cast<std::size_t>(vk)] = 0x80;
        }
        SetKeyboardState(state.data());
    }
    ~HeldModifiers() { SetKeyboardState(m_saved.data()); }
    HeldModifiers(const HeldModifiers&) = delete;
    HeldModifiers& operator=(const HeldModifiers&) = delete;

private:
    std::array<BYTE, 256> m_saved{};
};

/// Drives the next popup menus of this thread: each "round" is a key sequence posted to the menu window
/// once it is visible. A thread timer keeps firing inside the modal menu loop.
class MenuDriver {
public:
    void push_round(std::vector<UINT> keys) { m_rounds.push_back(std::move(keys)); }
    void start() {
        m_timer.start(std::chrono::milliseconds{30}, [this]() { tick(); }, true);
    }
    void stop() { m_timer.stop(); }
    [[nodiscard]] bool idle() const noexcept { return m_rounds.empty(); }
    int rounds_done = 0;

private:
    static HWND find_menu_window() {
        HWND found = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM lp) -> BOOL {
            wchar_t cls[16]{};
            GetClassNameW(hwnd, cls, 16);
            if (std::wstring_view(cls) == L"#32768" && IsWindowVisible(hwnd) != FALSE) {
                *reinterpret_cast<HWND*>(lp) = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&found));
        return found;
    }

    void tick() {
        if (m_rounds.empty()) {
            return;
        }
        const HWND menu = find_menu_window();
        if (menu == nullptr) {
            m_seen = nullptr;
            return;
        }
        if (menu == m_seen) {
            return; // already sent this round's keys to this menu window
        }
        m_seen = menu;
        const std::vector<UINT> keys = m_rounds.front();
        m_rounds.erase(m_rounds.begin());
        ++rounds_done;
        for (const UINT vk : keys) {
            PostMessageW(menu, WM_KEYDOWN, vk, 1);
            PostMessageW(menu, WM_KEYUP, vk, static_cast<LPARAM>(0xC0000001));
        }
    }

    boson::core::UiTimer m_timer;
    std::vector<std::vector<UINT>> m_rounds;
    HWND m_seen = nullptr;
};

std::unique_ptr<boson::core::Window> make_window(bool headless, std::wstring title) {
    boson::core::WindowConfig cfg;
    cfg.title = std::move(title);
    cfg.width = 520;
    cfg.height = 360;
    cfg.quit_on_destroy = false;
    cfg.headless_titlebar = headless;
    auto window = std::make_unique<boson::core::Window>(cfg);
    window->create(nullptr, SW_SHOWNOACTIVATE);
    return window;
}

std::wstring menu_text(HMENU menu, UINT position) {
    wchar_t buf[256]{};
    GetMenuStringW(menu, position, buf, 256, MF_BYPOSITION);
    return buf;
}

std::vector<std::byte> make_png(int size) {
    using Microsoft::WRL::ComPtr;
    std::vector<std::byte> out;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return out;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return out;
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    encoder->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(static_cast<UINT>(size), static_cast<UINT>(size));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&format);
    std::vector<BYTE> pixels(static_cast<std::size_t>(size * size * 4));
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 0x20;
        pixels[i + 1] = 0x80;
        pixels[i + 2] = 0xF0;
        pixels[i + 3] = 0xFF;
    }
    frame->WritePixels(static_cast<UINT>(size), static_cast<UINT>(size * 4), static_cast<UINT>(pixels.size()), pixels.data());
    frame->Commit();
    encoder->Commit();
    HGLOBAL global = nullptr;
    GetHGlobalFromStream(stream.Get(), &global);
    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    const auto* data = static_cast<const std::byte*>(GlobalLock(global));
    out.assign(data, data + stat.cbSize.QuadPart);
    GlobalUnlock(global);
    return out;
}

void append_u16(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFF));
    out.push_back(static_cast<std::byte>(v >> 8));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
}

/// ICO file with one entry holding image (PNG or a 32bpp DIB).
std::vector<std::byte> make_ico(const std::vector<std::byte>& image, int size, std::uint16_t bit_count) {
    std::vector<std::byte> out;
    append_u16(out, 0);
    append_u16(out, 1);
    append_u16(out, 1);
    out.push_back(static_cast<std::byte>(size >= 256 ? 0 : size));
    out.push_back(static_cast<std::byte>(size >= 256 ? 0 : size));
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
    append_u16(out, 1);
    append_u16(out, bit_count);
    append_u32(out, static_cast<std::uint32_t>(image.size()));
    append_u32(out, 22);
    out.insert(out.end(), image.begin(), image.end());
    return out;
}

std::vector<std::byte> make_dib_icon_image(int size) {
    std::vector<std::byte> out;
    append_u32(out, 40);
    append_u32(out, static_cast<std::uint32_t>(size));
    append_u32(out, static_cast<std::uint32_t>(size * 2));
    append_u16(out, 1);
    append_u16(out, 32);
    append_u32(out, 0);
    append_u32(out, static_cast<std::uint32_t>(size * size * 4));
    append_u32(out, 0);
    append_u32(out, 0);
    append_u32(out, 0);
    append_u32(out, 0);
    for (int i = 0; i < size * size; ++i) {
        append_u32(out, 0xFF3060C0U);
    }
    const int mask_stride = ((size + 31) / 32) * 4;
    out.insert(out.end(), static_cast<std::size_t>(mask_stride * size), std::byte{0});
    return out;
}

int icon_width(HICON icon) {
    ICONINFO info{};
    if (icon == nullptr || GetIconInfo(icon, &info) == FALSE) {
        return 0;
    }
    BITMAP bm{};
    GetObjectW(info.hbmColor != nullptr ? info.hbmColor : info.hbmMask, sizeof(bm), &bm);
    if (info.hbmColor != nullptr) DeleteObject(info.hbmColor);
    if (info.hbmMask != nullptr) DeleteObject(info.hbmMask);
    return bm.bmWidth;
}

std::string base64(const std::vector<std::byte>& bytes) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const std::byte b : bytes) {
        buffer = (buffer << 8) | static_cast<std::uint32_t>(b);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(kTable[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        out.push_back(kTable[(buffer << (6 - bits)) & 0x3F]);
    }
    while (out.size() % 4 != 0) {
        out.push_back('=');
    }
    return out;
}

struct ShellCall {
    DWORD message = 0;
    UINT flags = 0;
    UINT version = 0;
    UINT callback = 0;
    HICON icon = nullptr;
    std::wstring tip;
    std::wstring info_title;
};

struct FakeShell {
    std::vector<ShellCall> calls;
    int fail_next_adds = 0;
    boson::native::TrayIcon::ShellNotifyFn fn() {
        return [this](DWORD message, NOTIFYICONDATAW* data) -> BOOL {
            calls.push_back(ShellCall{
                .message = message,
                .flags = data->uFlags,
                .version = data->uVersion,
                .callback = data->uCallbackMessage,
                .icon = data->hIcon,
                .tip = data->szTip,
                .info_title = data->szInfoTitle,
            });
            if (message == NIM_ADD && fail_next_adds > 0) {
                --fail_next_adds;
                return FALSE;
            }
            return TRUE;
        };
    }
    [[nodiscard]] int count(DWORD message) const {
        int n = 0;
        for (const auto& c : calls) n += c.message == message ? 1 : 0;
        return n;
    }
};

// ---- Sections -----------------------------------------------------------------------------------------

void test_accelerator_parsing() {
    step("accelerator parsing");
    const auto ctrl_shift_p = Accelerator::parse("Ctrl+Shift+P");
    MT_CHECK(ctrl_shift_p.has_value());
    MT_CHECK(ctrl_shift_p->key == 'P');
    MT_CHECK(ctrl_shift_p->modifiers == (KeyModifiers::Ctrl | KeyModifiers::Shift));
    MT_CHECK(ctrl_shift_p->to_string() == "Ctrl+Shift+P");
    MT_CHECK(Accelerator::parse("cmdorctrl+s") == (Accelerator{KeyModifiers::Ctrl, 'S'}));
    MT_CHECK(Accelerator::parse("Alt+F4") == (Accelerator{KeyModifiers::Alt, VK_F4}));
    MT_CHECK(Accelerator::parse("F11") == (Accelerator{KeyModifiers::None, VK_F11}));
    MT_CHECK(Accelerator::parse("Super+Shift+S") == (Accelerator{KeyModifiers::Win | KeyModifiers::Shift, 'S'}));
    MT_CHECK(Accelerator::parse("Ctrl++") == (Accelerator{KeyModifiers::Ctrl, VK_OEM_PLUS}));
    MT_CHECK(Accelerator::parse("Ctrl+Plus") == (Accelerator{KeyModifiers::Ctrl, VK_OEM_PLUS}));
    MT_CHECK(Accelerator::parse("Ctrl+=") == (Accelerator{KeyModifiers::Ctrl, VK_OEM_PLUS}));
    MT_CHECK(Accelerator::parse("Ctrl+-") == (Accelerator{KeyModifiers::Ctrl, VK_OEM_MINUS}));
    MT_CHECK(Accelerator::parse(" ctrl + alt + delete ") == (Accelerator{KeyModifiers::Ctrl | KeyModifiers::Alt, VK_DELETE}));
    MT_CHECK(Accelerator::parse("Ctrl+PageDown") == (Accelerator{KeyModifiers::Ctrl, VK_NEXT}));
    MT_CHECK(Accelerator::parse("Ctrl+0") == (Accelerator{KeyModifiers::Ctrl, '0'}));
    MT_CHECK(Accelerator::parse("Shift+F24") == (Accelerator{KeyModifiers::Shift, VK_F24}));
    MT_CHECK(Accelerator::parse("Ctrl+Backquote") == (Accelerator{KeyModifiers::Ctrl, VK_OEM_3}));
    MT_CHECK(!Accelerator::parse("").has_value());
    MT_CHECK(!Accelerator::parse("Ctrl").has_value());
    MT_CHECK(!Accelerator::parse("Ctrl+Shift").has_value());
    MT_CHECK(!Accelerator::parse("Hyper+K").has_value());
    MT_CHECK(!Accelerator::parse("Ctrl+NoSuchKey").has_value());
    MT_CHECK((Accelerator{KeyModifiers::Ctrl | KeyModifiers::Alt | KeyModifiers::Shift | KeyModifiers::Win, 'K'}).to_string() == "Ctrl+Alt+Shift+Win+K");
    MT_CHECK((Accelerator{KeyModifiers::Ctrl, VK_OEM_PLUS}).to_string() == "Ctrl+Plus");
    MT_CHECK(boson::menu::key_name(VK_F5) == "F5");
    MT_CHECK(boson::menu::is_modifier_key(VK_LCONTROL));
    MT_CHECK(!boson::menu::is_modifier_key('A'));
}

void test_accelerator_table_headless() {
    step("accelerator table without a window");
    AcceleratorTable table;
    int fired = 0;
    const auto id = table.add(Accelerator{KeyModifiers::Ctrl, 'K'}, [&]() { ++fired; });
    MT_CHECK(table.add("Bogus+X", []() {}) == std::nullopt);
    MT_CHECK(table.size() == 1);
    MT_CHECK(table.contains(Accelerator{KeyModifiers::Ctrl, 'K'}));
    MT_CHECK(table.dispatch(Accelerator{KeyModifiers::Ctrl, 'K'}));
    MT_CHECK(fired == 1);
    MT_CHECK(table.dispatch(Accelerator{KeyModifiers::Ctrl, 'K'}, true)); // repeat is swallowed...
    MT_CHECK(fired == 1);                                              // ...without firing
    table.set_allow_repeat(true);
    MT_CHECK(table.dispatch(Accelerator{KeyModifiers::Ctrl, 'K'}, true));
    MT_CHECK(fired == 2);
    MT_CHECK(!table.dispatch(Accelerator{KeyModifiers::None, 'K'}));
    table.set_enabled(false);
    MT_CHECK(!table.dispatch(Accelerator{KeyModifiers::Ctrl, 'K'}));
    table.set_enabled(true);

    // WebView path: modifiers come from the thread keyboard state.
    int f5 = 0;
    table.add("F5", [&]() { ++f5; });
    MT_CHECK(table.handle_webview_key(COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN, VK_F5, false));
    MT_CHECK(f5 == 1);
    MT_CHECK(!table.handle_webview_key(COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP, VK_F5, false));
    MT_CHECK(!table.handle_webview_key(COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN, VK_CONTROL, false));
    {
        const HeldModifiers held{VK_CONTROL};
        MT_CHECK(boson::menu::current_modifiers() == KeyModifiers::Ctrl);
        MT_CHECK(table.handle_webview_key(COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN, 'K', false));
        MT_CHECK(fired == 3);
        MT_CHECK(!table.handle_webview_key(COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN, VK_F5, false)); // Ctrl+F5 unbound
    }
    {
        const HeldModifiers held{VK_MENU};
        std::optional<Accelerator> unmatched;
        table.set_unmatched_webview_key_handler([&](const Accelerator& chord, bool system_key) {
            unmatched = chord;
            return system_key;
        });
        MT_CHECK(table.handle_webview_key(COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN, 'F', false));
        MT_CHECK(unmatched == (Accelerator{KeyModifiers::Alt, 'F'}));
        table.set_unmatched_webview_key_handler({});
    }
    MT_CHECK(table.remove(id));
    MT_CHECK(!table.remove(id));
    MT_CHECK(!table.dispatch(Accelerator{KeyModifiers::Ctrl, 'K'}));
    table.clear();
    MT_CHECK(table.size() == 0);
}

void test_accelerator_table_window() {
    step("accelerator table on a window message loop");
    auto window = make_window(false, L"Boson menu test: accelerators");
    MT_CHECK(window->hwnd() != nullptr);
    int keydowns_seen = 0;
    window->add_message_hook([&](UINT msg, WPARAM, LPARAM) -> std::optional<LRESULT> {
        if (msg == WM_KEYDOWN) {
            ++keydowns_seen;
        }
        return std::nullopt;
    });
    AcceleratorTable table(*window);
    MT_CHECK(table.attached_hwnd() == window->hwnd());
    int f6 = 0;
    bool removed_inside = false;
    std::optional<AcceleratorTable::Id> f6_id;
    f6_id = table.add("F6", [&]() {
        ++f6;
        // Callbacks run outside the hook, so they may edit the table.
        removed_inside = table.remove(*f6_id);
    });
    PostMessageW(window->hwnd(), WM_KEYDOWN, VK_F6, 1);
    MT_CHECK(pump_until([&]() { return f6 == 1; }, std::chrono::seconds{3}));
    MT_CHECK(keydowns_seen == 0); // consumed before DispatchMessage
    MT_CHECK(removed_inside);
    PostMessageW(window->hwnd(), WM_KEYDOWN, VK_F6, 1); // now unbound: reaches the window
    pump_until([&]() { return keydowns_seen == 1; }, std::chrono::seconds{3});
    MT_CHECK(keydowns_seen == 1);
    MT_CHECK(f6 == 1);

    // A second table on another window shares the thread hook; destroying one keeps the other working.
    auto other = make_window(false, L"Boson menu test: accelerators 2");
    int other_fired = 0;
    {
        AcceleratorTable second(*other);
        second.add("F8", [&]() { ++other_fired; });
        PostMessageW(other->hwnd(), WM_KEYDOWN, VK_F8, 1);
        MT_CHECK(pump_until([&]() { return other_fired == 1; }, std::chrono::seconds{3}));
        PostMessageW(window->hwnd(), WM_KEYDOWN, VK_F8, 1); // F8 is not bound on window
        pump_for(std::chrono::milliseconds{100});
        MT_CHECK(other_fired == 1);
    }
    int f9 = 0;
    table.add("F9", [&]() { ++f9; });
    PostMessageW(window->hwnd(), WM_KEYDOWN, VK_F9, 1);
    MT_CHECK(pump_until([&]() { return f9 == 1; }, std::chrono::seconds{3}));

    MSG fake{};
    fake.message = WM_SYSKEYDOWN;
    fake.wParam = VK_F9;
    MT_CHECK(table.translate(fake));
    fake.message = WM_CHAR;
    MT_CHECK(!table.translate(fake));
    pump_for(std::chrono::milliseconds{50});
    MT_CHECK(f9 == 2);
    table.detach_window();
    other.reset();
    window.reset();
}

std::vector<MenuItem> sample_items(std::vector<std::string>& clicks) {
    auto record = [&clicks](const MenuClick& c) { clicks.push_back(c.id); };
    return {
        MenuItem::sub("&File", {
            MenuItem::action("open", "&Open...", "Ctrl+O", record),
            MenuItem::action("save", "&Save", "Ctrl+S", record),
            MenuItem::separator(),
            MenuItem::sub("&Recent", {
                MenuItem::action("recent1", "one.txt", {}, record),
            }, "recent"),
            MenuItem::from_role(MenuRole::Quit),
        }, "file"),
        MenuItem::sub("&View", {
            MenuItem::checkbox("wrap", "Word &Wrap", false, record),
            MenuItem::radio("small", "&Small", "size", false, record),
            MenuItem::radio("medium", "&Medium", "size", true, record),
            MenuItem::radio("large", "&Large", "size", false, record),
            MenuItem::separator(),
            MenuItem::radio("a", "A", "", true, record),
            MenuItem::radio("b", "B", "", false, record),
            MenuItem::separator(),
            MenuItem::radio("c", "C", "", true, record),
        }, "view"),
        MenuItem::from_role(MenuRole::Copy),
    };
}

void test_menu_model() {
    step("menu model, HMENU build and dynamic updates");
    MT_CHECK(boson::menu::mnemonic_of(L"&File") == L'F');
    MT_CHECK(boson::menu::mnemonic_of(L"Save &as") == L'A');
    MT_CHECK(boson::menu::mnemonic_of(L"R&&D") == 0);
    MT_CHECK(boson::menu::mnemonic_of(L"Tom && &Jerry") == L'J');
    MT_CHECK(boson::menu::parse_role("selectAll") == MenuRole::SelectAll);
    MT_CHECK(boson::menu::parse_role("select_all") == MenuRole::SelectAll);
    MT_CHECK(boson::menu::parse_role("TOGGLEDEVTOOLS") == MenuRole::ToggleDevTools);
    MT_CHECK(boson::menu::parse_role("exit") == MenuRole::Quit);
    MT_CHECK(boson::menu::parse_role("") == MenuRole::None);
    MT_CHECK(!boson::menu::parse_role("explode").has_value());
    MT_CHECK(boson::menu::role_is_edit_command(MenuRole::Paste));
    MT_CHECK(!boson::menu::role_is_edit_command(MenuRole::Reload));
    MT_CHECK(MenuItem::from_role(MenuRole::Copy).effective_label() == "&Copy");
    MT_CHECK(MenuItem::from_role(MenuRole::Copy).effective_accelerator() == "Ctrl+C");
    MT_CHECK(MenuItem::from_role(MenuRole::Copy, "Copy &Path").effective_label() == "Copy &Path");
    MT_CHECK(MenuItem::sub("x", {MenuItem::separator()}).effective_type() == MenuItemType::Submenu);

    std::vector<std::string> clicks;
    Menu menu(sample_items(clicks), 100);
    const HMENU root = menu.handle(boson::menu::MenuStyle::Popup);
    MT_CHECK(root != nullptr);
    MT_CHECK(GetMenuItemCount(root) == 3);
    const HMENU file = GetSubMenu(root, 0);
    MT_CHECK(file != nullptr && GetMenuItemCount(file) == 5);
    MT_CHECK(menu_text(file, 0) == L"&Open...\tCtrl+O");
    MT_CHECK(menu_text(root, 2) == L"&Copy\tCtrl+C");
    MT_CHECK(menu_text(file, 4) == L"E&xit");
    const auto open_cmd = menu.command_for("open");
    MT_CHECK(open_cmd == std::optional<UINT>{100});
    MT_CHECK(menu.owns_command(100));
    MT_CHECK(!menu.owns_command(99));
    MT_CHECK(!menu.command_for("file").has_value()); // submenus have no command
    MT_CHECK(menu.item_for_command(*open_cmd)->id == "open");
    MT_CHECK(menu.find("recent1") != nullptr);
    MT_CHECK(menu.find("") == nullptr);

    int changed = 0;
    menu.set_changed_callback([&]() { ++changed; });
    MT_CHECK(menu.set_enabled("save", false));
    MT_CHECK((GetMenuState(file, 1, MF_BYPOSITION) & MF_GRAYED) != 0);
    MT_CHECK(!menu.dispatch(*menu.command_for("save"), MenuSource::ApplicationMenu));
    MT_CHECK(menu.set_enabled("save", true));
    MT_CHECK((GetMenuState(file, 1, MF_BYPOSITION) & MF_GRAYED) == 0);
    MT_CHECK(menu.set_label("open", "&Open File..."));
    MT_CHECK(menu_text(file, 0) == L"&Open File...\tCtrl+O");
    MT_CHECK(menu.set_accelerator("open", "ctrl+shift+o"));
    MT_CHECK(menu_text(file, 0) == L"&Open File...\tCtrl+Shift+O");
    MT_CHECK(menu.set_label("recent", "Recent &Files"));
    MT_CHECK(menu_text(file, 3) == L"Recent &Files");
    MT_CHECK(!menu.set_label("missing", "x"));
    MT_CHECK(changed == 5);

    // Disabled parent blocks children.
    menu.set_enabled("recent", false);
    MT_CHECK(!menu.is_command_enabled(*menu.command_for("recent1")));
    menu.set_enabled("recent", true);
    MT_CHECK(menu.is_command_enabled(*menu.command_for("recent1")));

    // Checkbox toggles, radio groups are exclusive (named group and adjacent run).
    const HMENU view = GetSubMenu(root, 1);
    std::vector<MenuClick> seen;
    menu.set_listener([&](const MenuItem&, const MenuClick& c) { seen.push_back(c); });
    MT_CHECK(menu.dispatch(*menu.command_for("wrap"), MenuSource::ContextMenu));
    MT_CHECK(menu.find("wrap")->checked);
    MT_CHECK((GetMenuState(view, 0, MF_BYPOSITION) & MF_CHECKED) != 0);
    MT_CHECK(seen.size() == 1 && seen[0].checked && seen[0].source == MenuSource::ContextMenu);
    MT_CHECK(menu.dispatch(*menu.command_for("wrap"), MenuSource::ContextMenu));
    MT_CHECK(!menu.find("wrap")->checked);
    MT_CHECK(menu.dispatch(*menu.command_for("large"), MenuSource::ApplicationMenu));
    MT_CHECK(menu.find("large")->checked && !menu.find("medium")->checked && !menu.find("small")->checked);
    MT_CHECK((GetMenuState(view, 2, MF_BYPOSITION) & MF_CHECKED) == 0);
    MT_CHECK((GetMenuState(view, 3, MF_BYPOSITION) & MF_CHECKED) != 0);
    MT_CHECK(menu.find("a")->checked && menu.find("c")->checked); // other runs untouched
    MT_CHECK(menu.dispatch(*menu.command_for("b"), MenuSource::ApplicationMenu));
    MT_CHECK(!menu.find("a")->checked && menu.find("b")->checked && menu.find("c")->checked);
    MT_CHECK(menu.set_checked("small", true));
    MT_CHECK(menu.find("small")->checked && !menu.find("large")->checked);
    MT_CHECK(clicks == (std::vector<std::string>{"wrap", "wrap", "large", "b"}));

    // Visibility and rebuild produce a new handle and generation.
    const auto generation = menu.generation();
    MT_CHECK(menu.set_visible("save", false));
    MT_CHECK(menu.generation() == generation + 1);
    const HMENU rebuilt = menu.current_handle();
    MT_CHECK(rebuilt != nullptr && GetMenuItemCount(GetSubMenu(rebuilt, 0)) == 4);
    MT_CHECK(!menu.command_for("save").has_value());
    MT_CHECK((GetMenuState(GetSubMenu(rebuilt, 1), 3, MF_BYPOSITION) & MF_CHECKED) == 0); // state preserved: small checked
    MT_CHECK((GetMenuState(GetSubMenu(rebuilt, 1), 1, MF_BYPOSITION) & MF_CHECKED) != 0);

    // A callback may replace the whole tree safely.
    menu.find("recent1")->on_click = [&menu](const MenuClick&) { menu.set_items({MenuItem::action("only", "Only")}); };
    MT_CHECK(menu.dispatch(*menu.command_for("recent1"), MenuSource::ApplicationMenu));
    MT_CHECK(menu.items().size() == 1 && menu.find("only") != nullptr);
    MT_CHECK(menu.current_handle() != nullptr && GetMenuItemCount(menu.current_handle()) == 1);

    int accel_count = 0;
    Menu accel_menu(sample_items(clicks));
    accel_menu.for_each_accelerator([&](const MenuItem&, UINT, const Accelerator&) { ++accel_count; });
    MT_CHECK(accel_count == 3); // open, save, copy (role default)
}

void test_roles(boson::core::Window& window) {
    step("roles on a window");
    using boson::menu::RoleContext;
    MT_CHECK(!boson::menu::perform_role(MenuRole::Copy, RoleContext{}));
    MT_CHECK(!boson::menu::perform_role(MenuRole::Minimize, RoleContext{}));
    bool close_requested = false;
    const auto listener = window.on(boson::core::WindowEventKind::CloseRequested, [&](boson::core::WindowEvent& e) {
        close_requested = true;
        e.cancel = true;
    });
    MT_CHECK(boson::menu::perform_role(MenuRole::Close, RoleContext{.window = &window}));
    MT_CHECK(pump_until([&]() { return close_requested; }, std::chrono::seconds{3}));
    bool quit_called = false;
    MT_CHECK(boson::menu::perform_role(MenuRole::Quit, RoleContext{.window = &window, .on_quit = [&]() { quit_called = true; }}));
    MT_CHECK(quit_called);
    window.off(listener);
    MT_CHECK(boson::menu::perform_role(MenuRole::Minimize, RoleContext{.window = &window}));
    pump_for(std::chrono::milliseconds{100});
    MT_CHECK(window.is_minimized());
    ShowWindow(window.hwnd(), SW_SHOWNOACTIVATE);
    pump_for(std::chrono::milliseconds{100});
    MT_CHECK(!window.is_minimized());
    // Role items in a Menu run the role when they have no on_click.
    Menu menu({MenuItem::from_role(MenuRole::HideWindow)});
    (void)menu.handle();
    MT_CHECK(menu.dispatch(*menu.command_for("hideWindow"), MenuSource::ContextMenu, RoleContext{.window = &window}));
    MT_CHECK(!window.is_visible());
    ShowWindow(window.hwnd(), SW_SHOWNOACTIVATE);
}

void test_popups(boson::core::Window& window) {
    step("popup menus (TrackPopupMenuEx, keyboard driven)");
    MT_CHECK(boson::menu::dark_menus_supported());
    MT_CHECK(boson::menu::set_menu_theme(window.hwnd(), boson::menu::MenuTheme::Dark));

    std::vector<std::string> clicks;
    Menu menu(sample_items(clicks));
    MenuDriver driver;
    driver.start();

    boson::menu::PopupOptions options;
    options.owner = window.hwnd();
    RECT rc = window.bounds();
    options.screen_point = POINT{rc.left + 40, rc.top + 60};
    options.theme = boson::menu::MenuTheme::Dark;

    // Escape dismisses -> nullopt.
    driver.push_round({VK_ESCAPE});
    MT_CHECK(!boson::menu::popup_menu(menu, options).has_value());
    MT_CHECK(driver.rounds_done == 1);
    MT_CHECK(!boson::menu::is_popup_menu_open());

    // Down (highlight File), Right (open it on "open"), Down, Return -> "save".
    driver.push_round({VK_DOWN, VK_RIGHT, VK_DOWN, VK_RETURN});
    const auto chosen = boson::menu::popup_menu(menu, options);
    MT_CHECK(chosen.has_value() && chosen->id == "save");
    MT_CHECK(clicks == (std::vector<std::string>{"save"}));

    // Checkbox from a popup reports the new state; dispatch=false leaves it to the caller.
    const HMENU view = GetSubMenu(menu.handle(), 1);
    driver.push_round({VK_DOWN, VK_RETURN});
    const auto wrap = boson::menu::popup_menu(menu, view, options);
    MT_CHECK(wrap.has_value() && wrap->id == "wrap" && wrap->checked);
    MT_CHECK(menu.find("wrap")->checked);
    options.dispatch = false;
    driver.push_round({VK_DOWN, VK_RETURN});
    const auto wrap2 = boson::menu::popup_menu(menu, view, options);
    MT_CHECK(wrap2.has_value() && wrap2->id == "wrap" && !wrap2->checked);
    MT_CHECK(menu.find("wrap")->checked); // not dispatched
    options.dispatch = true;
    driver.stop();
    MT_CHECK(boson::menu::set_menu_theme(window.hwnd(), boson::menu::MenuTheme::System));
}

void test_native_menu_bar() {
    step("native menu bar");
    auto window = make_window(false, L"Boson menu test: native bar");
    MenuBar bar(*window);
    MT_CHECK(bar.mode() == MenuBarMode::Native);
    MT_CHECK(bar.theme() == boson::menu::MenuTheme::Dark); // window default dark_mode = true
    std::vector<std::string> clicks;
    std::vector<MenuClick> listened;
    bar.set_click_listener([&](const MenuItem&, const MenuClick& c) { listened.push_back(c); });
    bar.set_items(sample_items(clicks));
    MT_CHECK(GetMenu(window->hwnd()) == bar.handle());
    MT_CHECK(GetMenuItemCount(bar.handle()) == 3);
    MT_CHECK(menu_text(bar.handle(), 0) == L"&File");
    RedrawWindow(window->hwnd(), nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW); // UAH dark paint path
    pump_for(std::chrono::milliseconds{50});

    const UINT open_cmd = *bar.menu().command_for("open");
    MT_CHECK(open_cmd >= 0x7000);
    SendMessageW(window->hwnd(), WM_COMMAND, MAKEWPARAM(open_cmd, 0), 0);
    MT_CHECK(clicks == (std::vector<std::string>{"open"}));
    MT_CHECK(listened.size() == 1 && listened[0].source == MenuSource::ApplicationMenu);

    // Accelerators are bound from the items (edit roles are display-only).
    MT_CHECK(bar.accelerators().contains(Accelerator{KeyModifiers::Ctrl, 'O'}));
    MT_CHECK(bar.accelerators().contains(Accelerator{KeyModifiers::Ctrl, 'S'}));
    MT_CHECK(!bar.accelerators().contains(Accelerator{KeyModifiers::Ctrl, 'C'}));
    MT_CHECK(bar.accelerators().dispatch(Accelerator{KeyModifiers::Ctrl, 'S'}));
    MT_CHECK(pump_until([&]() { return clicks.size() == 2; }, std::chrono::seconds{3}));
    MT_CHECK(clicks.back() == "save" && listened.back().source == MenuSource::Accelerator);
    {
        const HeldModifiers held{VK_CONTROL};
        PostMessageW(window->hwnd(), WM_KEYDOWN, 'O', 1); // through the thread hook
        MT_CHECK(pump_until([&]() { return clicks.size() == 3; }, std::chrono::seconds{3}));
    }
    MT_CHECK(bar.set_enabled("save", false));
    MT_CHECK(!bar.accelerators().contains(Accelerator{KeyModifiers::Ctrl, 'S'}));
    MT_CHECK(bar.set_accelerator("open", "F3"));
    MT_CHECK(bar.accelerators().contains(Accelerator{KeyModifiers::None, VK_F3}));
    MT_CHECK(!bar.accelerators().contains(Accelerator{KeyModifiers::Ctrl, 'O'}));
    MT_CHECK(bar.set_label("file", "&Datei"));
    MT_CHECK(menu_text(bar.handle(), 0) == L"&Datei");
    const auto user_id = bar.accelerators().add("Ctrl+Shift+K", []() {});
    MT_CHECK(user_id.has_value());
    bar.rebuild();
    MT_CHECK(GetMenu(window->hwnd()) == bar.handle());
    MT_CHECK(bar.accelerators().contains(Accelerator{KeyModifiers::Ctrl | KeyModifiers::Shift, 'K'})); // user binding kept
    const auto entries = bar.entries();
    MT_CHECK(entries.size() == 3 && entries[0].text == "Datei" && entries[0].mnemonic == "D" && entries[0].has_submenu);
    MT_CHECK(entries[2].id == "copy" && !entries[2].has_submenu);
    MT_CHECK(bar.index_of("view") == std::optional<std::size_t>{1});
    MT_CHECK(bar.index_of("Datei") == std::optional<std::size_t>{0});
    bar.clear();
    MT_CHECK(GetMenu(window->hwnd()) == nullptr);
    MT_CHECK(!bar.accelerators().contains(Accelerator{KeyModifiers::None, VK_F3}));
}

void test_popup_menu_bar() {
    step("headless window: menu bar as anchored popups");
    auto window = make_window(true, L"Boson menu test: headless bar");
    MenuBar bar(*window);
    MT_CHECK(bar.mode() == MenuBarMode::Popups);
    std::vector<std::string> clicks;
    std::vector<MenuClick> listened;
    bar.set_click_listener([&](const MenuItem&, const MenuClick& c) { listened.push_back(c); });
    std::vector<MenuItem> items = sample_items(clicks);
    items.push_back(MenuItem::action("about", "&About", {}, [&](const MenuClick& c) { clicks.push_back(c.id); }));
    bar.set_items(std::move(items));
    MT_CHECK(GetMenu(window->hwnd()) == nullptr);
    MT_CHECK(bar.handle() != nullptr);
    bar.set_anchor_rects({RECT{0, 0, 40, 30}, RECT{40, 0, 80, 30}, RECT{80, 0, 120, 30}, RECT{120, 0, 160, 30}});

    std::vector<HMENU> opened;
    window->add_message_hook([&](UINT msg, WPARAM w_param, LPARAM) -> std::optional<LRESULT> {
        if (msg == WM_INITMENUPOPUP) {
            opened.push_back(reinterpret_cast<HMENU>(w_param));
        }
        return std::nullopt;
    });

    MenuDriver driver;
    driver.start();
    driver.push_round({VK_DOWN, VK_RETURN}); // File -> first item
    auto chosen = bar.open_submenu(0);
    MT_CHECK(chosen.has_value() && chosen->id == "open");
    MT_CHECK(listened.size() == 1 && listened[0].source == MenuSource::ApplicationMenu);

    // Right arrow on a plain item switches to the next top-level menu; Left goes back (wrapping skips the
    // leaf entries "copy"/"about").
    opened.clear();
    driver.push_round({VK_DOWN, VK_RIGHT});
    driver.push_round({VK_DOWN, VK_RETURN});
    chosen = bar.open_submenu(0);
    MT_CHECK(chosen.has_value() && chosen->id == "wrap");
    MT_CHECK(!opened.empty() && opened.front() == GetSubMenu(bar.handle(), 0));
    MT_CHECK(std::ranges::find(opened, GetSubMenu(bar.handle(), 1)) != opened.end());
    driver.push_round({VK_LEFT});
    driver.push_round({VK_ESCAPE});
    opened.clear();
    chosen = bar.open_submenu(0);
    MT_CHECK(!chosen.has_value());
    MT_CHECK(std::ranges::find(opened, GetSubMenu(bar.handle(), 1)) != opened.end()); // wrapped to View

    // A leaf top-level entry is dispatched directly.
    chosen = bar.open_submenu(3);
    MT_CHECK(chosen.has_value() && chosen->id == "about" && clicks.back() == "about");
    MT_CHECK(!bar.open_submenu(99).has_value());

    // Alt+V inside a WebView (unmatched accelerator) opens the View submenu via a posted message.
    opened.clear();
    driver.push_round({VK_ESCAPE});
    {
        const HeldModifiers held{VK_MENU};
        MT_CHECK(bar.accelerators().handle_webview_key(COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN, 'V', false));
        MT_CHECK(!bar.accelerators().handle_webview_key(COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN, 'Q', false));
    }
    MT_CHECK(pump_until([&]() { return driver.idle() && !opened.empty(); }, std::chrono::seconds{5}));
    MT_CHECK(!opened.empty() && opened.front() == GetSubMenu(bar.handle(), 1));
    pump_for(std::chrono::milliseconds{100});
    driver.stop();
}

void test_icons() {
    step("icons from PNG / ICO bytes (WIC)");
    const std::vector<std::byte> png = make_png(48);
    MT_CHECK(png.size() > 8);
    auto from_png = boson::native::load_icon_from_image(png, 16);
    MT_CHECK(from_png != nullptr && icon_width(from_png.get()) == 16);
    auto from_png_large = boson::native::load_icon_from_image(png, 32);
    MT_CHECK(from_png_large != nullptr && icon_width(from_png_large.get()) == 32);
    auto from_png_ico = boson::native::load_icon_from_image(make_ico(png, 48, 32), 24);
    MT_CHECK(from_png_ico != nullptr && icon_width(from_png_ico.get()) == 24);
    auto from_dib_ico = boson::native::load_icon_from_image(make_ico(make_dib_icon_image(16), 16, 32), 16);
    MT_CHECK(from_dib_ico != nullptr && icon_width(from_dib_ico.get()) == 16);
    const std::vector<std::byte> garbage(64, std::byte{0x5A});
    MT_CHECK(boson::native::load_icon_from_image(garbage, 16) == nullptr);
    MT_CHECK(boson::native::load_icon_from_image({}, 16) == nullptr);
    MT_CHECK(boson::native::small_icon_size() >= 16);
}

void test_tray() {
    step("tray icon (fake shell backend)");
    FakeShell shell;
    std::vector<boson::native::TrayEvent> events;
    {
        boson::native::TrayIcon tray;
        tray.set_shell_function(shell.fn());
        tray.set_event_callback([&](const boson::native::TrayEvent& e) { events.push_back(e); });
        MT_CHECK(tray.message_window() != nullptr);
        MT_CHECK(tray.set_tooltip("Boson tray")); // not visible yet: nothing sent
        MT_CHECK(shell.calls.empty());
        MT_CHECK(tray.show());
        MT_CHECK(tray.is_visible() && tray.registration_count() == 1);
        MT_CHECK(shell.calls.size() == 2);
        MT_CHECK(shell.calls[0].message == NIM_ADD);
        MT_CHECK((shell.calls[0].flags & (NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP)) == (NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP));
        MT_CHECK(shell.calls[0].callback == boson::native::TrayIcon::kWmTrayCallback);
        MT_CHECK(shell.calls[0].tip == L"Boson tray");
        MT_CHECK(shell.calls[0].icon != nullptr);
        MT_CHECK(shell.calls[1].message == NIM_SETVERSION && shell.calls[1].version == NOTIFYICON_VERSION_4);
        MT_CHECK(tray.show()); // idempotent
        MT_CHECK(shell.calls.size() == 2);

        MT_CHECK(tray.set_tooltip(std::string(300, 'x')));
        MT_CHECK(shell.calls.back().message == NIM_MODIFY && shell.calls.back().tip.size() == 127);
        MT_CHECK(tray.tooltip().size() == 300);

        const std::vector<std::byte> png = make_png(32);
        MT_CHECK(tray.set_icon_from_bytes(png));
        MT_CHECK(shell.calls.back().message == NIM_MODIFY && (shell.calls.back().flags & NIF_ICON) != 0);
        MT_CHECK(shell.calls.back().icon == tray.icon());
        MT_CHECK(!tray.set_icon_from_bytes(std::vector<std::byte>(10, std::byte{1})));
        MT_CHECK(tray.set_icon(LoadIconW(nullptr, IDI_INFORMATION)));

        // Explorer restart: TaskbarCreated re-adds the icon.
        SendMessageW(tray.message_window(), boson::native::TrayIcon::taskbar_created_message(), 0, 0);
        MT_CHECK(tray.registration_count() == 2);
        MT_CHECK(shell.count(NIM_ADD) == 2 && shell.count(NIM_SETVERSION) == 2);
        // A failing NIM_ADD (stale icon) is retried after NIM_DELETE.
        shell.fail_next_adds = 1;
        SendMessageW(tray.message_window(), boson::native::TrayIcon::taskbar_created_message(), 0, 0);
        MT_CHECK(tray.registration_count() == 3 && tray.is_visible());

        // Shell callbacks (NOTIFYICON_VERSION_4 layout).
        const auto send = [&](UINT event, int x, int y, UINT id = 1) {
            SendMessageW(tray.message_window(), boson::native::TrayIcon::kWmTrayCallback,
                         MAKEWPARAM(x, y), MAKELPARAM(event, id));
        };
        send(NIN_SELECT, 100, 200);
        send(WM_CONTEXTMENU, 101, 201); // no menu: event only
        send(WM_LBUTTONDBLCLK, 102, 202);
        send(WM_MBUTTONUP, 103, 203);
        send(NIN_KEYSELECT, 104, 204);
        send(NIN_BALLOONUSERCLICK, 0, 0);
        send(NIN_BALLOONTIMEOUT, 0, 0);
        send(NIN_SELECT, 1, 1, 7); // other icon id: ignored
        MT_CHECK(events.size() == 7);
        if (events.size() == 7) {
            MT_CHECK(events[0].kind == boson::native::TrayEventKind::LeftClick && events[0].position.x == 100 && events[0].position.y == 200);
            MT_CHECK(events[1].kind == boson::native::TrayEventKind::RightClick);
            MT_CHECK(events[2].kind == boson::native::TrayEventKind::DoubleClick);
            MT_CHECK(events[3].kind == boson::native::TrayEventKind::MiddleClick);
            MT_CHECK(events[4].kind == boson::native::TrayEventKind::KeyboardSelect);
            MT_CHECK(events[5].kind == boson::native::TrayEventKind::BalloonClicked);
            MT_CHECK(events[6].kind == boson::native::TrayEventKind::BalloonDismissed);
        }
        MT_CHECK(boson::native::tray_event_name(boson::native::TrayEventKind::DoubleClick) == "double");

        // Balloon reuses the tray icon.
        MT_CHECK(tray.show_balloon(boson::native::NotificationOptions{.title = "Hello", .body = "World"}));
        MT_CHECK(shell.calls.back().message == NIM_MODIFY && (shell.calls.back().flags & NIF_INFO) != 0);
        MT_CHECK(shell.calls.back().info_title == L"Hello");

        // Right click with a menu pops it up (driven by keys); the click is reported with source Tray.
        std::vector<MenuClick> menu_clicks;
        tray.set_menu_listener([&](const MenuItem&, const MenuClick& c) { menu_clicks.push_back(c); });
        int show_clicked = 0;
        tray.set_menu({
            MenuItem::action("show", "&Show", {}, [&](const MenuClick&) { ++show_clicked; }),
            MenuItem::separator(),
            MenuItem::from_role(MenuRole::Quit),
        });
        tray.set_foreground_on_popup(false);
        MenuDriver driver;
        driver.start();
        driver.push_round({VK_DOWN, VK_RETURN});
        send(WM_CONTEXTMENU, 300, 300);
        MT_CHECK(show_clicked == 1);
        MT_CHECK(menu_clicks.size() == 1 && menu_clicks[0].id == "show" && menu_clicks[0].source == MenuSource::Tray);
        bool quit = false;
        tray.set_role_context(boson::menu::RoleContext{.on_quit = [&]() { quit = true; }});
        driver.push_round({VK_UP, VK_RETURN});
        tray.set_menu_on_left_click(true);
        send(NIN_SELECT, 300, 300);
        MT_CHECK(quit);
        driver.stop();

        tray.hide();
        MT_CHECK(!tray.is_visible() && shell.calls.back().message == NIM_DELETE);
        MT_CHECK(tray.show());
    }
    MT_CHECK(shell.calls.back().message == NIM_DELETE); // destructor removes the icon

    // SystemTray is now an alias of TrayIcon and keeps the old API.
    FakeShell legacy_shell;
    {
        boson::native::SystemTray legacy;
        legacy.set_shell_function(legacy_shell.fn());
        MT_CHECK(legacy.show(nullptr, "legacy"));
        MT_CHECK(legacy.update_tooltip("legacy 2"));
        MT_CHECK(legacy_shell.calls.back().tip == L"legacy 2");
    }
}

void test_controller_helpers() {
    step("MenuController template conversion and base64");
    using boson::ipc::MenuItemTemplate;
    std::vector<MenuItemTemplate> templates{
        MenuItemTemplate{.id = "file", .label = "&File", .submenu = {
            MenuItemTemplate{.id = "new", .label = "&New", .accelerator = "Ctrl+N"},
            MenuItemTemplate{.type = "separator"},
            MenuItemTemplate{.id = "auto", .label = "Auto save", .type = "checkbox", .checked = true},
            MenuItemTemplate{.id = "r1", .label = "R1", .type = "radio", .group = "g"},
            MenuItemTemplate{.role = "quit"},
        }},
        MenuItemTemplate{.id = "empty", .label = "Empty", .type = "submenu"},
    };
    const auto items = boson::ipc::to_menu_items(templates);
    MT_CHECK(items.size() == 2);
    MT_CHECK(items[0].effective_type() == MenuItemType::Submenu && items[0].submenu.size() == 5);
    MT_CHECK(items[0].submenu[1].type == MenuItemType::Separator);
    MT_CHECK(items[0].submenu[2].type == MenuItemType::Checkbox && items[0].submenu[2].checked);
    MT_CHECK(items[0].submenu[3].type == MenuItemType::Radio && items[0].submenu[3].radio_group == "g");
    MT_CHECK(items[0].submenu[4].role == MenuRole::Quit && items[0].submenu[4].id == "quit");
    MT_CHECK(items[1].type == MenuItemType::Normal);
    const auto throws = [](auto fn) {
        try {
            fn();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    MT_CHECK(throws([] { (void)boson::ipc::to_menu_items({MenuItemTemplate{.type = "slider"}}); }));
    MT_CHECK(throws([] { (void)boson::ipc::to_menu_items({MenuItemTemplate{.role = "explode"}}); }));
    MT_CHECK(throws([] { (void)boson::ipc::to_menu_items({MenuItemTemplate{.accelerator = "Ctrl+Nope"}}); }));
    MenuItemTemplate deep{.id = "d"};
    for (int i = 0; i < 20; ++i) {
        deep = MenuItemTemplate{.id = "d" + std::to_string(i), .submenu = {deep}};
    }
    MT_CHECK(throws([&] { (void)boson::ipc::to_menu_items({deep}); }));

    const auto bytes = boson::ipc::decode_base64("SGVsbG8sIEJvc29uIQ==");
    MT_CHECK(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == "Hello, Boson!");
    MT_CHECK(boson::ipc::decode_base64("data:image/png;base64,QUJD").size() == 3);
    MT_CHECK(boson::ipc::decode_base64("QUI").size() == 2);
    MT_CHECK(throws([] { (void)boson::ipc::decode_base64("QU*D"); }));

    std::string json;
    MT_CHECK(!glz::write_json(boson::ipc::MenuClickEvent{.id = "x", .source = "tray", .checked = true}, json));
    MT_CHECK(json == R"({"id":"x","source":"tray","checked":true})");
    boson::ipc::ContextMenuRequest parsed;
    MT_CHECK(!glz::read_json(parsed, R"({"items":[{"id":"a","label":"A","submenu":[{"id":"b","label":"B"}]}],"x":10,"y":20})"));
    MT_CHECK(parsed.items.size() == 1 && parsed.items[0].submenu.size() == 1 && parsed.x == std::optional<double>{10.0});
}

void test_controller_rpc() {
    step("MenuController over the router (UI thread marshaling)");
    auto window = make_window(true, L"Boson menu test: controller");
    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
    window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::WebView webview; // never initialized: zoom 1, bounds 0
    boson::ipc::MenuBackend backend(*window, webview);
    boson::ipc::MenuController controller(backend, marshal);
    boson::ipc::Router<boson::ipc::MenuController> router(controller);
    std::vector<MenuClick> clicks;
    backend.set_click_listener([&](const MenuItem&, const MenuClick& c) { clicks.push_back(c); });
    FakeShell shell;
    backend.tray().set_shell_function(shell.fn());
    backend.tray().set_foreground_on_popup(false);
    std::vector<boson::native::TrayEvent> tray_events;
    backend.set_tray_listener([&](const boson::native::TrayEvent& e) { tray_events.push_back(e); });

    const auto call = [&](std::string_view json) {
        std::optional<std::string> reply;
        router.dispatch_async(json, runner, [&reply](std::string r) { reply = std::move(r); });
        pump_until([&]() { return reply.has_value(); }, std::chrono::seconds{10});
        return reply.value_or("<timeout>");
    };

    std::string r = call(R"({"id":1,"method":"Menu.set_application_menu","params":{"items":[)"
                         R"({"id":"file","label":"&File","submenu":[{"id":"open","label":"&Open","accelerator":"Ctrl+O"},{"type":"separator"},{"role":"quit"}]},)"
                         R"({"id":"edit","label":"&Edit","submenu":[{"role":"undo"},{"role":"selectAll"}]}]}})");
    MT_CHECK(r.find("\"success\"") != std::string::npos && r.find("\"value\":true") != std::string::npos);
    MT_CHECK(backend.menu_bar().entries().size() == 2);
    MT_CHECK(backend.menu_bar().accelerators().contains(Accelerator{KeyModifiers::Ctrl, 'O'}));

    r = call(R"({"id":2,"method":"Menu.get_application_menu","params":null})");
    MT_CHECK(r.find("\"mode\":\"popups\"") != std::string::npos);
    MT_CHECK(r.find("\"text\":\"Edit\"") != std::string::npos && r.find("\"mnemonic\":\"E\"") != std::string::npos);

    r = call(R"({"id":3,"method":"Menu.update_menu_item","params":{"id":"open","enabled":false,"label":"&Open!"}})");
    MT_CHECK(r.find("\"value\":true") != std::string::npos);
    MT_CHECK(!backend.menu_bar().menu().find("open")->enabled);
    MT_CHECK(!backend.menu_bar().accelerators().contains(Accelerator{KeyModifiers::Ctrl, 'O'}));
    r = call(R"({"id":4,"method":"Menu.update_menu_item","params":{"id":"nope","enabled":false}})");
    MT_CHECK(r.find("\"value\":false") != std::string::npos);
    r = call(R"({"id":5,"method":"Menu.set_application_menu","params":{"items":[{"id":"x","role":"teleport"}]}})");
    MT_CHECK(r.find("\"error\"") != std::string::npos && r.find("teleport") != std::string::npos);
    MT_CHECK(backend.menu_bar().entries().size() == 2); // unchanged on error

    MenuDriver driver;
    driver.start();
    driver.push_round({VK_DOWN, VK_DOWN, VK_RETURN});
    r = call(R"({"id":6,"method":"Menu.popup_context_menu","params":{"items":[{"id":"cut","label":"Cu&t"},{"id":"paste","label":"&Paste"}],"x":12,"y":34}})");
    MT_CHECK(r.find("\"selected\":true") != std::string::npos && r.find("\"id\":\"paste\"") != std::string::npos);
    MT_CHECK(clicks.size() == 1 && clicks[0].id == "paste" && clicks[0].source == MenuSource::ContextMenu);
    driver.push_round({VK_ESCAPE});
    r = call(R"({"id":7,"method":"Menu.popup_context_menu","params":{"items":[{"id":"cut","label":"Cut"}]}})");
    MT_CHECK(r.find("\"selected\":false") != std::string::npos);

    call(R"({"id":8,"method":"Menu.set_application_menu_anchors","params":{"rects":[{"x":0,"y":0,"width":40,"height":30},{"x":40,"y":0,"width":40,"height":30}]}})");
    pump_for(std::chrono::milliseconds{50});
    MT_CHECK(backend.menu_bar().anchor_rects().size() == 2);
    driver.push_round({VK_DOWN, VK_DOWN, VK_RETURN}); // Edit: undo, selectAll -> selectAll
    r = call(R"({"id":9,"method":"Menu.popup_application_menu","params":{"id":"edit"}})");
    MT_CHECK(r.find("\"id\":\"selectAll\"") != std::string::npos);
    MT_CHECK(clicks.size() == 2 && clicks[1].source == MenuSource::ApplicationMenu);
    r = call(R"({"id":10,"method":"Menu.popup_application_menu","params":{"id":"missing"}})");
    MT_CHECK(r.find("\"error\"") != std::string::npos);

    const std::string png64 = base64(make_png(32));
    r = call(R"({"id":11,"method":"Menu.tray_show","params":{"tooltip":"From JS","icon_base64":"data:image/png;base64,)" + png64 +
             R"(","menu":[{"id":"t1","label":"Tray One"}],"menu_on_left_click":false}})");
    MT_CHECK(r.find("\"value\":true") != std::string::npos);
    MT_CHECK(backend.tray().is_visible() && shell.count(NIM_ADD) == 1);
    MT_CHECK(backend.tray().tooltip() == "From JS");
    r = call(R"({"id":12,"method":"Menu.tray_set_tooltip","params":{"tooltip":"Changed"}})");
    MT_CHECK(shell.calls.back().tip == L"Changed");
    r = call(R"({"id":13,"method":"Menu.tray_set_icon","params":{"icon_base64":"AAAA"}})");
    MT_CHECK(r.find("\"value\":false") != std::string::npos);
    r = call(R"({"id":14,"method":"Menu.tray_show_balloon","params":{"title":"T","body":"B","level":"warning","silent":true}})");
    MT_CHECK(r.find("\"value\":true") != std::string::npos && shell.calls.back().info_title == L"T");
    r = call(R"({"id":15,"method":"Menu.update_menu_item","params":{"id":"t1","target":"tray","label":"Tray 1"}})");
    MT_CHECK(r.find("\"value\":true") != std::string::npos && backend.tray().menu().find("t1")->label == "Tray 1");
    driver.push_round({VK_DOWN, VK_RETURN});
    SendMessageW(backend.tray().message_window(), boson::native::TrayIcon::kWmTrayCallback, MAKEWPARAM(5, 6), MAKELPARAM(WM_CONTEXTMENU, 1));
    MT_CHECK(tray_events.size() == 1 && tray_events[0].kind == boson::native::TrayEventKind::RightClick);
    MT_CHECK(clicks.size() == 3 && clicks[2].id == "t1" && clicks[2].source == MenuSource::Tray);
    call(R"({"id":16,"method":"Menu.tray_hide","params":null})");
    pump_for(std::chrono::milliseconds{50});
    MT_CHECK(!backend.tray().is_visible());
    call(R"({"id":17,"method":"Menu.clear_application_menu","params":null})");
    pump_for(std::chrono::milliseconds{50});
    MT_CHECK(backend.menu_bar().empty());
    driver.stop();
}

void test_tsgen() {
    step("TsGenerator over MenuController + MenuEvents");
    const std::string dts = boson::ipc::TsGenerator::generate_with_events<boson::ipc::MenuEvents, boson::ipc::MenuController>();
    MT_CHECK(dts.find("export interface MenuItemTemplate") != std::string::npos);
    MT_CHECK(dts.find("submenu: Array<MenuItemTemplate>") != std::string::npos);
    MT_CHECK(dts.find("popup_context_menu") != std::string::npos);
    MT_CHECK(dts.find("menu:click") != std::string::npos && dts.find("tray:click") != std::string::npos);
    if (GetEnvironmentVariableW(L"BOSON_MENU_DUMP_DTS", nullptr, 0) != 0) {
        std::cout << dts;
    }
}

// ---- WebView2 section ---------------------------------------------------------------------------------

const std::string kPage =
    "<!doctype html><html><head><title>Menu</title><script>window.__events=[];window.__responses=[];"
    "chrome.webview.addEventListener('message',e=>{const d=e.data;"
    "if(d&&d.type==='event'){window.__events.push(d);}else{window.__responses.push(d);}});</script></head>"
    "<body><input id=i value='hello world'></body></html>";

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

void test_webview() {
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
        std::cout << "SKIP WebView2 section: no runtime installed\n";
        return;
    }
    CoTaskMemFree(version);
    step("WebView2: accelerator hook, edit roles, JS round trip");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path user_data = std::filesystem::path(temp) / (L"boson-menu-test-" + std::to_wstring(GetCurrentProcessId()));
    {
        auto window = make_window(true, L"Boson menu test: webview");
        boson::exec::Scheduler scheduler(2);
        boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
        window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
        boson::exec::TaskRunner runner(scheduler, marshal);
        boson::core::EnvironmentConfig env_cfg;
        env_cfg.user_data_folder = user_data.wstring();
        const auto env = boson::core::Environment::create(env_cfg);
        auto host = std::make_unique<boson::core::WebViewHost>(*window, env);
        host->set_default_asset_lookup(&lookup);
        bool navigated = false;
        boson::core::WebView& webview = host->create(boson::core::WebViewConfig{});
        webview.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { navigated = e.is_success; };
        MT_CHECK(pump_until([&]() { return webview.is_ready() && navigated; }, std::chrono::seconds{30}));

        AcceleratorTable table(*window);
        MT_CHECK(table.attach_webview(webview));
        MT_CHECK(table.attach_webview(webview)); // idempotent
        MT_CHECK(table.webview_count() == 1);
        table.detach_webview(webview);
        MT_CHECK(table.webview_count() == 0);

        // SelectAll role through CDP editing commands.
        MT_CHECK(eval(webview, "i.focus(); i.setSelectionRange(0,0); 1").has_value());
        MT_CHECK(boson::menu::perform_role(MenuRole::SelectAll, boson::menu::RoleContext{.webview = &webview}));
        MT_CHECK(pump_until([&]() { return eval(webview, "i.selectionStart + ',' + i.selectionEnd") == std::optional<std::string>{"\"0,11\""}; }, std::chrono::seconds{5}));
        MT_CHECK(boson::menu::perform_role(MenuRole::ZoomIn, boson::menu::RoleContext{.webview = &webview}));
        MT_CHECK(webview.zoom_factor().value_or(1.0) > 1.05);
        MT_CHECK(boson::menu::perform_role(MenuRole::ResetZoom, boson::menu::RoleContext{.webview = &webview}));

        // JS -> Menu.popup_context_menu -> native popup -> Promise result + "menu:click" event.
        boson::ipc::MenuBackend backend(*window, webview);
        boson::ipc::MenuController controller(backend, marshal);
        boson::ipc::Router<boson::ipc::MenuController> router(controller);
        auto bridge = std::make_unique<boson::ipc::Bridge>(runner, marshal, router);
        bridge->attach(webview);
        backend.set_bridge(bridge.get());
        MenuDriver driver;
        driver.start();
        driver.push_round({VK_DOWN, VK_DOWN, VK_RETURN});
        MT_CHECK(eval(webview, "chrome.webview.postMessage({id:1,method:'Menu.popup_context_menu',params:{items:[{id:'a',label:'A'},{id:'b',label:'B',type:'checkbox'}],x:5,y:5}}); 1").has_value());
        MT_CHECK(pump_until([&]() { return eval(webview, "window.__responses.length") == std::optional<std::string>{"1"}; }, std::chrono::seconds{10}));
        const auto response = eval(webview, "JSON.stringify(window.__responses[0])");
        MT_CHECK(response.has_value() && response->find("selected") != std::string::npos && response->find("\\\"b\\\"") != std::string::npos);
        MT_CHECK(pump_until([&]() { return eval(webview, "window.__events.length") == std::optional<std::string>{"1"}; }, std::chrono::seconds{5}));
        const auto event = eval(webview, "JSON.stringify(window.__events[0])");
        MT_CHECK(event.has_value() && event->find("menu:click") != std::string::npos && event->find("context") != std::string::npos);
        driver.stop();
        backend.set_bridge(nullptr);
        bridge.reset();

        bool shut_down = false;
        env->shutdown([&]() { shut_down = true; });
        pump_until([&]() { return shut_down; }, std::chrono::seconds{10});
        host.reset();
        window.reset();
    }
    std::error_code ec;
    std::filesystem::remove_all(user_data, ec);
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    test_accelerator_parsing();
    test_accelerator_table_headless();
    test_accelerator_table_window();
    test_menu_model();
    {
        auto window = make_window(false, L"Boson menu test: roles + popups");
        test_roles(*window);
        test_popups(*window);
    }
    test_native_menu_bar();
    test_popup_menu_bar();
    test_icons();
    test_tray();
    test_controller_helpers();
    test_controller_rpc();
    test_tsgen();
    test_webview();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " menu checks passed\n";
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
