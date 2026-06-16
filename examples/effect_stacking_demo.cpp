// ENG-2.F focused example #5 — a shader applied to ANOTHER shader (effect stacking).
//
// One idea: there is no shader `.then()` (that is `Transform` composition). Effects compose by STACKING
// in submission order — the frame-level `postEffects` chain runs each effect over the previous one's
// output. This demo puts region-gated waves in the chain:
//   [0] a horizontal wave confined to a circle (left)
//   [1] a vertical wave confined to a circle (right) that OVERLAPS the first
// where the two circles overlap, the second wave operates on the first's already-displaced pixels — a
// shader applied to a shader. Each effect's `region` keeps it local; together they prove stacking and
// region-confinement compose. B toggles the second wave so you can see [0] alone vs [0]∘[1]. Up adds a
// whole-frame BUILT-IN ripple (ScreenSpaceEffectKind::Ripple — ENG-2.I.a) stacked over the waves,
// showing the new effect-library member composing with RowDisplacement in the same chain.
//
// Opens a real window so the live chain keeps compiling on every CI platform. SLOW drift only — no
// strobing (photosensitivity).

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
}  // namespace

int main() {
    SDL_SetMainReady();
    const EngineConfig config{.window = {.title = "Retro++ — ENG-2.F: effect stacking"}};
    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    renderer.setSamplingMode(config.enhancements.sampling);

    std::array<std::uint8_t, 64> grid{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            grid[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId atlas = renderer.uploadAtlas(grid.data(), 8, 8);
    const std::array<Rgba8, 3> palA{{{0, 0, 0}, {60, 120, 80}, {180, 240, 190}}};
    const std::array<Rgba8, 3> palB{{{0, 0, 0}, {120, 80, 60}, {245, 210, 170}}};
    const PaletteId pa = renderer.uploadPalette(std::span<const Rgba8>(palA));
    const PaletteId pb = renderer.uploadPalette(std::span<const Rgba8>(palB));
    const std::array<PaletteId, 2> palSet{pa, pb};
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y)
        for (int x = 0; x < kMapW; ++x)
            cells[static_cast<std::size_t>(y) * kMapW + x] =
                TileCell{.tile = 0, .palette = static_cast<std::uint8_t>((x ^ y) & 1)};

    bool secondOn = true;   // B toggles the second (stacked) wave
    bool rippleOn = false;  // Up toggles a whole-frame built-in ripple stacked over the waves
    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::B)) { secondOn = !secondOn; std::printf("[dev] second stacked wave: %s\n", secondOn ? "on" : "off"); }
        if (in.justPressed(Button::Up)) { rippleOn = !rippleOn; std::printf("[dev] built-in ripple: %s\n", rippleOn ? "on" : "off"); }
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
        bg.scroll  = LayerScroll{tick / 10, 0};
        bg.content = TileContent{atlas, std::span<const PaletteId>(palSet),
                                 kMapW, kMapH, std::span<const TileCell>(cells)};
        frame.layers.push_back(bg);

        // Frame-level chain: two region-gated effects. The second reads the first's output where their
        // regions overlap (the chain ping-pongs in submission order).
        const float phase = static_cast<float>(tick) * 0.006f;
        frame.postEffects.clear();
        frame.postEffects.push_back(ScreenSpaceEffect{
            .kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 4.0f, .frequency = 2.5f,
            .phase = phase, .axis = Axis::Horizontal, .region = ShapePoints::circle({66, 72}, 40)});
        if (secondOn) {
            frame.postEffects.push_back(ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 4.0f, .frequency = 2.5f,
                .phase = phase, .axis = Axis::Vertical, .region = ShapePoints::circle({94, 72}, 40)});
        }
        if (rippleOn) {
            // A whole-frame built-in ripple stacked LAST — it runs over whatever the waves produced.
            // center in viewport pixels (engine normalizes to UV); slow outward phase (photosensitivity).
            frame.postEffects.push_back(ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::Ripple, .amplitude = 4.0f, .frequency = 6.0f,
                .phase = static_cast<float>(tick) * 0.012f,
                .center = {kViewW / 2.0f, kViewH / 2.0f}, .decay = 2.0f});
        }

        renderer.renderFrame(frame, alpha);
        ++tick;
    });

    std::printf("ENG-2.F effect stacking — two region-gated waves in the postEffects chain; where their "
                "circles overlap the second runs on the first's output. B toggles the second wave, Up adds a "
                "whole-frame built-in ripple over both. Select = fullscreen.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
