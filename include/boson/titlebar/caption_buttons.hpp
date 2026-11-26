#pragma once

#include <windows.h>

#include <boson/core/window.hpp>
#include <boson/titlebar/caption_accessibility.hpp>
#include <boson/titlebar/theme.hpp>
#include <boson/titlebar/window_chrome.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace boson::titlebar {

enum class ColorScheme {
    System, ///< Follow the OS apps theme (ThemeInfo::dark_mode).
    Light,
    Dark,
};

/// @brief Native caption buttons drawn by Boson (Electron's titleBarOverlay equivalent).
struct TitlebarOverlayOptions {
    std::optional<Rgba> color;        ///< Background behind the buttons. Default: transparent.
    std::optional<Rgba> symbol_color; ///< Glyph colour. Default: Windows 11 text colour of the scheme.
    std::optional<Rgba> hover_color;  ///< Minimize/maximize hover fill. Default: derived from symbol_color.
    int height = 32;                  ///< DIPs (Windows 11 caption height is 32).
    int button_width = 46;            ///< DIPs per button.
    ColorScheme scheme = ColorScheme::System;
    std::optional<bool> rtl;          ///< Mirror to the top-left. Default: parent has WS_EX_LAYOUTRTL.
};

/// @brief Colours of every button state (straight alpha).
struct CaptionPalette {
    Rgba background;
    Rgba symbol;
    Rgba inactive_symbol;
    Rgba hover_background;
    Rgba hover_symbol;
    Rgba pressed_background;
    Rgba pressed_symbol;
    Rgba close_hover_background;
    Rgba close_hover_symbol;
    Rgba close_pressed_background;
    Rgba close_pressed_symbol;
};

[[nodiscard]] CaptionPalette make_caption_palette(const TitlebarOverlayOptions& options, const ThemeInfo& theme) noexcept;
[[nodiscard]] bool resolve_dark(ColorScheme scheme, const ThemeInfo& theme) noexcept;

/// @brief Geometry of the overlay for one client width / DPI / direction.
struct CaptionLayout {
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    bool rtl = false;
    int button_width = 0; ///< px
    int height = 0;       ///< px
    int width = 0;        ///< px, all buttons
    /// Rect to pass to SetWindowPos (parent client coordinates; mirrored when the parent is RTL).
    RECT overlay_logical{};
    /// Where the overlay appears, in visual (left-to-right) parent client coordinates.
    RECT overlay_visual{};
    /// Per CaptionButton (indexed by its value), overlay-local visual rects.
    std::array<RECT, kCaptionButtonCount> buttons{};
    /// Free titlebar area beside the buttons, visual client px (CSS env(titlebar-area-*) equivalent).
    RECT titlebar_area{};
};

[[nodiscard]] CaptionLayout compute_caption_layout(
    int client_width, UINT dpi, int height_dip, int button_width_dip, bool rtl, bool parent_mirrored) noexcept;
[[nodiscard]] std::optional<CaptionButton> caption_button_at(const CaptionLayout& layout, POINT overlay_local) noexcept;
/// @brief WM_NCHITTEST answer for an overlay-local point: HTMINBUTTON / HTMAXBUTTON / HTCLOSE, or the top
/// resize codes inside the top resize_band px when allow_top_resize (restored window).
[[nodiscard]] LRESULT caption_hit_test(const CaptionLayout& layout, POINT overlay_local, bool allow_top_resize, int resize_band) noexcept;

/// @brief Segoe Fluent Icons / Segoe MDL2 Assets code point: E921, E922 / E923 (restore), E8BB.
[[nodiscard]] constexpr wchar_t caption_glyph(CaptionButton button, bool maximized) noexcept {
    switch (button) {
    case CaptionButton::Minimize: return static_cast<wchar_t>(0xE921);
    case CaptionButton::Maximize: return static_cast<wchar_t>(maximized ? 0xE923 : 0xE922);
    case CaptionButton::Close: return static_cast<wchar_t>(0xE8BB);
    }
    return L' ';
}

struct CaptionVisualState {
    std::optional<CaptionButton> hovered;
    std::optional<CaptionButton> pressed;
    bool active = true;
    bool maximized = false;
};

/// @brief Composes premultiplied BGRA pixels (top-down, width*height) from a glyph coverage mask (0..255,
/// same size) and the palette for the given state. Pure function; used by CaptionButtons::render.
void compose_caption_pixels(
    std::span<std::uint32_t> bgra,
    std::span<const std::uint8_t> coverage,
    int width,
    int height,
    const CaptionLayout& layout,
    const CaptionPalette& palette,
    const CaptionVisualState& state) noexcept;

/// @brief Unpremultiplies one composed pixel (tests / diagnostics).
[[nodiscard]] Rgba unpremultiply(std::uint32_t bgra) noexcept;

/// @brief A per-pixel-alpha layered child window at the top-right (top-left in RTL) of a Window that draws
/// and drives Windows 11 caption buttons, with Snap Layouts on maximize, top-edge resizing, inactive
/// dimming, theme / high-contrast colours and a UI Automation tree. Stays above WebView HWNDs. UI thread;
/// must be destroyed before its Window.
class CaptionButtons {
public:
    using LayoutListener = std::function<void()>;

    CaptionButtons(core::Window& window, ThemeWatcher& theme);
    ~CaptionButtons();

    CaptionButtons(const CaptionButtons&) = delete;
    CaptionButtons& operator=(const CaptionButtons&) = delete;
    CaptionButtons(CaptionButtons&&) = delete;
    CaptionButtons& operator=(CaptionButtons&&) = delete;

    bool show(TitlebarOverlayOptions options);
    void hide();
    [[nodiscard]] bool is_shown() const noexcept { return m_shown; }
    /// @brief Shown and not hidden by fullscreen.
    [[nodiscard]] bool is_visible() const noexcept;

    [[nodiscard]] HWND hwnd() const noexcept { return m_hwnd; }
    [[nodiscard]] const CaptionLayout& layout() const noexcept { return m_layout; }
    [[nodiscard]] const TitlebarOverlayOptions& options() const noexcept { return m_options; }
    [[nodiscard]] const CaptionPalette& palette() const noexcept { return m_palette; }
    [[nodiscard]] const CaptionVisualState& state() const noexcept { return m_state; }
    [[nodiscard]] bool is_layered() const noexcept { return m_layered; }
    /// @brief Last composed frame (premultiplied BGRA, layout().width * layout().height).
    [[nodiscard]] std::span<const std::uint32_t> pixels() const noexcept { return m_pixels; }
    /// @brief Name of the glyph font in use ("Segoe Fluent Icons", "Segoe MDL2 Assets" or empty = vector).
    [[nodiscard]] const std::wstring& glyph_font() const noexcept { return m_font_face; }

    /// @brief Performs a button's action (minimize, maximize/restore, close) asynchronously.
    void invoke(CaptionButton button);
    /// @brief Called after every geometry or visibility change.
    void set_layout_listener(LayoutListener listener) { m_on_layout = std::move(listener); }
    /// @brief Re-reads layout from the window and repaints.
    void update();

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
    LRESULT handle(UINT msg, WPARAM w_param, LPARAM l_param);
    bool ensure_window();
    void destroy_window() noexcept;
    void relayout();
    void render();
    void rebuild_font();
    void set_hovered(std::optional<CaptionButton> hovered);
    [[nodiscard]] POINT screen_to_local(LPARAM l_param) const noexcept;
    [[nodiscard]] bool parent_mirrored() const noexcept;
    [[nodiscard]] std::vector<AccessibleButton> accessible_buttons() const;

    core::Window& m_window;
    ThemeWatcher& m_theme;
    TitlebarOverlayOptions m_options;
    CaptionPalette m_palette{};
    CaptionLayout m_layout{};
    CaptionVisualState m_state{};
    HWND m_hwnd = nullptr;
    bool m_layered = false;
    bool m_shown = false;
    bool m_tracking = false;
    UINT m_dpi = USER_DEFAULT_SCREEN_DPI;
    HFONT m_font = nullptr;
    UINT m_font_dpi = 0;
    std::wstring m_font_face;
    std::vector<std::uint32_t> m_pixels;
    std::vector<std::uint8_t> m_coverage;
    std::vector<core::Window::ListenerId> m_listeners;
    ThemeWatcher::ListenerId m_theme_listener = 0;
    std::unique_ptr<CaptionAccessibility> m_accessibility;
    std::wstring m_max_name;
    LayoutListener m_on_layout;
};

} // namespace boson::titlebar
