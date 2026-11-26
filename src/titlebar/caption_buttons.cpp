#include <boson/titlebar/caption_buttons.hpp>

#include <boson/core/result.hpp>

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace boson::titlebar {

namespace {

constexpr wchar_t kClassName[] = L"BosonCaptionButtons";
constexpr wchar_t kKeepOnTopProp[] = L"BosonKeepOnTop";
constexpr UINT kWmInvoke = WM_USER + 0x42;

constexpr Rgba kCloseRed{0xC4, 0x2B, 0x1C, 0xFF};

struct Premul {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

Premul premul(Rgba c, float coverage = 1.0f) noexcept {
    const float a = (static_cast<float>(c.a) / 255.0f) * coverage;
    return Premul{
        static_cast<float>(c.r) / 255.0f * a,
        static_cast<float>(c.g) / 255.0f * a,
        static_cast<float>(c.b) / 255.0f * a,
        a,
    };
}

Premul over(Premul dst, Premul src) noexcept {
    const float k = 1.0f - src.a;
    return Premul{src.r + dst.r * k, src.g + dst.g * k, src.b + dst.b * k, src.a + dst.a * k};
}

std::uint32_t pack(Premul p) noexcept {
    const auto q = [](float v) {
        return static_cast<std::uint32_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
    };
    return (q(p.a) << 24) | (q(p.r) << 16) | (q(p.g) << 8) | q(p.b);
}

Rgba with_alpha(Rgba c, std::uint8_t alpha) noexcept {
    return Rgba{c.r, c.g, c.b, alpha};
}

Rgba scale_alpha(Rgba c, float factor) noexcept {
    return Rgba{c.r, c.g, c.b, static_cast<std::uint8_t>(std::clamp(std::lround(c.a * factor), 0L, 255L))};
}

bool is_light(Rgba c) noexcept {
    return (0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) > 128.0;
}

std::optional<CaptionButton> button_from_hit(WPARAM code) noexcept {
    switch (code) {
    case HTMINBUTTON: return CaptionButton::Minimize;
    case HTMAXBUTTON: return CaptionButton::Maximize;
    case HTCLOSE: return CaptionButton::Close;
    default: return std::nullopt;
    }
}

// Vector glyphs for systems without an icon font; drawn straight into the coverage mask.
void plot(std::vector<std::uint8_t>& mask, int width, int height, int x, int y) noexcept {
    if (x >= 0 && y >= 0 && x < width && y < height) {
        mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = 255;
    }
}

void draw_vector_glyph(std::vector<std::uint8_t>& mask, int width, int height, RECT cell, CaptionButton button,
    bool maximized, UINT dpi) noexcept {
    const int size = MulDiv(10, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    const int stroke = std::max(1, MulDiv(1, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI));
    const int cx = (cell.left + cell.right) / 2;
    const int cy = (cell.top + cell.bottom) / 2;
    const int x0 = cx - size / 2;
    const int y0 = cy - size / 2;
    const auto hline = [&](int x, int y, int len) {
        for (int s = 0; s < stroke; ++s) for (int i = 0; i < len; ++i) plot(mask, width, height, x + i, y + s);
    };
    const auto vline = [&](int x, int y, int len) {
        for (int s = 0; s < stroke; ++s) for (int i = 0; i < len; ++i) plot(mask, width, height, x + s, y + i);
    };
    const auto box = [&](int x, int y, int w, int h) {
        hline(x, y, w);
        hline(x, y + h - stroke, w);
        vline(x, y, h);
        vline(x + w - stroke, y, h);
    };
    switch (button) {
    case CaptionButton::Minimize:
        hline(x0, cy, size);
        break;
    case CaptionButton::Maximize:
        if (maximized) {
            const int inner = size - MulDiv(2, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
            const int off = size - inner;
            box(x0, y0 + off, inner, inner);
            hline(x0 + off, y0, inner);
            vline(x0 + size - stroke, y0, inner);
        } else {
            box(x0, y0, size, size);
        }
        break;
    case CaptionButton::Close:
        for (int i = 0; i < size; ++i) {
            for (int s = 0; s < stroke; ++s) {
                plot(mask, width, height, x0 + i + s, y0 + i);
                plot(mask, width, height, x0 + size - 1 - i + s, y0 + i);
            }
        }
        break;
    }
}

bool register_class(HINSTANCE instance, WNDPROC proc) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "RegisterClassExW(BosonCaptionButtons)");
        return false;
    }
    return true;
}

} // namespace

bool resolve_dark(ColorScheme scheme, const ThemeInfo& theme) noexcept {
    switch (scheme) {
    case ColorScheme::Light: return false;
    case ColorScheme::Dark: return true;
    case ColorScheme::System: break;
    }
    return theme.dark_mode;
}

CaptionPalette make_caption_palette(const TitlebarOverlayOptions& options, const ThemeInfo& theme) noexcept {
    CaptionPalette p{};
    if (theme.high_contrast) {
        // Contrast themes: system colours only, like the native caption (custom colours are ignored).
        const SystemColors& c = theme.colors;
        p.background = c.button_face;
        p.symbol = c.button_text;
        p.inactive_symbol = c.gray_text;
        p.hover_background = c.highlight;
        p.hover_symbol = c.highlight_text;
        p.pressed_background = c.highlight;
        p.pressed_symbol = c.highlight_text;
        p.close_hover_background = c.highlight;
        p.close_hover_symbol = c.highlight_text;
        p.close_pressed_background = c.highlight;
        p.close_pressed_symbol = c.highlight_text;
        return p;
    }
    const bool dark = resolve_dark(options.scheme, theme);
    // Windows 11 / WinUI caption resources: TextFillColorPrimary, SubtleFillColorSecondary / Tertiary,
    // TextFillColorDisabled for inactive windows, and the fixed close-button red.
    p.symbol = options.symbol_color.value_or(dark ? Rgba{0xFF, 0xFF, 0xFF, 0xFF} : Rgba{0x00, 0x00, 0x00, 0xE4});
    p.background = options.color.value_or(Rgba{0, 0, 0, 0});
    const bool light_symbol = is_light(p.symbol);
    const Rgba fill_base = light_symbol ? Rgba{0xFF, 0xFF, 0xFF, 0xFF} : Rgba{0x00, 0x00, 0x00, 0xFF};
    p.hover_background = options.hover_color.value_or(with_alpha(fill_base, light_symbol ? 0x0F : 0x09));
    p.pressed_background = options.hover_color.has_value() ? scale_alpha(*options.hover_color, 0.7f)
                                                           : with_alpha(fill_base, light_symbol ? 0x0A : 0x06);
    p.hover_symbol = p.symbol;
    p.pressed_symbol = scale_alpha(p.symbol, 0.77f);
    p.inactive_symbol = scale_alpha(p.symbol, 0.36f);
    p.close_hover_background = kCloseRed;
    p.close_hover_symbol = Rgba{0xFF, 0xFF, 0xFF, 0xFF};
    p.close_pressed_background = with_alpha(kCloseRed, 0xE6);
    p.close_pressed_symbol = Rgba{0xFF, 0xFF, 0xFF, 0xB3};
    return p;
}

CaptionLayout compute_caption_layout(
    int client_width, UINT dpi, int height_dip, int button_width_dip, bool rtl, bool parent_mirrored) noexcept {
    CaptionLayout l{};
    l.dpi = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
    l.rtl = rtl;
    l.button_width = std::max(1, MulDiv(std::max(1, button_width_dip), static_cast<int>(l.dpi), USER_DEFAULT_SCREEN_DPI));
    l.height = std::max(1, MulDiv(std::max(1, height_dip), static_cast<int>(l.dpi), USER_DEFAULT_SCREEN_DPI));
    l.width = l.button_width * static_cast<int>(kCaptionButtonCount);
    client_width = std::max(client_width, 0);

    const int visual_left = rtl ? 0 : std::max(0, client_width - l.width);
    l.overlay_visual = RECT{visual_left, 0, visual_left + l.width, l.height};
    const int logical_left = parent_mirrored ? std::max(0, client_width - visual_left - l.width) : visual_left;
    l.overlay_logical = RECT{logical_left, 0, logical_left + l.width, l.height};

    for (std::size_t i = 0; i < kCaptionButtonCount; ++i) {
        const int slot = rtl ? static_cast<int>(kCaptionButtonCount - 1 - i) : static_cast<int>(i);
        l.buttons[i] = RECT{slot * l.button_width, 0, (slot + 1) * l.button_width, l.height};
    }
    l.titlebar_area = rtl ? RECT{std::min(l.width, client_width), 0, client_width, l.height}
                          : RECT{0, 0, visual_left, l.height};
    return l;
}

std::optional<CaptionButton> caption_button_at(const CaptionLayout& layout, POINT pt) noexcept {
    for (std::size_t i = 0; i < kCaptionButtonCount; ++i) {
        if (PtInRect(&layout.buttons[i], pt) != FALSE) {
            return static_cast<CaptionButton>(i);
        }
    }
    return std::nullopt;
}

LRESULT caption_hit_test(const CaptionLayout& layout, POINT pt, bool allow_top_resize, int resize_band) noexcept {
    if (pt.x < 0 || pt.y < 0 || pt.x >= layout.width || pt.y >= layout.height) {
        return HTNOWHERE;
    }
    if (allow_top_resize && pt.y < resize_band) {
        const int corner = resize_band * 2;
        if (!layout.rtl && pt.x >= layout.width - corner) return HTTOPRIGHT;
        if (layout.rtl && pt.x < corner) return HTTOPLEFT;
        return HTTOP;
    }
    switch (caption_button_at(layout, pt).value_or(CaptionButton::Close)) {
    case CaptionButton::Minimize: return HTMINBUTTON;
    case CaptionButton::Maximize: return HTMAXBUTTON;
    case CaptionButton::Close: return HTCLOSE;
    }
    return HTNOWHERE;
}

void compose_caption_pixels(
    std::span<std::uint32_t> bgra,
    std::span<const std::uint8_t> coverage,
    int width,
    int height,
    const CaptionLayout& layout,
    const CaptionPalette& palette,
    const CaptionVisualState& state) noexcept {
    const std::size_t count = static_cast<std::size_t>(std::max(width, 0)) * static_cast<std::size_t>(std::max(height, 0));
    if (bgra.size() < count || coverage.size() < count) {
        return;
    }
    const Premul base = premul(palette.background);
    for (std::size_t i = 0; i < kCaptionButtonCount; ++i) {
        const auto kind = static_cast<CaptionButton>(i);
        const bool hovered = state.hovered == kind;
        const bool pressed = state.pressed == kind && hovered;
        const bool hot = hovered && (!state.pressed.has_value() || pressed);
        const bool close = kind == CaptionButton::Close;
        Rgba fill{0, 0, 0, 0};
        Rgba symbol = state.active ? palette.symbol : palette.inactive_symbol;
        if (pressed) {
            fill = close ? palette.close_pressed_background : palette.pressed_background;
            symbol = close ? palette.close_pressed_symbol : palette.pressed_symbol;
        } else if (hot) {
            fill = close ? palette.close_hover_background : palette.hover_background;
            symbol = close ? palette.close_hover_symbol : palette.hover_symbol;
        }
        const Premul cell = over(base, premul(fill));
        const RECT& rc = layout.buttons[i];
        const int x1 = std::min<int>(rc.right, width);
        const int y1 = std::min<int>(rc.bottom, height);
        for (int y = std::max<int>(rc.top, 0); y < y1; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
            for (int x = std::max<int>(rc.left, 0); x < x1; ++x) {
                const std::size_t idx = row + static_cast<std::size_t>(x);
                const std::uint8_t cov = coverage[idx];
                bgra[idx] = cov == 0 ? pack(cell) : pack(over(cell, premul(symbol, static_cast<float>(cov) / 255.0f)));
            }
        }
    }
}

Rgba unpremultiply(std::uint32_t px) noexcept {
    const std::uint8_t a = static_cast<std::uint8_t>(px >> 24);
    if (a == 0) {
        return Rgba{0, 0, 0, 0};
    }
    const auto un = [a](std::uint32_t v) {
        return static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (v * 255 + a / 2) / a));
    };
    return Rgba{un((px >> 16) & 0xFF), un((px >> 8) & 0xFF), un(px & 0xFF), a};
}

// ---- CaptionButtons --------------------------------------------------------------------------------------

CaptionButtons::CaptionButtons(core::Window& window, ThemeWatcher& theme)
    : m_window(window)
    , m_theme(theme)
    , m_dpi(window.dpi()) {
    using core::WindowEventKind;
    const auto on = [this](WindowEventKind kind, core::Window::EventListener fn) {
        m_listeners.push_back(m_window.on(kind, std::move(fn)));
    };
    on(WindowEventKind::Resize, [this](core::WindowEvent&) { update(); });
    on(WindowEventKind::DpiChanged, [this](core::WindowEvent& e) {
        m_dpi = e.dpi == 0 ? USER_DEFAULT_SCREEN_DPI : e.dpi;
        update();
    });
    const auto state_changed = [this](core::WindowEvent&) {
        const bool maximized = m_window.is_maximized();
        if (maximized != m_state.maximized) {
            m_state.maximized = maximized;
            const std::wstring name = localized_caption_name(m_window.hwnd(), CaptionButton::Maximize, maximized);
            if (m_accessibility) {
                m_accessibility->notify_name_changed(CaptionButton::Maximize, m_max_name, name);
            }
            m_max_name = name;
        }
        update();
    };
    on(WindowEventKind::Maximize, state_changed);
    on(WindowEventKind::Restore, state_changed);
    on(WindowEventKind::Focus, [this](core::WindowEvent&) {
        m_state.active = true;
        render();
    });
    on(WindowEventKind::Blur, [this](core::WindowEvent&) {
        m_state.active = false;
        render();
    });
    on(WindowEventKind::Destroyed, [this](core::WindowEvent&) { destroy_window(); });
    m_theme_listener = m_theme.on_changed([this](const ThemeInfo&) {
        m_palette = make_caption_palette(m_options, m_theme.current());
        render();
    });
    m_state.maximized = m_window.is_maximized();
    m_state.active = m_window.hwnd() != nullptr && GetActiveWindow() == m_window.hwnd();
    m_max_name = localized_caption_name(m_window.hwnd(), CaptionButton::Maximize, m_state.maximized);
}

CaptionButtons::~CaptionButtons() {
    m_theme.off(m_theme_listener);
    for (const auto id : m_listeners) {
        m_window.off(id);
    }
    destroy_window();
    if (m_font != nullptr) {
        DeleteObject(m_font);
    }
}

bool CaptionButtons::parent_mirrored() const noexcept {
    return m_window.hwnd() != nullptr && (GetWindowLongPtrW(m_window.hwnd(), GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
}

bool CaptionButtons::is_visible() const noexcept {
    return m_shown && m_hwnd != nullptr && IsWindowVisible(m_hwnd) != FALSE;
}

bool CaptionButtons::show(TitlebarOverlayOptions options) {
    options.height = std::clamp(options.height, 1, 400);
    options.button_width = std::clamp(options.button_width, 1, 400);
    m_options = options;
    m_palette = make_caption_palette(m_options, m_theme.current());
    if (!ensure_window()) {
        return false;
    }
    m_shown = true;
    update();
    return true;
}

void CaptionButtons::hide() {
    m_shown = false;
    if (m_hwnd != nullptr) {
        ShowWindow(m_hwnd, SW_HIDE);
    }
    m_state.hovered.reset();
    m_state.pressed.reset();
    if (m_on_layout) {
        m_on_layout();
    }
}

bool CaptionButtons::ensure_window() {
    if (m_hwnd != nullptr) {
        return true;
    }
    const HWND parent = m_window.hwnd();
    if (parent == nullptr) {
        return false;
    }
    const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    if (!register_class(instance, &CaptionButtons::wnd_proc)) {
        return false;
    }
    // Per-pixel-alpha layered child (Windows 8+, supportedOS manifest). WS_EX_NOINHERITLAYOUT keeps the
    // overlay's own coordinates left-to-right; RTL is handled explicitly by the layout.
    m_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_NOINHERITLAYOUT, kClassName, L"",
        WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, nullptr, instance, this);
    m_layered = m_hwnd != nullptr;
    if (m_hwnd == nullptr) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW(layered caption buttons); falling back to opaque");
        m_hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOINHERITLAYOUT, kClassName, L"",
            WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, nullptr, instance, this);
    }
    if (m_hwnd == nullptr) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW(BosonCaptionButtons)");
        return false;
    }
    SetPropW(m_hwnd, kKeepOnTopProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
    m_accessibility = std::make_unique<CaptionAccessibility>(m_hwnd, CaptionAccessibilitySource{
        .buttons = [this]() { return accessible_buttons(); },
        .invoke = [this](CaptionButton button) { invoke(button); },
        .container_name = [this]() {
            std::array<wchar_t, 256> title{};
            const int len = m_window.hwnd() != nullptr
                ? GetWindowTextW(m_window.hwnd(), title.data(), static_cast<int>(title.size()))
                : 0;
            return std::wstring(title.data(), static_cast<std::size_t>(std::max(len, 0)));
        },
    });
    return true;
}

void CaptionButtons::destroy_window() noexcept {
    if (m_accessibility) {
        m_accessibility->disconnect();
    }
    if (m_hwnd != nullptr) {
        const HWND hwnd = m_hwnd;
        if (IsWindow(hwnd)) {
            RemovePropW(hwnd, kKeepOnTopProp);
            DestroyWindow(hwnd);
        }
        m_hwnd = nullptr;
    }
    m_accessibility.reset();
}

void CaptionButtons::update() {
    if (!m_shown || m_hwnd == nullptr) {
        return;
    }
    relayout();
    render();
}

void CaptionButtons::relayout() {
    const RECT client = m_window.client_rect();
    const bool mirrored = parent_mirrored();
    m_layout = compute_caption_layout(client.right - client.left, m_dpi, m_options.height, m_options.button_width,
        m_options.rtl.value_or(mirrored), mirrored);
    const bool visible = !has_fullscreen_style(m_window.hwnd());
    const RECT& rc = m_layout.overlay_logical;
    SetWindowPos(m_hwnd, HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (m_on_layout) {
        m_on_layout();
    }
}

void CaptionButtons::rebuild_font() {
    if (m_font != nullptr && m_font_dpi == m_dpi) {
        return;
    }
    if (m_font != nullptr) {
        DeleteObject(m_font);
        m_font = nullptr;
    }
    m_font_dpi = m_dpi;
    m_font_face.clear();
    const int px = MulDiv(10, static_cast<int>(m_dpi), USER_DEFAULT_SCREEN_DPI);
    HDC screen = GetDC(nullptr);
    for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
        HFONT font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        if (font == nullptr) {
            continue;
        }
        std::array<wchar_t, LF_FACESIZE> actual{};
        const HGDIOBJ old = SelectObject(screen, font);
        GetTextFaceW(screen, static_cast<int>(actual.size()), actual.data());
        SelectObject(screen, old);
        if (std::wstring_view(actual.data()) == face) {
            m_font = font;
            m_font_face = face;
            break;
        }
        DeleteObject(font); // substituted: the icon font is not installed
    }
    ReleaseDC(nullptr, screen);
}

void CaptionButtons::render() {
    if (!m_shown || m_hwnd == nullptr) {
        return;
    }
    const int width = m_layout.width;
    const int height = m_layout.height;
    if (width <= 0 || height <= 0) {
        return;
    }
    rebuild_font();

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreateDIBSection(caption buttons)");
        if (dib != nullptr) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    const HGDIOBJ old_bitmap = SelectObject(mem, dib);
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    auto* pixels = static_cast<std::uint32_t*>(bits);
    std::memset(pixels, 0, count * sizeof(std::uint32_t));

    // 1. Glyph coverage: white grayscale-antialiased text on black.
    m_coverage.assign(count, 0);
    if (m_font != nullptr) {
        const HGDIOBJ old_font = SelectObject(mem, m_font);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 255));
        for (std::size_t i = 0; i < kCaptionButtonCount; ++i) {
            RECT cell = m_layout.buttons[i];
            const wchar_t glyph = caption_glyph(static_cast<CaptionButton>(i), m_state.maximized);
            DrawTextW(mem, &glyph, 1, &cell, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(mem, old_font);
        GdiFlush();
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t px = pixels[i];
            m_coverage[i] = static_cast<std::uint8_t>(std::max({(px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF}));
        }
    } else {
        for (std::size_t i = 0; i < kCaptionButtonCount; ++i) {
            draw_vector_glyph(m_coverage, width, height, m_layout.buttons[i], static_cast<CaptionButton>(i),
                m_state.maximized, m_dpi);
        }
    }

    // 2. Compose backgrounds, state fills and glyphs into premultiplied BGRA.
    m_pixels.assign(count, 0);
    compose_caption_pixels(m_pixels, m_coverage, width, height, m_layout, m_palette, m_state);
    std::memcpy(pixels, m_pixels.data(), count * sizeof(std::uint32_t));

    if (m_layered) {
        SIZE size{width, height};
        POINT src{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (UpdateLayeredWindow(m_hwnd, screen, nullptr, &size, mem, &src, 0, &blend, ULW_ALPHA) == FALSE) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "UpdateLayeredWindow(caption buttons)");
        }
    } else {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    SelectObject(mem, old_bitmap);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void CaptionButtons::invoke(CaptionButton button) {
    if (m_hwnd != nullptr) {
        PostMessageW(m_hwnd, kWmInvoke, static_cast<WPARAM>(button), 0);
    }
}

void CaptionButtons::set_hovered(std::optional<CaptionButton> hovered) {
    if (m_state.hovered == hovered) {
        return;
    }
    m_state.hovered = hovered;
    render();
}

POINT CaptionButtons::screen_to_local(LPARAM l_param) const noexcept {
    POINT pt{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    ScreenToClient(m_hwnd, &pt);
    return pt;
}

std::vector<AccessibleButton> CaptionButtons::accessible_buttons() const {
    std::vector<AccessibleButton> list;
    if (!is_visible()) {
        return list;
    }
    RECT window_rect{};
    GetWindowRect(m_hwnd, &window_rect);
    for (std::size_t slot = 0; slot < kCaptionButtonCount; ++slot) {
        for (std::size_t i = 0; i < kCaptionButtonCount; ++i) {
            const RECT& rc = m_layout.buttons[i];
            if (rc.left != static_cast<LONG>(slot) * m_layout.button_width) {
                continue;
            }
            const auto kind = static_cast<CaptionButton>(i);
            list.push_back(AccessibleButton{
                .kind = kind,
                .screen_rect = RECT{window_rect.left + rc.left, window_rect.top + rc.top, window_rect.left + rc.right,
                    window_rect.top + rc.bottom},
                .name = localized_caption_name(m_window.hwnd(), kind, m_window.is_maximized()),
                .enabled = true,
            });
        }
    }
    return list;
}

LRESULT CALLBACK CaptionButtons::wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        if (auto* creator = static_cast<CaptionButtons*>(cs->lpCreateParams); creator != nullptr) {
            creator->m_hwnd = hwnd;
        }
    }
    auto* self = reinterpret_cast<CaptionButtons*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr || (self->m_hwnd != nullptr && self->m_hwnd != hwnd)) {
        return DefWindowProcW(hwnd, msg, w_param, l_param);
    }
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self->m_accessibility) {
            self->m_accessibility->disconnect();
        }
        self->m_hwnd = nullptr;
        return DefWindowProcW(hwnd, msg, w_param, l_param);
    }
    return self->handle(msg, w_param, l_param);
}

LRESULT CaptionButtons::handle(UINT msg, WPARAM w_param, LPARAM l_param) {
    const HWND hwnd = m_hwnd;
    const HWND parent = m_window.hwnd();
    switch (msg) {
    case WM_NCHITTEST: {
        const bool resizable = m_window.is_headless() && !m_window.is_maximized() && !m_window.is_fullscreen();
        return caption_hit_test(m_layout, screen_to_local(l_param), resizable,
            GetSystemMetricsForDpi(SM_CYSIZEFRAME, m_dpi));
    }
    case WM_NCMOUSEMOVE: {
        if (GetCapture() != hwnd) {
            set_hovered(button_from_hit(w_param));
        }
        if (!m_tracking) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
            tme.hwndTrack = hwnd;
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        if (w_param == HTMAXBUTTON) {
            break; // DefWindowProc sees the HTMAXBUTTON hover (Windows 11 Snap Layouts)
        }
        return 0;
    }
    case WM_NCMOUSELEAVE:
        m_tracking = false;
        if (GetCapture() != hwnd) {
            set_hovered(std::nullopt);
        }
        return 0;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (is_sizing_hit(static_cast<LRESULT>(w_param))) {
            // The top-level window sizes itself (a double-click on the top edge maximizes vertically).
            ReleaseCapture();
            SendMessageW(parent, msg, w_param, l_param);
            return 0;
        }
        if (const auto button = button_from_hit(w_param); button.has_value()) {
            m_state.pressed = button;
            m_state.hovered = button;
            SetCapture(hwnd);
            render();
        }
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd) {
            const POINT pt{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            set_hovered(caption_button_at(m_layout, pt));
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_NCLBUTTONUP: {
        if (!m_state.pressed.has_value()) {
            return 0;
        }
        const CaptionButton pressed = *m_state.pressed;
        std::optional<CaptionButton> released;
        if (msg == WM_LBUTTONUP) {
            const POINT pt{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            released = caption_button_at(m_layout, pt);
        } else {
            released = button_from_hit(w_param);
        }
        m_state.pressed.reset();
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        if (released == pressed) {
            invoke(pressed);
        } else {
            m_state.hovered = released;
        }
        render();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (m_state.pressed.has_value()) {
            m_state.pressed.reset();
            render();
        }
        return 0;
    case WM_NCRBUTTONDOWN:
        return 0;
    case WM_NCRBUTTONUP:
        // Same as a right-click on the caption: the window menu (Titlebar shows it for headless windows).
        SendMessageW(parent, WM_NCRBUTTONUP, HTCAPTION, l_param);
        return 0;
    case WM_SETCURSOR:
        break; // DefWindowProc picks the sizing or arrow cursor from the hit-test code
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_GETOBJECT:
        if (m_accessibility) {
            if (const auto result = m_accessibility->handle_get_object(w_param, l_param); result.has_value()) {
                return *result;
            }
        }
        break;
    case kWmInvoke:
        switch (static_cast<CaptionButton>(w_param)) {
        case CaptionButton::Minimize: m_window.minimize(); break;
        case CaptionButton::Maximize: m_window.maximize_or_restore(); break;
        case CaptionButton::Close: m_window.close(); break;
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (!m_layered && !m_pixels.empty() && dc != nullptr) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = m_layout.width;
            bi.bmiHeader.biHeight = -m_layout.height;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(m_layout.width), static_cast<DWORD>(m_layout.height), 0, 0, 0,
                static_cast<UINT>(m_layout.height), m_pixels.data(), &bi, DIB_RGB_COLORS);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

} // namespace boson::titlebar
