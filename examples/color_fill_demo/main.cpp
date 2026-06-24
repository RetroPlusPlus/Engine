// ColorFill demo — a runnable host that VISUALLY proves the built-in region-confinable colour fill: a
// colour painted onto a Region's shape. It opens a window over an opaque dim-grid backdrop and draws four
// things with the SAME built-in effect (ScreenSpaceEffectKind::ColorFill), each confined to a Region:
//
//   • a SOLID filled rectangle (fillStrength 1) — a solid coloured shape;
//   • a STROKED ring (a circle + strokeWidth) — a coloured outline / hoop;
//   • a CURVED drawn LINE (ShapePoints::fromCurve on an OPEN curve + strokeWidth) — the missing primitive:
//     a stroked region filled with a colour IS a drawn colored path;
//   • a shaped mul/add TINT (no fill — just clamp(in*mul + add)) over the backdrop art — a shaped grade.
//
// All four are frame-level regions (FrameDrawState::regions), so they paint onto the composited (opaque)
// backdrop — the line-drawing rule: ColorFill recolours EXISTING pixels, so the source must be opaque for
// the fill to show (a frame-level / Below source is the opaque scene; a Layer-scope source would be the
// layer's own art, recolouring it in place). The pixel-exact colour math is the device-free ctest suite's
// job (applyColorFill vs the shader); this is the live GPU sanity check.
//
// Photosensitivity: the scene is STATIC — ColorFill animates nothing on its own (a game would tween a
// parameter to animate it), so there is no motion, flashing, or strobing. The window never auto-launches
// (a dev drives it). Select = fullscreen; close to quit.

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
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
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;  // 20×18 tiles cover the 160×144 viewport

// A solid colour fill (paint the region's shape this colour, ignoring what was under it).
[[nodiscard]] ScreenSpaceEffect solidFill(float r, float g, float b) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                             .fillR = r, .fillG = g, .fillB = b, .fillStrength = 1.0f};
}

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{.window = {.title = "Retro++ — colour-fill demo (solid / stroke / drawn line / tint)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    renderer.setSamplingMode(config.enhancements.sampling);

    // An opaque dim-grid backdrop so the fills/lines have something to paint onto and the tint has texture
    // to grade. A faint two-tone 8×8 grid tile (index 1 fill, index 2 on the top/left edge).
    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8);

    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {40, 44, 62}, {58, 64, 90}}};  // opaque dim slate grid
    const std::array<PaletteId, 1> gridSet{renderer.uploadPalette(std::span<const Rgba8>(gridPal))};
    const std::vector<TileCell>    gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                             TileCell{.tile = 0, .palette = 0});

    // The four shapes (viewport pixels):
    // 1 — a solid filled rectangle (top-left).
    const ShapePoints solidRect = ShapePoints::rectangle(Point{12, 16}, 56, 38);
    // 2 — a stroked ring (top-right): a circle whose effects confine to a band along its boundary.
    ShapePoints       ring = ShapePoints::circle(Point{122, 38}, 22);
    ring.strokeWidth       = 6.0f;
    // 3 — a CURVED drawn LINE (bottom): an OPEN two-segment quadratic curve, stroked into an open band — a
    // stroked region filled with colour is a drawn colored path (the headline: real vector lines).
    Curve wavy;
    wavy.closed   = false;
    wavy.segments = {CurveSegment{Vec2{18, 116}, Vec2{46, 98}, Vec2{74, 116}, Vec2{}, CurveDegree::Quadratic},
                     CurveSegment{Vec2{74, 116}, Vec2{102, 134}, Vec2{142, 112}, Vec2{}, CurveDegree::Quadratic}};
    ShapePoints drawnLine = ShapePoints::fromCurve(wavy);
    drawnLine.strokeWidth = 5.0f;
    // 4 — a shaped mul/add TINT over the backdrop (right side): darken + warm, no fill.
    const ShapePoints tintRect = ShapePoints::rectangle(Point{96, 64}, 52, 30);

    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::Select)) platform.setFullscreen(!platform.isFullscreen());
    });

    FrameDrawState frame;
    loop.setRender([&](float alpha) {
        frame.layers.clear();
        DrawLayer bg{};
        bg.id      = "backgroundGrid";
        bg.z       = -10;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{gridAtlas, std::span<const PaletteId>(gridSet),
                                 kMapW, kMapH, std::span<const TileCell>(gridCells)};
        frame.layers.push_back(bg);

        // A shaped grade: clamp(in*mul + add) inside tintRect — darkens and warms the backdrop there.
        const ScreenSpaceEffect tint{.kind = ScreenSpaceEffectKind::ColorFill,
                                     .mulR = 0.45f, .mulG = 0.5f, .mulB = 0.72f,
                                     .addR = 0.26f, .addG = 0.12f, .addB = 0.0f};

        frame.regions.clear();
        frame.regions.push_back(Region{.shape = solidRect, .effects = {solidFill(0.12f, 0.86f, 1.0f)}});   // cyan solid
        frame.regions.push_back(Region{.shape = ring,      .effects = {solidFill(1.0f, 0.22f, 0.88f)}});   // magenta ring
        frame.regions.push_back(Region{.shape = drawnLine, .effects = {solidFill(1.0f, 0.84f, 0.10f)}});   // yellow drawn line
        frame.regions.push_back(Region{.shape = tintRect,  .effects = {tint}});                            // shaped tint

        renderer.renderFrame(frame, alpha);
    });

    std::printf("colour-fill demo — a SOLID cyan rectangle, a STROKED magenta ring, a CURVED yellow drawn "
                "LINE (fromCurve + stroke = a real vector path), and a shaped darken+warm TINT over the "
                "grid. All one built-in: ScreenSpaceEffectKind::ColorFill confined by a Region. Static "
                "scene; Select = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
