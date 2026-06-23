// Stencil demo — a runnable host that VISUALLY proves the built-in region-erase effect: a region applied
// as a MASK on a layer's own alpha (the subtractive sibling of region-as-effect-gate). It opens a window
// with three layers — a vivid REAR scene (z low), a brick WALL (z high) carrying a Stencil effect, and the
// backdrop behind everything — and erases the wall's pixels in/around a slowly-drifting region:
//
//   • EraseInside  — a HOLE: the wall is erased inside the shape. (default)
//   • EraseOutside — a PORTHOLE: the wall is erased outside the shape; only the inside survives.
//
// What the hole reveals depends on the effect's SCOPE — the same erase, two reveal targets:
//   • Layer scope — erase ONLY the wall → the REAR scene behind it shows through (the area behind is seen).
//   • Below scope — erase the wall AND everything beneath it → the BACKDROP shows through (a true cut-out).
//
// A toggles the mode; B cycles the shape (circle / capsule / triangle / rectangle / roundedRectangle /
// hexagon / quadratic curve); Up toggles a soft FEATHERED edge against a hard one; Down toggles the scope
// (reveal the rear layer vs reveal the backdrop); Select = fullscreen; close to quit.
//
// Containment + survival are the device-free ctest suite's job (stencilCoverage / stencilSurvival vs the
// region SDF); this is the live GPU sanity check. Photosensitivity: the region drifts slowly side to side
// and never strobes or flashes; the window never auto-launches (a dev drives it).

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/curve.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/transform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;  // 20×18 tiles cover the 160×144 viewport

// A closed SCALLOPED FLOWER of `lobes` quadratic Bezier segments about (cx,cy): anchor points sit on the
// inner radius rIn, each segment's control point bulges out to rOut between them, so the boundary is an
// unmistakably CURVED outline (petals) — not a circle, not a polygon. Genuinely quadratic, so the analytic
// stencil shader (region_stencil_curve.frag) erases along the true curve exactly, no facets.
[[nodiscard]] Curve flowerCurve(float cx, float cy, float rIn, float rOut, int lobes) {
    constexpr float kTwoPi = 6.283185307179586f;
    Curve c;
    c.closed = true;
    const auto pt = [&](float radius, float frac) {
        const float a = kTwoPi * frac;
        return Vec2{cx + radius * std::cos(a), cy + radius * std::sin(a)};
    };
    for (int i = 0; i < lobes; ++i) {
        const Vec2 p0   = pt(rIn,  static_cast<float>(i) / static_cast<float>(lobes));
        const Vec2 ctrl = pt(rOut, (static_cast<float>(i) + 0.5f) / static_cast<float>(lobes));  // bulge out
        const Vec2 p2   = pt(rIn,  static_cast<float>(i + 1) / static_cast<float>(lobes));
        c.segments.push_back(CurveSegment{p0, ctrl, p2, Vec2{}, CurveDegree::Quadratic});
    }
    return c;
}

// The base region for a cycle index, centred on the screen (80, 72); the render loop adds the drift via a
// transform translation, so one offset moves every shape (including the curve).
[[nodiscard]] ShapePoints shapeForIndex(int i) {
    switch (i % 7) {
        case 0:  return ShapePoints::circle({80, 72}, 38);
        case 1:  return ShapePoints::capsule({52, 72}, {108, 72}, 26);
        case 2:  return ShapePoints::triangle({80, 32}, {124, 110}, {36, 110});
        case 3:  return ShapePoints::rectangle({44, 44}, 72, 56);
        case 4:  return ShapePoints::roundedRectangle({40, 40}, 80, 64, 18);
        case 5:  return ShapePoints::regularPolygon({80, 72}, 42, 6);            // hexagon
        default: return ShapePoints::fromCurve(flowerCurve(80, 72, 26, 46, 6));  // scalloped quadratic curve
    }
}
[[nodiscard]] const char* shapeName(int i) {
    switch (i % 7) {
        case 0: return "circle";           case 1: return "capsule";
        case 2: return "triangle";         case 3: return "rectangle";
        case 4: return "roundedRectangle"; case 5: return "regularPolygon (hexagon)";
        default: return "quadratic curve (scalloped flower)";
    }
}

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{.window = {.title = "Retro++ — stencil demo (region erase: hole / porthole)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    renderer.setSamplingMode(config.enhancements.sampling);

    // Rear scene: one solid 8×8 tile (palette index 1); the colour comes from the per-cell palette
    // selection, so one tile draws a multi-colour scene.
    std::array<std::uint8_t, 64> solidArt{};
    solidArt.fill(1);
    const AtlasId rearAtlas = renderer.uploadAtlas(solidArt.data(), 8, 8);

    // Wall: a two-tile brick atlas (16×8). Tile 0 carries its vertical mortar seam at column 0, tile 1 at
    // column 4; alternating them per row makes a running-bond brick wall, so the foreground reads as a
    // distinct TEXTURED surface, not a flat fill. index 1 = brick, index 2 = mortar.
    std::array<std::uint8_t, 128> brickArt{};
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int  tile   = x / 8;
            const int  localX = x % 8;
            const bool mortar = (y == 7) || (tile == 0 ? localX == 0 : localX == 4);  // course + seam lines
            brickArt[static_cast<std::size_t>(y) * 16 + x] = mortar ? 2 : 1;
        }
    }
    const AtlasId wallAtlas = renderer.uploadAtlas(brickArt.data(), 16, 8);

    // Rear-scene palette set: four bright hues a cell selects between, so the revealed scene is an
    // unmistakable distinct layer behind the wall. Index 0 is unused backdrop black.
    const std::array<Rgba8, 2> palCyan{{{0, 0, 0}, {70, 200, 220}}};
    const std::array<Rgba8, 2> palGold{{{0, 0, 0}, {235, 190, 70}}};
    const std::array<Rgba8, 2> palRose{{{0, 0, 0}, {225, 95, 150}}};
    const std::array<Rgba8, 2> palLime{{{0, 0, 0}, {130, 210, 100}}};
    const std::array<PaletteId, 4> rearSet{renderer.uploadPalette(std::span<const Rgba8>(palCyan)),
                                           renderer.uploadPalette(std::span<const Rgba8>(palGold)),
                                           renderer.uploadPalette(std::span<const Rgba8>(palRose)),
                                           renderer.uploadPalette(std::span<const Rgba8>(palLime))};

    // Bold diagonal colour bands → the rear scene reads as a vivid, distinct layer through any hole. Static.
    std::vector<TileCell> rearCells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int ty = 0; ty < kMapH; ++ty) {
        for (int tx = 0; tx < kMapW; ++tx) {
            const std::uint8_t pal = static_cast<std::uint8_t>(((tx + ty) / 2) % 4);  // diagonal bands
            rearCells[static_cast<std::size_t>(ty) * kMapW + tx] = TileCell{.tile = 0, .palette = pal};
        }
    }

    const std::array<Rgba8, 3> wallPal{{{0, 0, 0}, {176, 84, 64}, {224, 212, 196}}};  // brick + light mortar
    const std::array<PaletteId, 1> wallSet{renderer.uploadPalette(std::span<const Rgba8>(wallPal))};
    // Running bond: even rows draw tile 0 (seam at column 0), odd rows tile 1 (seam offset to column 4).
    std::vector<TileCell> wallCells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int ty = 0; ty < kMapH; ++ty)
        for (int tx = 0; tx < kMapW; ++tx)
            wallCells[static_cast<std::size_t>(ty) * kMapW + tx] =
                TileCell{.tile = static_cast<std::uint16_t>(ty & 1), .palette = 0};

    // Stencil controls.
    StencilMode mode     = StencilMode::EraseInside;  // A toggles
    int         shapeIdx = 0;                         // B cycles
    bool        soft     = false;                     // Up toggles feather hard ↔ soft
    bool        below    = false;                     // Down toggles Layer (reveal rear) ↔ Below (reveal backdrop)
    constexpr float kSoftFeather = 16.0f;

    int tick = 0;
    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::A)) {
            mode = mode == StencilMode::EraseInside ? StencilMode::EraseOutside : StencilMode::EraseInside;
            std::printf("[dev] mode: %s\n", mode == StencilMode::EraseInside ? "EraseInside (hole)"
                                                                             : "EraseOutside (porthole)");
        }
        if (in.justPressed(Button::B)) {
            ++shapeIdx;
            std::printf("[dev] shape: %s\n", shapeName(shapeIdx));
        }
        if (in.justPressed(Button::Up)) {
            soft = !soft;
            std::printf("[dev] edge: %s\n", soft ? "feathered (soft)" : "hard");
        }
        if (in.justPressed(Button::Down)) {
            below = !below;
            std::printf("[dev] scope: %s\n", below ? "Below (hole reveals the BACKDROP)"
                                                   : "Layer (hole reveals the REAR scene)");
        }
        if (in.justPressed(Button::Select)) platform.setFullscreen(!platform.isFullscreen());
        ++tick;
    });

    FrameDrawState frame;
    loop.setRender([&](float alpha) {
        // The region drifts slowly side to side (~8 s sweep) via a transform translation — one offset moves
        // every shape, the curve included. Gentle, no strobing.
        const float t  = static_cast<float>(tick) * 0.012f;
        const float dx = 34.0f * std::sin(t);

        ShapePoints region = shapeForIndex(shapeIdx);
        region.transform   = Transform::translation(dx, 0.0f);

        frame.layers.clear();
        DrawLayer rear{};
        rear.id      = "rearScene";
        rear.z       = -10;
        rear.size    = PixelSize{kViewW, kViewH};
        rear.content = TileContent{rearAtlas, std::span<const PaletteId>(rearSet),
                                   kMapW, kMapH, std::span<const TileCell>(rearCells)};
        frame.layers.push_back(rear);

        DrawLayer wall{};
        wall.id      = "wall";
        wall.z       = 0;
        wall.size    = PixelSize{kViewW, kViewH};
        wall.content = TileContent{wallAtlas, std::span<const PaletteId>(wallSet),
                                   kMapW, kMapH, std::span<const TileCell>(wallCells)};
        // The Stencil erases the wall in/around the region. Layer scope reveals the rear scene through the
        // hole; Below scope erases the rear scene too, revealing the backdrop.
        wall.effect = ScreenSpaceEffect{
            .kind    = ScreenSpaceEffectKind::Stencil,
            .scope   = below ? ScreenSpaceEffectScope::Below : ScreenSpaceEffectScope::Layer,
            .stencil = mode,
            .feather = soft ? kSoftFeather : 0.0f,
            .region  = region};
        frame.layers.push_back(std::move(wall));

        renderer.renderFrame(frame, alpha);
    });

    std::printf("stencil demo — a Stencil erases the brick WALL to reveal what's behind it. "
                "A: EraseInside (hole) / EraseOutside (porthole). B: cycle shape. Up: hard / feathered edge. "
                "Down: Layer (reveal rear scene) / Below (reveal backdrop). Select = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
