// ENG-2.F CAPSTONE — region-confined effects, everything in one scene.
//
// Combines every focused example into the split the feature was asked for:
//   • TOP HALF: two parallax tile layers scrolling at different speeds (ordinary layers — parallax is
//     not an "effect"; the region feature coexists with it).
//   • BOTTOM HALF: a "water" layer carrying an Axis::VERTICAL RowDisplacement confined to the bottom-half
//     rectangle (per-layer Layer scope + region) — the wave the top parallax must NOT get.
//   • A roaming BUILT-IN ripple (ScreenSpaceEffectKind::Ripple), confined to a CIRCLE that both MOVES
//     across the screen and is SCALED by the region's Transform (moving + transformed + region at once),
//     stacked as a frame-level postEffect over the whole composited scene.
// So: region-confinement, per-layer vs frame-level effects, the vertical axis, a built-in effect through
// the engine-side gate, a moving + transformed shape, and ordinary parallax — all in one frame.
//
// Opens a real window so the whole live path keeps compiling on every CI platform. SLOW, same-direction
// motion only — no strobing (photosensitivity).

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
constexpr int kMapW = 20, kMapH = 18;
constexpr int kHalf = kViewH / 2;  // 72 — the top/bottom split
}  // namespace

int main() {
    SDL_SetMainReady();
    const EngineConfig config{.window = {.title = "Retro++ — ENG-2.F capstone: region showcase"}};
    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    renderer.setSamplingMode(config.enhancements.sampling);

    // Atlas: 0 = transparent hole, 1 = solid fill, 2 = grid-lined cell (border over fill).
    std::array<std::uint8_t, 8 * 8 * 3> atlasPx{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            atlasPx[static_cast<std::size_t>(y) * 24 + 0 * 8 + x] = 0;                               // hole
            atlasPx[static_cast<std::size_t>(y) * 24 + 1 * 8 + x] = 1;                               // solid
            atlasPx[static_cast<std::size_t>(y) * 24 + 2 * 8 + x] = (x == 0 || y == 0) ? 2 : 1;       // grid
        }
    const AtlasId opaque = renderer.uploadAtlas(atlasPx.data(), 24, 8);
    const AtlasId holed  = renderer.uploadAtlas(atlasPx.data(), 24, 8, /*transparentIndex=*/0);

    const std::array<Rgba8, 3> sky{{{0, 0, 0}, {86, 150, 222}, {0, 0, 0}}};
    const std::array<Rgba8, 3> hills{{{0, 0, 0}, {70, 130, 90}, {150, 220, 150}}};   // far parallax
    const std::array<Rgba8, 3> trees{{{0, 0, 0}, {40, 92, 60}, {120, 196, 120}}};    // near parallax
    const std::array<Rgba8, 3> water{{{0, 0, 0}, {32, 86, 150}, {150, 214, 252}}};
    const PaletteId skyP = renderer.uploadPalette(std::span<const Rgba8>(sky));
    const PaletteId hillP = renderer.uploadPalette(std::span<const Rgba8>(hills));
    const PaletteId treeP = renderer.uploadPalette(std::span<const Rgba8>(trees));
    const PaletteId watP = renderer.uploadPalette(std::span<const Rgba8>(water));
    const std::array<PaletteId, 1> skySet{skyP}, hillSet{hillP}, treeSet{treeP}, watSet{watP};

    // Tilemaps. Sky = full solid. Hills/trees = a band of holed grid in the TOP half (parallax shows
    // there). Water = solid grid across the BOTTOM half.
    std::vector<TileCell> skyC(static_cast<std::size_t>(kMapW) * kMapH, TileCell{.tile = 1, .palette = 0});
    std::vector<TileCell> hillC(static_cast<std::size_t>(kMapW) * kMapH, TileCell{.tile = 0, .palette = 0});
    std::vector<TileCell> treeC(static_cast<std::size_t>(kMapW) * kMapH, TileCell{.tile = 0, .palette = 0});
    std::vector<TileCell> watC(static_cast<std::size_t>(kMapW) * kMapH, TileCell{.tile = 0, .palette = 0});
    for (int y = 0; y < kMapH; ++y)
        for (int x = 0; x < kMapW; ++x) {
            const auto i = static_cast<std::size_t>(y) * kMapW + x;
            if (y >= 3 && y <= 5)  hillC[i] = TileCell{.tile = 2, .palette = 0};       // far band
            if (y >= 5 && y <= 7)  treeC[i] = TileCell{.tile = 2, .palette = 0};       // near band
            if (y >= 9)            watC[i]  = TileCell{.tile = 2, .palette = 0};       // bottom half
        }

    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::Select)) platform.setFullscreen(!platform.isFullscreen());
    });

    FrameDrawState frame;
    int            tick = 0;
    loop.setRender([&](float alpha) {
        frame.layers.clear();
        const int t = tick;

        DrawLayer skyL{};
        skyL.id = "sky"; skyL.z = 0; skyL.size = PixelSize{kViewW, kViewH};
        skyL.content = TileContent{opaque, std::span<const PaletteId>(skySet), kMapW, kMapH, std::span<const TileCell>(skyC)};
        frame.layers.push_back(skyL);

        DrawLayer hillL{};  // far parallax — slow
        hillL.id = "hills"; hillL.z = 5; hillL.size = PixelSize{kViewW, kViewH};
        hillL.scroll = LayerScroll{t / 12, 0};
        hillL.content = TileContent{holed, std::span<const PaletteId>(hillSet), kMapW, kMapH, std::span<const TileCell>(hillC)};
        frame.layers.push_back(hillL);

        DrawLayer treeL{};  // near parallax — faster (the parallax depth cue)
        treeL.id = "trees"; treeL.z = 10; treeL.size = PixelSize{kViewW, kViewH};
        treeL.scroll = LayerScroll{t / 5, 0};
        treeL.content = TileContent{holed, std::span<const PaletteId>(treeSet), kMapW, kMapH, std::span<const TileCell>(treeC)};
        frame.layers.push_back(treeL);

        DrawLayer waterL{};  // bottom-half water with a VERTICAL wave confined to the bottom half. The
        // HOLED atlas makes the empty top-half cells (tile 0) transparent so the sky + parallax show
        // through above the waterline — with the opaque atlas they'd paint opaque black over the top half.
        waterL.id = "water"; waterL.z = 15; waterL.size = PixelSize{kViewW, kViewH};
        waterL.content = TileContent{holed, std::span<const PaletteId>(watSet), kMapW, kMapH, std::span<const TileCell>(watC)};
        waterL.effect = ScreenSpaceEffect{
            .kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 3.0f, .frequency = 3.0f,
            .phase = static_cast<float>(t) * 0.006f, .axis = Axis::Vertical,
            .scope = ScreenSpaceEffectScope::Layer,
            .region = ShapePoints::rectangle({0, kHalf}, kViewW, kViewH - kHalf)};
        frame.layers.push_back(waterL);

        // A roaming, scaled BUILT-IN ripple confined to a circle — moving + transformed + region, stacked
        // over the whole composited scene. center in viewport pixels (the engine normalizes to UV).
        const float rx = 80.0f + 50.0f * std::sin(static_cast<float>(t) * 0.009f);  // glide
        const float rs = 1.0f + 0.3f * std::sin(static_cast<float>(t) * 0.02f);     // breathe (scale)
        ScreenSpaceEffect rip{
            .kind = ScreenSpaceEffectKind::Ripple, .amplitude = 5.0f, .frequency = 7.0f,
            .phase = static_cast<float>(t) * 0.012f,
            .center = {kViewW / 2.0f, 0.35f * kViewH}, .decay = 2.5f};
        ShapePoints circ = ShapePoints::circle({rx, 50.0f}, 26.0f);
        circ.transform = Transform::scale(rs, rs, rx, 50.0f);
        rip.region = circ;
        frame.postEffects.clear();
        frame.postEffects.push_back(rip);

        renderer.renderFrame(frame, alpha);
        ++tick;
    });

    std::printf("ENG-2.F capstone — top-half parallax (hills slow, trees fast), a VERTICAL wave confined to "
                "the bottom-half water, and a roaming scaled BUILT-IN ripple in a circle over it all. Select = fullscreen.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
