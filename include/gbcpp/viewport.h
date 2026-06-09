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

}  // namespace gbcpp
