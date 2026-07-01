#pragma once

#include "retropp/geometry.h"

namespace retropp {

// The engine's internal render resolution — the offscreen target the game draws into, before
// it is scaled/letterboxed to the window. Defaults to the original Game Boy 160×144 (the
// faithful default); configurable so a game can request a larger internal viewport (e.g. a
// wider visible world for a zoom-out feature) without the engine assuming a fixed size.
// Identity is the named fields.
//
// Common-platform resolutions are named presets — static members of the type
// (ViewportResolution::GameBoyAdvance, …), the self-type-constant idiom: declared in-class,
// defined inline constexpr just below. A resolution IS a {width, height} tuple, so a preset and
// a raw value are interchangeable (the value-as-data pattern, like PaletteSize / TickPeriodNs).
// Not an exhaustive registry; add platforms as needed. The engine generalizes beyond the Game
// Boy, so a fixed resolution baked into the type would be the hardcoded-dimensions mistake the
// project avoids elsewhere.
//
//   Renderer{dev, win, ViewportResolution::GameBoyAdvance};   // a preset
//   Renderer{dev, win, {256, 224}};                           // or any raw {width, height}
struct ViewportResolution {
    int width = 160;
    int height = 144;

    static const ViewportResolution GameBoy;
    static const ViewportResolution GameBoyColor;
    static const ViewportResolution GameBoyAdvance;
    static const ViewportResolution Nes;
    static const ViewportResolution Snes;
    static const ViewportResolution Genesis;
    static const ViewportResolution MasterSystem;
};

inline constexpr ViewportResolution ViewportResolution::GameBoy{160, 144};
inline constexpr ViewportResolution ViewportResolution::GameBoyColor{160, 144};
inline constexpr ViewportResolution ViewportResolution::GameBoyAdvance{240, 160};
inline constexpr ViewportResolution ViewportResolution::Nes{256, 240};
inline constexpr ViewportResolution ViewportResolution::Snes{256, 224};
inline constexpr ViewportResolution ViewportResolution::Genesis{320, 224};
inline constexpr ViewportResolution ViewportResolution::MasterSystem{256, 192};

// The compose grid a renderer rasterizes content onto: the viewport scaled by an integer factor.
// This is the raster resolution of the offscreen targets and content placement — distinct from the
// viewport's normalization role, where effects and regions measure against the viewport dimensions.
// A factor of 1 makes the compose grid identical to the viewport.
[[nodiscard]] constexpr PixelSize composeDimensions(ViewportResolution viewport,
                                                    int composeScale) noexcept {
    return {viewport.width * composeScale, viewport.height * composeScale};
}

}  // namespace retropp
