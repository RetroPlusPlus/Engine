#pragma once

#include <algorithm>

namespace gbcpp {

// A size in physical pixels. Used for the window's drawable surface and the internal
// viewport. Identity is the named fields — never a positional pair.
struct PixelSize {
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool operator==(const PixelSize&) const noexcept = default;
};

// An axis-aligned integer rectangle in pixels (origin top-left). The blit's destination
// region within the swapchain is one of these.
struct IntRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool operator==(const IntRect&) const noexcept = default;
};

// Largest integer multiple of `viewport` that fits within `drawable`, centred, with the
// leftover split into letterbox/pillarbox margins (the negative-margin case below). This
// is the faithful default output scaling — integer-scale-to-fit + letterbox; the richer
// scaling-mode vocabulary (fit / fullscreen / N×) lives in the later enhancement chain.
//
// Clamps the scale to a minimum of 1×: if the window is smaller than the viewport the
// content is shown at 1× and the centred rect's origin goes negative (it overflows the
// window) rather than collapsing to nothing. Degenerate (non-positive) sizes yield an
// empty rect.
[[nodiscard]] constexpr IntRect integerScaleToFitRect(PixelSize drawable,
                                                      PixelSize viewport) noexcept {
    if (drawable.width <= 0 || drawable.height <= 0 ||
        viewport.width <= 0 || viewport.height <= 0) {
        return IntRect{};
    }
    const int scale = std::max(1, std::min(drawable.width / viewport.width,
                                           drawable.height / viewport.height));
    const int width = viewport.width * scale;
    const int height = viewport.height * scale;
    return IntRect{(drawable.width - width) / 2, (drawable.height - height) / 2,
                   width, height};
}

}  // namespace gbcpp
