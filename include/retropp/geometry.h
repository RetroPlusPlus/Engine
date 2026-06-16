#pragma once

#include <algorithm>

namespace retropp {

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

// Float vectors — the C++ image of HLSL float2/float3/float4. A custom shader (ENG-2.I.b) declares its
// own cbuffer in those HLSL types; the build reflects the cbuffer and surfaces each field on
// ScreenSpaceEffect with the matching type below (float→float, float2→Vec2, …), so the game sets the
// shader's own vector params inline. Plain layout-compatible PODs (tightly packed floats, no padding) so
// the generated packer can memcpy them straight into the cbuffer.
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    [[nodiscard]] constexpr bool operator==(const Vec2&) const noexcept = default;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    [[nodiscard]] constexpr bool operator==(const Vec3&) const noexcept = default;
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    [[nodiscard]] constexpr bool operator==(const Vec4&) const noexcept = default;
};

// The pixel dimensions of ONE asset — the unit a sprite draws (Sprite::size) AND the unit the
// atlas slicer carves a loaded image into (sliceLayout / loadAtlas, ENG-2.G). It lives here in
// geometry.h (foundational, beside PixelSize) so both the draw-state layer and the asset-ingestion
// layer depend on it without image.h ever including draw_state.h. Identity is the named fields.
//
// Console asset sizes are named presets — static members of the type (AssetDimensions::GameBoy8x8,
// …), the self-type-constant idiom (declared in-class, defined inline constexpr just below), byte-
// for-byte the ViewportResolution / TimingProfile pattern. An asset size IS a {width, height} tuple,
// so a preset and a raw value are interchangeable. The preset names carry their dimensions
// (GameBoy8x16, not "GameBoyTall") so the value is legible at the call site. Not an exhaustive
// registry; the engine generalizes beyond the Game Boy, so an arbitrary AssetDimensions{w,h} covers
// anything not named.
struct AssetDimensions {
    int width  = 8;
    int height = 8;
    [[nodiscard]] constexpr bool operator==(const AssetDimensions&) const noexcept = default;

    static const AssetDimensions GameBoy8x8;        // default when nothing is specified
    static const AssetDimensions GameBoy8x16;
    static const AssetDimensions GameBoyColor8x8;
    static const AssetDimensions GameBoyColor8x16;
    static const AssetDimensions GameBoyAdvance8x8; // GBA base; OBJ range 8×8…64×64
    static const AssetDimensions Nes8x8;
    static const AssetDimensions Nes8x16;
    static const AssetDimensions MasterSystem8x8;
    static const AssetDimensions MasterSystem8x16;
    static const AssetDimensions Snes8x8;
    static const AssetDimensions Snes16x16;
    static const AssetDimensions Snes32x32;
    static const AssetDimensions Snes64x64;
    static const AssetDimensions Genesis32x32;      // max single sprite; MD composes 8px cells
};

inline constexpr AssetDimensions AssetDimensions::GameBoy8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::GameBoy8x16{8, 16};
inline constexpr AssetDimensions AssetDimensions::GameBoyColor8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::GameBoyColor8x16{8, 16};
inline constexpr AssetDimensions AssetDimensions::GameBoyAdvance8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::Nes8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::Nes8x16{8, 16};
inline constexpr AssetDimensions AssetDimensions::MasterSystem8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::MasterSystem8x16{8, 16};
inline constexpr AssetDimensions AssetDimensions::Snes8x8{8, 8};
inline constexpr AssetDimensions AssetDimensions::Snes16x16{16, 16};
inline constexpr AssetDimensions AssetDimensions::Snes32x32{32, 32};
inline constexpr AssetDimensions AssetDimensions::Snes64x64{64, 64};
inline constexpr AssetDimensions AssetDimensions::Genesis32x32{32, 32};

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

// The integer window scale to actually use, given a desired `target` multiple and the `usable`
// display area the window must fit inside (both in the SAME units — logical points, since window
// sizing and SDL_GetDisplayUsableBounds are both logical). The window is opened/resized to
// `viewport * scale`; this clamps that scale DOWN so the window never exceeds the usable display
// in either dimension:
//
//   • `target` if viewport*target fits the usable area in both width and height;
//   • otherwise the largest k in [1, target] whose viewport*k fits — the "nearest ratio";
//   • a floor of 1× even when the viewport is larger than the display (the window opens at the OS
//     max / the content letterboxes — never zero).
//
// Pure / constexpr so the clamp is unit-testable without a window. The default target (4×) and the
// huge-viewport guard both flow through here. Degenerate (non-positive) sizes yield max(1, target).
[[nodiscard]] constexpr int fitWindowScale(PixelSize viewport, PixelSize usable,
                                           int target) noexcept {
    const int want = std::max(1, target);
    if (viewport.width <= 0 || viewport.height <= 0 ||
        usable.width <= 0 || usable.height <= 0) {
        return want;  // nothing meaningful to clamp against
    }
    const int maxFitW = usable.width / viewport.width;
    const int maxFitH = usable.height / viewport.height;
    const int maxFit  = std::max(1, std::min(maxFitW, maxFitH));
    return std::min(want, maxFit);
}

}  // namespace retropp
