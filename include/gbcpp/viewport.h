#pragma once

namespace gbcpp {

// The engine's internal render resolution — the offscreen target the game draws into,
// before it is scaled/letterboxed to the window. Defaults to the original Game Boy
// 160×144 (the faithful baseline); configurable so a game can request a larger internal
// viewport (e.g. a wider visible world for a zoom-out feature) without the engine
// assuming a fixed size. Identity is the named fields.
struct ViewportConfig {
    int width = 160;
    int height = 144;
};

// Common-platform internal render resolutions, as plain {width, height} values. A resolution
// IS a tuple, so these are named ViewportConfig constants, not an enum you convert back to a
// tuple — pass a preset or a raw ViewportConfig interchangeably (the value-as-data pattern,
// like PaletteSize / TickPeriodNs). Not an exhaustive registry; add platforms as needed. The
// engine generalizes beyond the Game Boy, so a fixed resolution in the type would be the same
// hardcoded-dimensions mistake the project avoids elsewhere.
//
//   Renderer{dev, win, resolutions::GameBoyAdvance};   // a preset
//   Renderer{dev, win, {256, 224}};                    // or any raw size
namespace resolutions {
inline constexpr ViewportConfig GameBoy{160, 144};
inline constexpr ViewportConfig GameBoyColor{160, 144};
inline constexpr ViewportConfig GameBoyAdvance{240, 160};
inline constexpr ViewportConfig Nes{256, 240};
inline constexpr ViewportConfig Snes{256, 224};
inline constexpr ViewportConfig Genesis{320, 224};
inline constexpr ViewportConfig MasterSystem{256, 192};
}  // namespace resolutions

}  // namespace gbcpp
