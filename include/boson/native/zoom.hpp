#pragma once

#include <array>
#include <cmath>

namespace boson::native {

/// Chromium's preset zoom levels (Ctrl+Plus / Ctrl+Minus steps), clamped to WebView2's 0.25 .. 5.0 range.
inline constexpr std::array<double, 17> kZoomLevels{
    0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0};
inline constexpr double kMinZoom = 0.25;
inline constexpr double kMaxZoom = 5.0;

/// @brief The next preset level above (direction > 0) or below (direction < 0) current; current itself
/// (clamped) when direction == 0 or there is no further level.
[[nodiscard]] constexpr double next_zoom_level(double current, int direction) noexcept {
    constexpr double epsilon = 1e-3;
    const double clamped = current < kMinZoom ? kMinZoom : (current > kMaxZoom ? kMaxZoom : current);
    if (direction > 0) {
        for (const double level : kZoomLevels) {
            if (level > clamped + epsilon) {
                return level;
            }
        }
    } else if (direction < 0) {
        for (auto it = kZoomLevels.rbegin(); it != kZoomLevels.rend(); ++it) {
            if (*it < clamped - epsilon) {
                return *it;
            }
        }
    }
    return clamped;
}

/// @brief Chromium zoom level (log base 1.2) <-> factor, as used by Electron's webContents.setZoomLevel.
[[nodiscard]] inline double zoom_level_to_factor(double level) noexcept { return std::pow(1.2, level); }
[[nodiscard]] inline double zoom_factor_to_level(double factor) noexcept {
    return factor > 0.0 ? std::log(factor) / std::log(1.2) : 0.0;
}

} // namespace boson::native
