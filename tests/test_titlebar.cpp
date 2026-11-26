// Titlebar workstream tests: frame math, caption-button geometry at several DPIs, RTL, hit testing in every
// mode, rendering, theme tracking, the window menu, UI Automation (IUIAutomation client on a worker thread)
// and, when a WebView2 runtime is installed, the Titlebar controller over the bridge.
#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

using Microsoft::WRL::ComPtr;
using boson::titlebar::CaptionButton;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_titlebar.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define TB_CHECK(...) check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
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

LPARAM screen_lparam(POINT pt) {
    return MAKELPARAM(static_cast<WORD>(static_cast<SHORT>(pt.x)), static_cast<WORD>(static_cast<SHORT>(pt.y)));
}

LRESULT hit(HWND hwnd, POINT screen_pt) {
    return SendMessageW(hwnd, WM_NCHITTEST, 0, screen_lparam(screen_pt));
}

RECT window_rect(HWND hwnd) {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    return rc;
}

RECT client_screen_rect(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    POINT a{rc.left, rc.top};
    POINT b{rc.right, rc.bottom};
    ClientToScreen(hwnd, &a);
    ClientToScreen(hwnd, &b);
    return RECT{std::min(a.x, b.x), a.y, std::max(a.x, b.x), b.y};
}

bool grayed(HMENU menu, UINT id) {
    return (GetMenuState(menu, id, MF_BYCOMMAND) & (MF_GRAYED | MF_DISABLED)) != 0;
}

std::uint8_t max_alpha(std::span<const std::uint32_t> pixels, int width, RECT cell) {
    std::uint8_t best = 0;
    for (int y = cell.top; y < cell.bottom; ++y) {
        for (int x = cell.left; x < cell.right; ++x) {
            best = std::max(best, static_cast<std::uint8_t>(pixels[static_cast<std::size_t>(y * width + x)] >> 24));
        }
    }
    return best;
}

// ---- Pure functions ------------------------------------------------------------------------------------

void test_colors() {
    step("CSS colour parsing");
    using boson::titlebar::parse_css_color;
    using boson::titlebar::Rgba;
    TB_CHECK(parse_css_color("#C42B1C") == std::optional{Rgba{0xC4, 0x2B, 0x1C, 0xFF}});
    TB_CHECK(parse_css_color("#fff") == std::optional{Rgba{255, 255, 255, 255}});
    TB_CHECK(parse_css_color("#0008") == std::optional{Rgba{0, 0, 0, 0x88}});
    TB_CHECK(parse_css_color("#11223344") == std::optional{Rgba{0x11, 0x22, 0x33, 0x44}});
    TB_CHECK(parse_css_color(" rgb(1, 2, 3) ") == std::optional{Rgba{1, 2, 3, 255}});
    TB_CHECK(parse_css_color("rgba(255,0,0,0.5)") == std::optional{Rgba{255, 0, 0, 128}});
    TB_CHECK(parse_css_color("rgb(10 20 30 / 0.25)") == std::optional{Rgba{10, 20, 30, 64}});
    TB_CHECK(parse_css_color("transparent") == std::optional{Rgba{0, 0, 0, 0}});
    TB_CHECK(!parse_css_color("#12345").has_value());
    TB_CHECK(!parse_css_color("blue").has_value());
    TB_CHECK(!parse_css_color("rgb(1,2)").has_value());
    TB_CHECK(!parse_css_color("").has_value());
    TB_CHECK(boson::titlebar::to_css_hex(Rgba{0xC4, 0x2B, 0x1C, 0xFF}) == "#c42b1c");
    TB_CHECK(boson::titlebar::to_css_hex(Rgba{0, 0, 0, 0x80}) == "#00000080");
}

void test_menu_text_and_keys() {
    step("menu strings and system-key classification");
    using boson::titlebar::classify_system_key;
    using boson::titlebar::strip_menu_text;
    using boson::titlebar::SystemKeyAction;
    TB_CHECK(strip_menu_text(L"Mi&nimize") == L"Minimize");
    TB_CHECK(strip_menu_text(L"&Close\tAlt+F4") == L"Close");
    TB_CHECK(strip_menu_text(L"A && B") == L"A & B");
    TB_CHECK(strip_menu_text(L"\x6700\x5927\x5316(&X)") == L"\x6700\x5927\x5316");
    constexpr int kSystemKeyDown = 2;
    constexpr int kKeyDown = 0;
    TB_CHECK(classify_system_key(kSystemKeyDown, VK_SPACE, false) == SystemKeyAction::SystemMenu);
    TB_CHECK(classify_system_key(kSystemKeyDown, VK_F4, false) == SystemKeyAction::Close);
    TB_CHECK(classify_system_key(kSystemKeyDown, VK_F4, true) == SystemKeyAction::None); // auto-repeat
    TB_CHECK(classify_system_key(kKeyDown, VK_SPACE, false) == SystemKeyAction::None);
    TB_CHECK(classify_system_key(kSystemKeyDown, 'A', false) == SystemKeyAction::None);
}

void test_frame_math() {
    step("WM_NCCALCSIZE math: work area clamp, auto-hide taskbar, restored frames");
    using namespace boson::titlebar;
    const RECT monitor{0, 0, 1920, 1080};
    const RECT work{0, 0, 1920, 1032}; // bottom taskbar
    // A maximized window extends by the frame on every side.
    const RECT proposed{-8, -8, 1928, 1040};
    const RECT client = compute_maximized_client_rect(proposed, work, monitor, 8, 8, kAutoHideNone);
    TB_CHECK(EqualRect(&client, &work) != FALSE);
    // Oversized proposal (bigger frame than reported) is still clamped to the work area.
    const RECT big = compute_maximized_client_rect(RECT{-20, -20, 1940, 1100}, work, monitor, 8, 8, kAutoHideNone);
    TB_CHECK(EqualRect(&big, &work) != FALSE);
    // Auto-hide bottom taskbar: work area == monitor; leave a reveal strip.
    const RECT ah = compute_maximized_client_rect(RECT{-8, -8, 1928, 1088}, monitor, monitor, 8, 8, kAutoHideBottom);
    TB_CHECK(ah.bottom == 1080 - kAutoHideTaskbarReveal && ah.top == 0 && ah.left == 0 && ah.right == 1920);
    const RECT ah_top = compute_maximized_client_rect(RECT{-8, -8, 1928, 1088}, monitor, monitor, 8, 8, kAutoHideTop | kAutoHideLeft);
    TB_CHECK(ah_top.top == kAutoHideTaskbarReveal && ah_top.left == kAutoHideTaskbarReveal && ah_top.bottom == 1080);
    // Secondary monitor at negative coordinates.
    const RECT mon2{-1280, 0, 0, 1024};
    const RECT m2 = compute_maximized_client_rect(RECT{-1288, -8, 8, 1032}, mon2, mon2, 8, 8, kAutoHideNone);
    TB_CHECK(EqualRect(&m2, &mon2) != FALSE);
    // Restored.
    const RECT r{100, 100, 900, 700};
    const RECT keep = compute_restored_client_rect(r, 8, 8, true);
    TB_CHECK(keep.left == 108 && keep.right == 892 && keep.top == 100 && keep.bottom == 692);
    const RECT none = compute_restored_client_rect(r, 8, 8, false);
    TB_CHECK(EqualRect(&none, &r) != FALSE);
    TB_CHECK(is_sizing_hit(HTTOP) && is_sizing_hit(HTBOTTOMRIGHT) && !is_sizing_hit(HTCAPTION) && !is_sizing_hit(HTCLOSE));
}

void test_layout() {
    step("caption layout at 100/150/200% and RTL");
    using namespace boson::titlebar;
    for (const auto& [dpi, bw, h] : {std::tuple{96u, 46, 32}, std::tuple{144u, 69, 48}, std::tuple{192u, 92, 64}}) {
        const int client_w = 1000 * static_cast<int>(dpi) / 96;
        const CaptionLayout l = compute_caption_layout(client_w, dpi, 32, 46, false, false);
        TB_CHECK(l.button_width == bw && l.height == h && l.width == 3 * bw);
        TB_CHECK(l.overlay_visual.right == client_w && l.overlay_visual.left == client_w - 3 * bw);
        TB_CHECK(EqualRect(&l.overlay_logical, &l.overlay_visual) != FALSE);
        TB_CHECK(l.buttons[0].left == 0 && l.buttons[1].left == bw && l.buttons[2].left == 2 * bw);
        TB_CHECK(l.titlebar_area.left == 0 && l.titlebar_area.right == client_w - 3 * bw && l.titlebar_area.bottom == h);
        TB_CHECK(caption_hit_test(l, POINT{bw / 2, h / 2}, true, 4) == HTMINBUTTON);
        TB_CHECK(caption_hit_test(l, POINT{bw + bw / 2, h / 2}, true, 4) == HTMAXBUTTON);
        TB_CHECK(caption_hit_test(l, POINT{2 * bw + 5, h / 2}, true, 4) == HTCLOSE);
        TB_CHECK(caption_hit_test(l, POINT{bw / 2, 1}, true, 4) == HTTOP);
        TB_CHECK(caption_hit_test(l, POINT{3 * bw - 2, 1}, true, 4) == HTTOPRIGHT);
        TB_CHECK(caption_hit_test(l, POINT{bw + 3, 1}, false, 4) == HTMAXBUTTON); // maximized: no resize band
        TB_CHECK(caption_hit_test(l, POINT{3 * bw, 1}, true, 4) == HTNOWHERE);

        // RTL in a mirrored parent: visually top-left, Close first; SetWindowPos coordinates are mirrored.
        const CaptionLayout r = compute_caption_layout(client_w, dpi, 32, 46, true, true);
        TB_CHECK(r.overlay_visual.left == 0 && r.overlay_visual.right == 3 * bw);
        TB_CHECK(r.overlay_logical.left == client_w - 3 * bw);
        TB_CHECK(r.buttons[static_cast<int>(CaptionButton::Close)].left == 0);
        TB_CHECK(r.buttons[static_cast<int>(CaptionButton::Minimize)].left == 2 * bw);
        TB_CHECK(r.titlebar_area.left == 3 * bw && r.titlebar_area.right == client_w);
        TB_CHECK(caption_hit_test(r, POINT{5, h / 2}, true, 4) == HTCLOSE);
        TB_CHECK(caption_hit_test(r, POINT{1, 1}, true, 4) == HTTOPLEFT);
        // RTL requested on a non-mirrored parent.
        const CaptionLayout r2 = compute_caption_layout(client_w, dpi, 32, 46, true, false);
        TB_CHECK(r2.overlay_logical.left == 0);
    }
    const CaptionLayout tiny = compute_caption_layout(50, 96, 32, 46, false, false);
    TB_CHECK(tiny.overlay_visual.left == 0 && tiny.titlebar_area.right == 0);
    TB_CHECK(caption_glyph(CaptionButton::Minimize, false) == static_cast<wchar_t>(0xE921));
    TB_CHECK(caption_glyph(CaptionButton::Maximize, false) == static_cast<wchar_t>(0xE922));
    TB_CHECK(caption_glyph(CaptionButton::Maximize, true) == static_cast<wchar_t>(0xE923));
    TB_CHECK(caption_glyph(CaptionButton::Close, false) == static_cast<wchar_t>(0xE8BB));
}

boson::titlebar::ThemeInfo fake_theme(bool dark, bool high_contrast) {
    boson::titlebar::ThemeInfo t;
    t.dark_mode = dark;
    t.high_contrast = high_contrast;
    t.colors.button_face = {1, 2, 3, 255};
    t.colors.button_text = {250, 250, 250, 255};
    t.colors.highlight = {0, 0, 200, 255};
    t.colors.highlight_text = {255, 255, 0, 255};
    t.colors.gray_text = {128, 128, 128, 255};
    return t;
}

void test_palette_and_compose() {
    step("palette (dark / light / high contrast / custom) and pixel composition");
    using namespace boson::titlebar;
    const TitlebarOverlayOptions defaults{};
    const CaptionPalette dark = make_caption_palette(defaults, fake_theme(true, false));
    TB_CHECK(dark.symbol == (Rgba{255, 255, 255, 255}));
    TB_CHECK(dark.background.a == 0);
    TB_CHECK(dark.close_hover_background == (Rgba{0xC4, 0x2B, 0x1C, 0xFF}));
    TB_CHECK(dark.inactive_symbol.a < dark.symbol.a);
    const CaptionPalette light = make_caption_palette(defaults, fake_theme(false, false));
    TB_CHECK(light.symbol.r == 0 && light.hover_background.r == 0 && light.hover_background.a > 0);
    TitlebarOverlayOptions forced{};
    forced.scheme = ColorScheme::Dark;
    TB_CHECK(make_caption_palette(forced, fake_theme(false, false)).symbol.r == 255);
    TitlebarOverlayOptions custom{};
    custom.color = Rgba{10, 20, 30, 255};
    custom.symbol_color = Rgba{200, 0, 0, 255};
    const CaptionPalette cp = make_caption_palette(custom, fake_theme(true, false));
    TB_CHECK(cp.background == (Rgba{10, 20, 30, 255}) && cp.symbol == (Rgba{200, 0, 0, 255}));
    const CaptionPalette hc = make_caption_palette(custom, fake_theme(true, true));
    TB_CHECK(hc.background == (Rgba{1, 2, 3, 255}) && hc.symbol == (Rgba{250, 250, 250, 255}));
    TB_CHECK(hc.close_hover_background == (Rgba{0, 0, 200, 255}) && hc.hover_symbol == (Rgba{255, 255, 0, 255}));
    TB_CHECK(hc.inactive_symbol == (Rgba{128, 128, 128, 255}));

    const CaptionLayout l = compute_caption_layout(300, 96, 32, 46, false, false);
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(l.width * l.height), 0);
    const auto at = [&](int x, int y) { return static_cast<std::size_t>(y * l.width + x); };
    coverage[at(23, 16)] = 255;       // a glyph pixel in Minimize
    coverage[at(46 * 2 + 23, 16)] = 255; // and in Close
    std::vector<std::uint32_t> px(coverage.size(), 0);
    CaptionVisualState state{};
    state.hovered = CaptionButton::Close;
    compose_caption_pixels(px, coverage, l.width, l.height, l, cp, state);
    TB_CHECK(unpremultiply(px[at(2, 2)]) == (Rgba{10, 20, 30, 255}));            // background
    TB_CHECK(unpremultiply(px[at(23, 16)]) == (Rgba{200, 0, 0, 255}));           // symbol
    TB_CHECK(unpremultiply(px[at(46 * 2 + 2, 2)]) == (Rgba{0xC4, 0x2B, 0x1C, 255})); // close hover red
    TB_CHECK(unpremultiply(px[at(46 * 2 + 23, 16)]) == (Rgba{255, 255, 255, 255}));  // white glyph on red
    state.pressed = CaptionButton::Close;
    compose_caption_pixels(px, coverage, l.width, l.height, l, dark, state);
    const Rgba pressed = unpremultiply(px[at(46 * 2 + 2, 2)]);
    TB_CHECK(pressed.r == 0xC4 && pressed.a == 0xE6);
    state = CaptionVisualState{.active = false};
    compose_caption_pixels(px, coverage, l.width, l.height, l, dark, state);
    TB_CHECK((px[at(23, 16)] >> 24) == dark.inactive_symbol.a);
    TB_CHECK((px[at(2, 2)] >> 24) == 0); // transparent background stays transparent
}

// ---- UI Automation client (runs on a worker thread so the UI thread can answer) ------------------------

struct UiaExpectations {
    HWND overlay = nullptr;
    HWND parent = nullptr;
    std::wstring min_name;
    std::wstring max_name;
    std::wstring restore_name;
    std::wstring close_name;
    std::atomic<bool>* close_requested = nullptr;
    bool full = true; // false: single-button HTML overlay
};

struct UiaResult {
    std::vector<std::pair<bool, std::string>> checks;
    std::atomic<bool> done{false};
    void add(bool ok, std::string what) { checks.emplace_back(ok, std::move(what)); }
};

bool wait_for(const std::function<bool()>& cond, std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!cond()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}

std::wstring element_name(IUIAutomationElement* el) {
    BSTR name = nullptr;
    std::wstring out;
    if (el != nullptr && SUCCEEDED(el->get_CurrentName(&name)) && name != nullptr) {
        out.assign(name, SysStringLen(name));
    }
    SysFreeString(name);
    return out;
}

ComPtr<IUIAutomationElement> find_by_id(IUIAutomation* uia, IUIAutomationElement* root, const wchar_t* id) {
    VARIANT v{};
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(id);
    ComPtr<IUIAutomationCondition> cond;
    ComPtr<IUIAutomationElement> found;
    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &cond))) {
        root->FindFirst(TreeScope_Descendants, cond.Get(), &found);
    }
    VariantClear(&v);
    return found;
}

bool invoke(IUIAutomationElement* el) {
    ComPtr<IUIAutomationInvokePattern> pattern;
    if (el == nullptr || FAILED(el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&pattern))) || !pattern) {
        return false;
    }
    return SUCCEEDED(pattern->Invoke());
}

void run_uia_client(const UiaExpectations& ex, UiaResult& out) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IUIAutomation> uia;
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) {
            out.add(false, "CoCreateInstance(CUIAutomation)");
            out.done = true;
            return;
        }
        ComPtr<IUIAutomationElement> root;
        out.add(SUCCEEDED(uia->ElementFromHandle(ex.overlay, &root)) && root, "ElementFromHandle(overlay)");
        if (root) {
            CONTROLTYPEID type = 0;
            root->get_CurrentControlType(&type);
            out.add(type == UIA_TitleBarControlTypeId, "root is a TitleBar");

            VARIANT v{};
            v.vt = VT_I4;
            v.lVal = UIA_ButtonControlTypeId;
            ComPtr<IUIAutomationCondition> is_button;
            ComPtr<IUIAutomationElementArray> buttons;
            int count = 0;
            if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &is_button)) &&
                SUCCEEDED(root->FindAll(TreeScope_Children, is_button.Get(), &buttons)) && buttons) {
                buttons->get_Length(&count);
            }
            out.add(count == (ex.full ? 3 : 1), "button count");

            const auto max_el = find_by_id(uia.Get(), root.Get(), L"Maximize");
            out.add(max_el != nullptr, "find Maximize by AutomationId");
            out.add(element_name(max_el.Get()) == ex.max_name, "Maximize has the localized name");
            if (max_el) {
                BOOL focusable = TRUE;
                max_el->get_CurrentIsKeyboardFocusable(&focusable);
                out.add(focusable == FALSE, "caption buttons are not keyboard focusable");
                RECT bounds{};
                max_el->get_CurrentBoundingRectangle(&bounds);
                RECT overlay_rc{};
                GetWindowRect(ex.overlay, &overlay_rc);
                RECT inter{};
                out.add(IntersectRect(&inter, &bounds, &overlay_rc) != FALSE && EqualRect(&inter, &bounds) != FALSE,
                    "Maximize bounds inside the overlay");
            }
            out.add(invoke(max_el.Get()), "Invoke(Maximize)");
            out.add(wait_for([&]() { return IsZoomed(ex.parent) != FALSE; }), "window maximized via UIA");
            out.add(wait_for([&]() { return element_name(max_el.Get()) == ex.restore_name; }), "name becomes Restore");
            out.add(invoke(max_el.Get()), "Invoke(Restore)");
            out.add(wait_for([&]() { return IsZoomed(ex.parent) == FALSE; }), "window restored via UIA");

            if (ex.full) {
                const auto min_el = find_by_id(uia.Get(), root.Get(), L"Minimize");
                const auto close_el = find_by_id(uia.Get(), root.Get(), L"Close");
                out.add(element_name(min_el.Get()) == ex.min_name, "Minimize has the localized name");
                out.add(element_name(close_el.Get()) == ex.close_name, "Close has the localized name");
                out.add(invoke(min_el.Get()), "Invoke(Minimize)");
                out.add(wait_for([&]() { return IsIconic(ex.parent) != FALSE; }), "window minimized via UIA");
                ShowWindowAsync(ex.parent, SW_SHOWNOACTIVATE);
                out.add(wait_for([&]() { return IsIconic(ex.parent) == FALSE; }), "window shown again");
                out.add(invoke(close_el.Get()), "Invoke(Close)");
                out.add(wait_for([&]() { return ex.close_requested != nullptr && ex.close_requested->load(); }),
                    "Close invoked -> CloseRequested");
            }
        }
    }
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    out.done = true;
}

void run_uia(const UiaExpectations& ex, std::string_view label) {
    UiaResult result;
    std::thread worker([&]() { run_uia_client(ex, result); });
    pump_until([&]() { return result.done.load(); }, std::chrono::seconds{60});
    worker.join();
    for (const auto& [ok, what] : result.checks) {
        ++g_checks;
        if (!ok) {
            ++g_failures;
            std::cerr << "[FAIL] UIA (" << label << "): " << what << '\n';
        }
    }
    std::cout << "  UIA " << label << ": " << result.checks.size() << " checks\n";
}

// ---- Window tests --------------------------------------------------------------------------------------

void test_window(HINSTANCE) {
    boson::core::WindowConfig cfg;
    cfg.title = L"Boson titlebar test";
    cfg.width = 820;
    cfg.height = 520;
    cfg.headless_titlebar = true;
    cfg.titlebar_height = 32;
    cfg.quit_on_destroy = false;
    auto window = std::make_unique<boson::core::Window>(cfg);
    TB_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));
    const HWND hwnd = window->hwnd();
    pump_for(std::chrono::milliseconds{100});
    const UINT real_dpi = window->dpi();
    std::cout << "  monitor DPI " << real_dpi << '\n';

    // -- Headless parent hit testing and frames.
    step("headless frame: native resize borders and hit testing");
    {
        const RECT wr = window_rect(hwnd);
        const RECT cr = client_screen_rect(hwnd);
        const SIZE frame = boson::titlebar::resize_frame_size(real_dpi);
        TB_CHECK(cr.left - wr.left == frame.cx && wr.right - cr.right == frame.cx);
        TB_CHECK(cr.top == wr.top && wr.bottom - cr.bottom == frame.cy);
        const int mid_x = (wr.left + wr.right) / 2;
        const int mid_y = (wr.top + wr.bottom) / 2;
        TB_CHECK(hit(hwnd, POINT{wr.left + 1, mid_y}) == HTLEFT);
        TB_CHECK(hit(hwnd, POINT{wr.right - 2, mid_y}) == HTRIGHT);
        TB_CHECK(hit(hwnd, POINT{mid_x, wr.bottom - 2}) == HTBOTTOM);
        TB_CHECK(hit(hwnd, POINT{mid_x, wr.top + 1}) == HTTOP);
        TB_CHECK(hit(hwnd, POINT{wr.left + 1, wr.top + 1}) == HTTOPLEFT);
        TB_CHECK(hit(hwnd, POINT{wr.right - 2, wr.bottom - 2}) == HTBOTTOMRIGHT);
        TB_CHECK(hit(hwnd, POINT{mid_x, wr.top + window->dip_to_px(20)}) == HTCAPTION);
        TB_CHECK(hit(hwnd, POINT{mid_x, wr.top + window->dip_to_px(100)}) == HTCLIENT);
    }

    auto titlebar = std::make_unique<boson::titlebar::Titlebar>(*window);
    std::vector<POINT> menu_points;
    UINT menu_reply = 0;
    titlebar->set_menu_tracker([&](HWND, HMENU menu, UINT, POINT pt) {
        menu_points.push_back(pt);
        TB_CHECK(menu != nullptr);
        return menu_reply;
    });
    std::atomic<bool> close_requested{false};
    window->on(boson::core::WindowEventKind::CloseRequested, [&](boson::core::WindowEvent& e) {
        close_requested = true;
        e.cancel = true;
    });

    step("top resize grip above the content");
    {
        const HWND grip = titlebar->top_grip_hwnd();
        TB_CHECK(grip != nullptr && IsWindowVisible(grip));
        const RECT gr = window_rect(grip);
        const RECT cr = client_screen_rect(hwnd);
        TB_CHECK(gr.top == cr.top && gr.left == cr.left && gr.right == cr.right);
        TB_CHECK(gr.bottom - gr.top == GetSystemMetricsForDpi(SM_CYSIZEFRAME, real_dpi));
        TB_CHECK(hit(grip, POINT{(gr.left + gr.right) / 2, gr.top}) == HTTOP);
        const RECT wr = window_rect(hwnd);
        TB_CHECK(hit(grip, POINT{wr.left + 1, gr.top}) == HTTOPLEFT);
        TB_CHECK(hit(grip, POINT{wr.right - 2, gr.top}) == HTTOPRIGHT);
    }

    step("maximized headless window fills exactly the work area");
    {
        window->maximize_or_restore();
        TB_CHECK(pump_until([&]() { return window->is_maximized(); }));
        pump_for(std::chrono::milliseconds{100});
        MONITORINFO mi{};
        mi.cbSize = sizeof(MONITORINFO);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT cr = client_screen_rect(hwnd);
        RECT inter{};
        TB_CHECK(IntersectRect(&inter, &cr, &mi.rcWork) != FALSE && EqualRect(&inter, &cr) != FALSE);
        const unsigned auto_hide = boson::titlebar::query_auto_hide_edges(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
        if (auto_hide == boson::titlebar::kAutoHideNone) {
            TB_CHECK(EqualRect(&cr, &mi.rcWork) != FALSE);
        }
        TB_CHECK(!IsWindowVisible(titlebar->top_grip_hwnd()));
        TB_CHECK(hit(hwnd, POINT{(cr.left + cr.right) / 2, cr.top + 1}) != HTTOP); // no top resize when maximized
        HMENU menu = GetSystemMenu(hwnd, FALSE);
        boson::titlebar::update_system_menu_state(hwnd, menu);
        TB_CHECK(grayed(menu, SC_MOVE) && grayed(menu, SC_SIZE) && grayed(menu, SC_MAXIMIZE));
        TB_CHECK(!grayed(menu, SC_RESTORE) && !grayed(menu, SC_MINIMIZE) && !grayed(menu, SC_CLOSE));
        SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        TB_CHECK(pump_until([&]() { return !window->is_maximized(); }));
        boson::titlebar::update_system_menu_state(hwnd, menu);
        TB_CHECK(!grayed(menu, SC_MOVE) && !grayed(menu, SC_SIZE) && !grayed(menu, SC_MAXIMIZE) && grayed(menu, SC_RESTORE));
        TB_CHECK(IsWindowVisible(titlebar->top_grip_hwnd()));
    }

    step("double-click on the drag region maximizes; system menu commands");
    {
        const RECT wr = window_rect(hwnd);
        const POINT caption{(wr.left + wr.right) / 2, wr.top + window->dip_to_px(16)};
        TB_CHECK(hit(hwnd, caption) == HTCAPTION);
        SendMessageW(hwnd, WM_NCLBUTTONDBLCLK, HTCAPTION, screen_lparam(caption));
        TB_CHECK(pump_until([&]() { return window->is_maximized(); }));
        SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        TB_CHECK(pump_until([&]() { return !window->is_maximized(); }));
        SendMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        TB_CHECK(pump_until([&]() { return window->is_minimized(); }));
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        TB_CHECK(pump_until([&]() { return !window->is_minimized(); }));
        TB_CHECK(!boson::titlebar::localized_caption_name(hwnd, CaptionButton::Close, false).empty());
    }

    step("right-click on caption / Alt+Space / Alt+F4 show the window menu or close");
    {
        const RECT wr = window_rect(hwnd);
        const POINT caption{(wr.left + wr.right) / 2, wr.top + window->dip_to_px(16)};
        menu_reply = SC_MAXIMIZE;
        SendMessageW(hwnd, WM_NCRBUTTONDOWN, HTCAPTION, screen_lparam(caption));
        SendMessageW(hwnd, WM_NCRBUTTONUP, HTCAPTION, screen_lparam(caption));
        TB_CHECK(menu_points.size() == 1 && menu_points.back().x == caption.x && menu_points.back().y == caption.y);
        TB_CHECK(pump_until([&]() { return window->is_maximized(); })); // chosen command was executed
        menu_reply = SC_RESTORE;
        SendMessageW(hwnd, WM_NCRBUTTONUP, HTCAPTION, screen_lparam(caption));
        TB_CHECK(pump_until([&]() { return !window->is_maximized(); }));
        menu_reply = 0;
        SendMessageW(hwnd, WM_SYSCOMMAND, SC_KEYMENU, VK_SPACE);
        TB_CHECK(menu_points.size() == 3);
        POINT expected{0, window->dip_to_px(32)};
        ClientToScreen(hwnd, &expected);
        TB_CHECK(!menu_points.empty() && menu_points.back().x == expected.x && menu_points.back().y == expected.y);
        const UINT key_msg = RegisterWindowMessageW(L"Boson.Titlebar.SystemKey");
        PostMessageW(hwnd, key_msg, static_cast<WPARAM>(boson::titlebar::SystemKeyAction::SystemMenu), 0);
        TB_CHECK(pump_until([&]() { return menu_points.size() == 4; }));
        PostMessageW(hwnd, key_msg, static_cast<WPARAM>(boson::titlebar::SystemKeyAction::Close), 0);
        TB_CHECK(pump_until([&]() { return close_requested.load(); }));
        close_requested = false;
        TB_CHECK(!titlebar->handle_system_key(boson::titlebar::SystemKeyAction::None));
    }

    // -- Native caption buttons.
    step("native caption buttons: placement, hit testing, rendering");
    std::vector<boson::titlebar::TitlebarOverlayInfo> infos;
    titlebar->on_overlay_changed([&](const boson::titlebar::TitlebarOverlayInfo& info) { infos.push_back(info); });
    TB_CHECK(titlebar->set_overlay(boson::titlebar::TitlebarOverlayOptions{}));
    auto& buttons = titlebar->caption_buttons();
    const HWND overlay = buttons.hwnd();
    TB_CHECK(overlay != nullptr && buttons.is_visible());
    std::wcout << L"  glyph font: " << (buttons.glyph_font().empty() ? L"(vector fallback)" : buttons.glyph_font())
               << L", layered: " << buttons.is_layered() << L'\n';
    {
        const RECT orc = window_rect(overlay);
        const RECT cr = client_screen_rect(hwnd);
        const int bw = MulDiv(46, static_cast<int>(real_dpi), 96);
        TB_CHECK(orc.right == cr.right && orc.top == cr.top && orc.right - orc.left == 3 * bw);
        TB_CHECK(orc.bottom - orc.top == MulDiv(32, static_cast<int>(real_dpi), 96));
        const int cy = (orc.top + orc.bottom) / 2;
        TB_CHECK(hit(overlay, POINT{orc.left + bw / 2, cy}) == HTMINBUTTON);
        TB_CHECK(hit(overlay, POINT{orc.left + bw + bw / 2, cy}) == HTMAXBUTTON);
        TB_CHECK(hit(overlay, POINT{orc.left + 2 * bw + bw / 2, cy}) == HTCLOSE);
        TB_CHECK(hit(overlay, POINT{orc.left + bw / 2, orc.top}) == HTTOP);
        TB_CHECK(hit(overlay, POINT{orc.right - 1, orc.top}) == HTTOPRIGHT);
        TB_CHECK(!infos.empty() && infos.back().visible);
        if (!infos.empty()) {
            const auto& info = infos.back();
            TB_CHECK(std::abs(info.buttons_width - 138.0) < 1.0 && std::abs(info.buttons_height - 32.0) < 1.0);
            TB_CHECK(std::abs(info.x) < 0.01 && std::abs(info.width + info.buttons_width - window->px_to_dip(cr.right - cr.left)) < 1.5);
        }
        // Pixels: glyphs drawn, close hover red, inactive dimming.
        const auto& layout = buttons.layout();
        const RECT min_cell = layout.buttons[0];
        const std::uint8_t active_alpha = max_alpha(buttons.pixels(), layout.width, min_cell);
        TB_CHECK(active_alpha > 128);
        const POINT close_pt{orc.left + 2 * bw + bw / 2, cy};
        SendMessageW(overlay, WM_NCMOUSEMOVE, HTCLOSE, screen_lparam(close_pt));
        TB_CHECK(buttons.state().hovered == std::optional{CaptionButton::Close});
        const auto close_corner = buttons.pixels()[static_cast<std::size_t>(2 * layout.width + layout.buttons[2].left + 2)];
        TB_CHECK(boson::titlebar::unpremultiply(close_corner) == (boson::titlebar::Rgba{0xC4, 0x2B, 0x1C, 0xFF}));
        SendMessageW(overlay, WM_NCMOUSELEAVE, 0, 0);
        TB_CHECK(!buttons.state().hovered.has_value());
        SendMessageW(hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
        TB_CHECK(!buttons.state().active);
        const std::uint8_t inactive_alpha = max_alpha(buttons.pixels(), layout.width, min_cell);
        TB_CHECK(inactive_alpha < active_alpha && inactive_alpha > 0);
        SendMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        TB_CHECK(buttons.state().active);

        // Press + release on maximize performs the action; the glyph switches to Restore.
        const POINT max_pt{orc.left + bw + bw / 2, cy};
        SendMessageW(overlay, WM_NCLBUTTONDOWN, HTMAXBUTTON, screen_lparam(max_pt));
        TB_CHECK(buttons.state().pressed == std::optional{CaptionButton::Maximize});
        SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(bw + bw / 2, (orc.bottom - orc.top) / 2));
        TB_CHECK(pump_until([&]() { return window->is_maximized(); }));
        TB_CHECK(buttons.state().maximized && !buttons.state().pressed.has_value());
        {
            const RECT m_orc = window_rect(overlay);
            const RECT m_cr = client_screen_rect(hwnd);
            TB_CHECK(m_orc.right == m_cr.right && m_orc.top == m_cr.top);
            TB_CHECK(hit(overlay, POINT{m_orc.left + bw + bw / 2, m_orc.top}) == HTMAXBUTTON); // no resize band
        }
        // Pressing then releasing elsewhere cancels.
        SendMessageW(overlay, WM_NCLBUTTONDOWN, HTMINBUTTON, screen_lparam(POINT{orc.left + 2, cy}));
        SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(2 * bw + 3, 3));
        pump_for(std::chrono::milliseconds{100});
        TB_CHECK(!window->is_minimized());
        window->maximize_or_restore();
        TB_CHECK(pump_until([&]() { return !window->is_maximized(); }));
        // Right-click on a caption button shows the window menu.
        const std::size_t before = menu_points.size();
        SendMessageW(overlay, WM_NCRBUTTONUP, HTCLOSE, screen_lparam(close_pt));
        TB_CHECK(menu_points.size() == before + 1);
    }

    step("simulated WM_DPICHANGED: 150% and 200%");
    {
        for (const UINT dpi : {144u, 192u, real_dpi}) {
            RECT bounds = window_rect(hwnd);
            SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&bounds));
            const auto& layout = buttons.layout();
            const int bw = MulDiv(46, static_cast<int>(dpi), 96);
            TB_CHECK(layout.dpi == dpi && layout.button_width == bw && layout.height == MulDiv(32, static_cast<int>(dpi), 96));
            const RECT orc = window_rect(overlay);
            TB_CHECK(orc.right - orc.left == 3 * bw && orc.bottom - orc.top == layout.height);
            TB_CHECK(orc.right == client_screen_rect(hwnd).right);
            const RECT gr = window_rect(titlebar->top_grip_hwnd());
            TB_CHECK(gr.bottom - gr.top == GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi));
            TB_CHECK(!infos.empty() && infos.back().dpi == dpi && std::abs(infos.back().buttons_width - 138.0) < 1.0);
            TB_CHECK(hit(overlay, POINT{orc.left + 2 * bw + bw / 2, (orc.top + orc.bottom) / 2}) == HTCLOSE);
            TB_CHECK(max_alpha(buttons.pixels(), layout.width, layout.buttons[2]) > 128);
        }
    }

    step("RTL mirroring");
    {
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYOUTRTL);
        buttons.update();
        const RECT orc = window_rect(overlay);
        const RECT cr = client_screen_rect(hwnd);
        const int bw = buttons.layout().button_width;
        TB_CHECK(buttons.layout().rtl);
        TB_CHECK(orc.left == cr.left && orc.right - orc.left == 3 * bw);
        const int cy = (orc.top + orc.bottom) / 2;
        TB_CHECK(hit(overlay, POINT{orc.left + bw / 2, cy}) == HTCLOSE);
        TB_CHECK(hit(overlay, POINT{orc.left + 2 * bw + bw / 2, cy}) == HTMINBUTTON);
        TB_CHECK(hit(overlay, POINT{orc.left, orc.top}) == HTTOPLEFT);
        TB_CHECK(!infos.empty() && infos.back().rtl && std::abs(infos.back().x - 138.0) < 1.0);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
        buttons.update();
        TB_CHECK(window_rect(overlay).right == client_screen_rect(hwnd).right);
    }

    step("theme changes: WM_SETTINGCHANGE re-reads, high contrast palette");
    {
        int theme_events = 0;
        titlebar->theme_watcher().on_changed([&](const boson::titlebar::ThemeInfo&) { ++theme_events; });
        titlebar->theme_watcher().set_query([]() { return fake_theme(true, true); });
        SendMessageW(hwnd, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
        TB_CHECK(pump_until([&]() { return theme_events == 1; }, std::chrono::seconds{3}));
        TB_CHECK(titlebar->theme().high_contrast);
        TB_CHECK(buttons.palette().background == (boson::titlebar::Rgba{1, 2, 3, 255}));
        SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
        SendMessageW(hwnd, WM_SYSCOLORCHANGE, 0, 0);
        pump_for(std::chrono::milliseconds{200});
        TB_CHECK(theme_events == 1); // unchanged settings are not re-announced
        titlebar->theme_watcher().set_query({});
        titlebar->theme_watcher().refresh(true);
        TB_CHECK(theme_events == 2);
        const auto real = boson::titlebar::query_theme();
        std::cout << "  OS theme: dark=" << real.dark_mode << " high_contrast=" << real.high_contrast
                  << " accent=" << boson::titlebar::to_css_hex(real.accent) << '\n';
        TB_CHECK(real.accent.a == 255);
    }

    step("fullscreen hides the overlay");
    {
        window->set_fullscreen(true);
        pump_for(std::chrono::milliseconds{100});
        TB_CHECK(!buttons.is_visible());
        TB_CHECK(!titlebar->overlay_info().visible);
        window->set_fullscreen(false);
        pump_for(std::chrono::milliseconds{100});
        TB_CHECK(buttons.is_visible());
    }

    step("UI Automation: native caption buttons");
    {
        UiaExpectations ex;
        ex.overlay = overlay;
        ex.parent = hwnd;
        ex.min_name = boson::titlebar::localized_caption_name(hwnd, CaptionButton::Minimize, false);
        ex.max_name = boson::titlebar::localized_caption_name(hwnd, CaptionButton::Maximize, false);
        ex.restore_name = boson::titlebar::localized_caption_name(hwnd, CaptionButton::Maximize, true);
        ex.close_name = boson::titlebar::localized_caption_name(hwnd, CaptionButton::Close, false);
        ex.close_requested = &close_requested;
        std::wcout << L"  localized names: " << ex.min_name << L" / " << ex.max_name << L" / " << ex.restore_name << L" / "
                   << ex.close_name << L'\n';
        run_uia(ex, "native buttons");
    }

    step("hide overlay");
    TB_CHECK(titlebar->set_overlay(std::nullopt));
    TB_CHECK(!buttons.is_visible() && !titlebar->overlay_info().visible);

    titlebar.reset();
    window.reset();
}

void test_html_overlay_and_native_frame() {
    step("HTML-button mode: BosonCaptionOverlay is a UIA Button; native frame hit testing unchanged");
    boson::core::WindowConfig cfg;
    cfg.title = L"Boson html overlay test";
    cfg.width = 600;
    cfg.height = 400;
    cfg.quit_on_destroy = false;
    auto window = std::make_unique<boson::core::Window>(cfg);
    TB_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));
    const HWND hwnd = window->hwnd();
    {
        // Native frame: DefWindowProc results.
        const RECT wr = window_rect(hwnd);
        const RECT cr = client_screen_rect(hwnd);
        TB_CHECK(hit(hwnd, POINT{(cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2}) == HTCLIENT);
        TB_CHECK(hit(hwnd, POINT{(wr.left + wr.right) / 2, (wr.top + cr.top) / 2}) == HTCAPTION);
    }
    const RECT client = window->client_rect();
    const RECT max_rect{client.right - window->dip_to_px(92), 0, client.right - window->dip_to_px(46), window->dip_to_px(32)};
    window->set_maximize_button_rect(max_rect);
    const auto overlay = static_cast<HWND>(GetPropW(hwnd, L"BosonCaptionOverlay"));
    TB_CHECK(overlay != nullptr);
    if (overlay != nullptr) {
        UiaExpectations ex;
        ex.overlay = overlay;
        ex.parent = hwnd;
        ex.max_name = boson::titlebar::localized_caption_name(hwnd, CaptionButton::Maximize, false);
        ex.restore_name = boson::titlebar::localized_caption_name(hwnd, CaptionButton::Maximize, true);
        ex.full = false;
        run_uia(ex, "HTML maximize overlay");
    }
    window->set_maximize_button_rect(std::nullopt);
    window.reset();
}

// ---- WebView2: controller over the bridge, right-click on a CSS drag region --------------------------

const std::string kDragPage =
    "<!doctype html><html><head><title>Drag</title><style>html,body{margin:0;height:100%}"
    "#bar{app-region:drag;-webkit-app-region:drag;position:fixed;left:0;top:0;width:100%;height:60px;background:#333}</style>"
    "<script>window.__events=[];window.__responses=[];chrome.webview.addEventListener('message',e=>{const d=e.data;"
    "if(d&&d.type==='event'){window.__events.push(d);}else{window.__responses.push(d);}});</script>"
    "</head><body><div id=bar></div></body></html>";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/drag.html") {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(kDragPage.data()), kDragPage.size());
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
    pump_until([&]() { return done; });
    return result;
}

bool contains(const std::optional<std::string>& s, std::string_view needle) {
    return s.has_value() && s->find(needle) != std::string::npos;
}

void test_webview() {
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
        std::cout << "SKIP: no WebView2 runtime installed (controller / drag-region tests)\n";
        return;
    }
    CoTaskMemFree(version);
    step("WebView2: Titlebar controller over the bridge, right-click on CSS app-region drag");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path user_data = std::filesystem::path(temp) / (L"boson-titlebar-" + std::to_wstring(GetCurrentProcessId()));

    boson::core::WindowConfig cfg;
    cfg.title = L"Boson titlebar WebView test";
    cfg.width = 700;
    cfg.height = 480;
    cfg.headless_titlebar = true;
    cfg.quit_on_destroy = false;
    auto window = std::make_unique<boson::core::Window>(cfg);
    TB_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
    window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = user_data.wstring();
    env_cfg.enable_features = {L"msWebView2EnableDraggableRegions"};
    const auto env = boson::core::Environment::create(env_cfg);
    {
        auto host = std::make_unique<boson::core::WebViewHost>(*window, env, boson::core::HostingMode::Composition);
        host->set_default_asset_lookup(&lookup);
        bool nav = false;
        boson::core::WebViewConfig wv_cfg;
        wv_cfg.initial_url = "boson://app/drag.html";
        wv_cfg.settings.non_client_region_support_enabled = true;
        boson::core::WebView& wv = host->create(wv_cfg);
        wv.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { nav = e.is_success; };
        TB_CHECK(pump_until([&]() { return wv.is_ready() && nav; }, std::chrono::seconds{30}));

        auto titlebar = std::make_unique<boson::titlebar::Titlebar>(*window);
        std::vector<POINT> menu_points;
        titlebar->set_menu_tracker([&](HWND, HMENU, UINT, POINT pt) {
            menu_points.push_back(pt);
            return 0u;
        });
        TB_CHECK(titlebar->attach_webview(wv));
        TB_CHECK(titlebar->attach_webview(wv)); // idempotent

        // Right-click on the CSS drag region (composition hosting maps it to HTCAPTION).
        const bool caption = pump_until([&]() {
            return wv.non_client_region_at(POINT{200, 30}) == std::optional{COREWEBVIEW2_NON_CLIENT_REGION_KIND_CAPTION};
        });
        TB_CHECK(caption);
        POINT drag{200, 30};
        ClientToScreen(window->hwnd(), &drag);
        TB_CHECK(hit(window->hwnd(), drag) == HTCAPTION);
        SendMessageW(window->hwnd(), WM_NCRBUTTONUP, HTCAPTION, screen_lparam(drag));
        TB_CHECK(menu_points.size() == 1);

        boson::ipc::TitlebarController controller(*window, wv, marshal, *titlebar);
        boson::ipc::Router<boson::ipc::TitlebarController> router(controller);
        auto bridge = std::make_unique<boson::ipc::Bridge>(runner, marshal, router);
        bridge->attach(wv);
        boson::ipc::connect_titlebar_events(*titlebar, *bridge);

        const auto call = [&](int id, std::string_view method, std::string_view params) {
            const std::string js = "chrome.webview.postMessage({id:" + std::to_string(id) + ",method:'" + std::string(method) +
                                   "',params:" + std::string(params) + "}); 1";
            eval(wv, js);
            const std::string probe = "JSON.stringify(window.__responses.find(r => r.id === " + std::to_string(id) + ") || null)";
            std::optional<std::string> response;
            pump_until([&]() {
                response = eval(wv, probe);
                return response.has_value() && *response != "\"null\"";
            });
            return response;
        };
        const auto overlay0 = call(1, "Titlebar.get_overlay", "{}");
        TB_CHECK(contains(overlay0, "success") && contains(overlay0, "visible\\\":false"));
        const auto overlay1 = call(2, "Titlebar.set_overlay", "{color:'#202020',symbol_color:'#ffffff',height:40}");
        TB_CHECK(contains(overlay1, "success") && contains(overlay1, "visible\\\":true") && contains(overlay1, "height\\\":40"));
        TB_CHECK(titlebar->caption_buttons().is_visible());
        TB_CHECK(pump_until([&]() {
            return contains(eval(wv, "JSON.stringify(window.__events.filter(e => e.event === 'titlebar:overlay').length > 0)"), "true");
        }));
        const auto theme = call(3, "Titlebar.get_theme", "{}");
        TB_CHECK(contains(theme, "success") && contains(theme, "accent") && contains(theme, "high_contrast"));
        const auto bad = call(4, "Titlebar.set_overlay", "{color:'not-a-colour'}");
        TB_CHECK(contains(bad, "error") && contains(bad, "Invalid CSS colour"));
        const auto menu = call(5, "Titlebar.show_system_menu", "{x:10,y:10}");
        TB_CHECK(contains(menu, "success"));
        TB_CHECK(pump_until([&]() { return menu_points.size() == 2; }));
        titlebar->theme_watcher().refresh(true);
        TB_CHECK(pump_until([&]() {
            return contains(eval(wv, "JSON.stringify(window.__events.filter(e => e.event === 'theme:changed').length > 0)"), "true");
        }));
        const auto hidden = call(6, "Titlebar.set_overlay", "{enabled:false}");
        TB_CHECK(contains(hidden, "visible\\\":false"));

        bridge.reset();
        titlebar.reset();
        bool shut_down = false;
        env->shutdown([&]() { shut_down = true; });
        TB_CHECK(pump_until([&]() { return shut_down; }, std::chrono::seconds{15}));
        host.reset();
    }
    window.reset();
    std::error_code ec;
    for (int i = 0; i < 20 && std::filesystem::exists(user_data); ++i) {
        std::filesystem::remove_all(user_data, ec);
        if (std::filesystem::exists(user_data)) std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    test_colors();
    test_menu_text_and_keys();
    test_frame_math();
    test_layout();
    test_palette_and_compose();
    test_window(GetModuleHandleW(nullptr));
    test_html_overlay_and_native_frame();
    test_webview();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " titlebar checks passed\n";
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
