// ENG-2.F focused example #1 — the SHAPE VOCABULARY.
//
// One idea: an effect's `region` (ShapePoints) confines it to a shape. A single horizontal
// RowDisplacement wave runs over a scrolling grid, but ONLY inside the current region — outside the
// shape the grid is undisturbed. Press B to cycle the shape through every preset:
//   circle → capsule → triangle → rectangle → roundedRectangle → regularPolygon(hexagon) → none
// `none` (count 0) removes the region so the wave covers the whole viewport — the byte-identical
// pre-ENG-2.F baseline. The points are VIEWPORT PIXELS; the shape just sits where its coordinates put
// it. Watch the wavy patch take each shape in turn.
//
// Like the other example hosts this opens a real window with SdlPlatform + Renderer, so the live gate
// path keeps compiling + linking on every CI platform even though CI never opens the window. SLOW,
// same-direction drift only — no strobing (photosensitivity).

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

namespace {
using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;

// The current region for a cycle index (B steps it). The points are viewport pixels; everything is
// placed around the screen centre (80, 72) so each shape sits over the same patch.
ShapePoints shapeForIndex(int i) {
    switch (i % 7) {
        case 0:  return ShapePoints::circle({80, 72}, 34);
        case 1:  return ShapePoints::capsule({44, 72}, {116, 72}, 22);
        case 2:  return ShapePoints::triangle({80, 34}, {126, 110}, {34, 110});
        case 3:  return ShapePoints::rectangle({40, 44}, 80, 56);
        case 4:  return ShapePoints::roundedRectangle({36, 40}, 88, 64, 18);
        case 5:  return ShapePoints::regularPolygon({80, 72}, 40, 6);  // hexagon
        default: return ShapePoints{};                                  // none → whole viewport
    }
}
const char* shapeName(int i) {
    switch (i % 7) {
        case 0: return "circle";          case 1: return "capsule";
        case 2: return "triangle";        case 3: return "rectangle";
        case 4: return "roundedRectangle"; case 5: return "regularPolygon (hexagon)";
        default: return "none (whole viewport)";
    }
}
}  // namespace

int main() {
    SDL_SetMainReady();
    const EngineConfig config{.window = {.title = "Retro++ — ENG-2.F: region shapes"}};
    SteadyClock clock;
    RunLoop     loop{clock, config.timing};
    SdlPlatform platform{config};
    Renderer    renderer{platform.device(), platform.window(), config.viewport};
    renderer.setSamplingMode(config.enhancements.sampling);

    // A grid tile (border index 2 over fill index 1) repeated everywhere — the displacement waves its
    // lines, so the effect's reach is obvious. Two palettes checkerboarded for legibility.
    std::array<std::uint8_t, 64> grid{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            grid[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId atlas = renderer.uploadAtlas(grid.data(), 8, 8);

    const std::array<Rgba8, 3> palA{{{0, 0, 0}, {54, 78, 140}, {150, 188, 255}}};
    const std::array<Rgba8, 3> palB{{{0, 0, 0}, {140, 60, 96}, {255, 178, 210}}};
    const PaletteId pa = renderer.uploadPalette(std::span<const Rgba8>(palA));
    const PaletteId pb = renderer.uploadPalette(std::span<const Rgba8>(palB));
    const std::array<PaletteId, 2> palSet{pa, pb};

    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y)
        for (int x = 0; x < kMapW; ++x)
            cells[static_cast<std::size_t>(y) * kMapW + x] =
                TileCell{.tile = 0, .palette = static_cast<std::uint8_t>((x + y) & 1)};

    int shapeIdx = 0;
    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::B)) {
            ++shapeIdx;
            std::printf("[dev] region shape: %s\n", shapeName(shapeIdx));
        }
        if (in.justPressed(Button::Select)) platform.setFullscreen(!platform.isFullscreen());
    });

    FrameDrawState frame;
    int            tick = 0;
    loop.setRender([&](float alpha) {
        frame.layers.clear();
        DrawLayer bg{};
        bg.id      = "grid";
        bg.z       = 0;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.scroll  = LayerScroll{tick / 8, 0};  // slow same-direction drift
        bg.content = TileContent{atlas, std::span<const PaletteId>(palSet),
                                 kMapW, kMapH, std::span<const TileCell>(cells)};
        // The wave — confined to the current shape. Everything ELSE about the effect is ordinary; only
        // `region` makes it local. A None-kind region (shapeIdx % 7 == 6) covers the whole viewport.
        bg.effect = ScreenSpaceEffect{
            .kind      = ScreenSpaceEffectKind::RowDisplacement,
            .amplitude = 4.0f,
            .frequency = 2.5f,
            .phase     = static_cast<float>(tick) * 0.006f,
            .axis      = Axis::Horizontal,
            .scope     = ScreenSpaceEffectScope::Layer,
            .region    = shapeForIndex(shapeIdx)};
        frame.layers.push_back(bg);

        renderer.renderFrame(frame, alpha);
        ++tick;
    });

    std::printf("ENG-2.F region shapes — a horizontal wave confined to a shape. B cycles "
                "circle/capsule/triangle/rectangle/roundedRectangle/hexagon/none. Select = fullscreen.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
