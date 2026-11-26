#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace boson::webview {

/// @brief A rectangle in CSS pixels of the host page's viewport (getBoundingClientRect()).
struct CssRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

/// @brief One <boson-webview> element as measured by frontend/src/webview_element.ts in one animation frame.
struct GuestLayoutItem {
    std::uint64_t id = 0;               ///< Guest id (WebView::id()).
    CssRect rect;                       ///< The element's border box.
    std::optional<CssRect> clip;        ///< Visible part after overflow clipping; unset = not clipped.
    bool visible = true;                ///< false for display:none, visibility:hidden, offscreen, hidden tab.
    std::int32_t z = 0;                 ///< Stacking order among the owner's guests (higher = on top).
};

/// @brief The native placement derived from a GuestLayoutItem: physical pixels in the parent window's
/// client area, plus the clip relative to bounds' origin.
struct ElementPlacement {
    RECT bounds{};
    RECT clip{};          ///< Relative to bounds.left/top; meaningful when clipped is true.
    bool clipped = false; ///< clip is smaller than bounds.
    bool visible = false; ///< false also when the visible area is empty.

    friend bool operator==(const ElementPlacement& a, const ElementPlacement& b) noexcept {
        return EqualRect(&a.bounds, &b.bounds) != FALSE && a.clipped == b.clipped && a.visible == b.visible &&
               (!a.clipped || EqualRect(&a.clip, &b.clip) != FALSE);
    }
};

/// @brief Snaps a CSS coordinate to physical pixels (each edge is rounded on its own so that adjacent
/// elements never leave a one-pixel gap).
[[nodiscard]] inline LONG css_to_physical(double css, double device_pixel_ratio) noexcept {
    return static_cast<LONG>(std::lround(css * device_pixel_ratio));
}

/// @brief Converts a measured element box to its native placement. device_pixel_ratio is the host page's
/// window.devicePixelRatio (monitor scale times page zoom); origin is the host WebView's top-left corner in
/// the parent window's client area.
[[nodiscard]] inline ElementPlacement compute_element_placement(
    const CssRect& rect,
    const std::optional<CssRect>& clip,
    double device_pixel_ratio,
    POINT origin,
    bool visible) noexcept {
    const double dpr = (device_pixel_ratio > 0.0 && std::isfinite(device_pixel_ratio)) ? device_pixel_ratio : 1.0;
    ElementPlacement out;
    out.bounds = RECT{
        origin.x + css_to_physical(rect.x, dpr),
        origin.y + css_to_physical(rect.y, dpr),
        origin.x + css_to_physical(rect.x + std::max(0.0, rect.width), dpr),
        origin.y + css_to_physical(rect.y + std::max(0.0, rect.height), dpr),
    };
    const LONG width = out.bounds.right - out.bounds.left;
    const LONG height = out.bounds.bottom - out.bounds.top;
    out.visible = visible && width > 0 && height > 0;
    if (!out.visible) {
        return out;
    }
    if (clip.has_value()) {
        const RECT clip_abs{
            origin.x + css_to_physical(clip->x, dpr),
            origin.y + css_to_physical(clip->y, dpr),
            origin.x + css_to_physical(clip->x + std::max(0.0, clip->width), dpr),
            origin.y + css_to_physical(clip->y + std::max(0.0, clip->height), dpr),
        };
        RECT visible_abs{};
        if (IntersectRect(&visible_abs, &out.bounds, &clip_abs) == FALSE) {
            out.visible = false;
            return out;
        }
        if (EqualRect(&visible_abs, &out.bounds) == FALSE) {
            out.clipped = true;
            out.clip = RECT{
                visible_abs.left - out.bounds.left,
                visible_abs.top - out.bounds.top,
                visible_abs.right - out.bounds.left,
                visible_abs.bottom - out.bounds.top,
            };
        }
    }
    return out;
}

} // namespace boson::webview
