// ENG-2.F focused example #4 — a VERTICAL wave in a region.
//
// One idea: the built-in RowDisplacement can displace along EITHER axis. `Axis::Horizontal` slides each
// row sideways (classic wavy water); `Axis::Vertical` slides each column up/down. This demo confines a
// wave to the bottom-half rectangle and lets B toggle its axis, so you can compare horizontal vs
// vertical displacement in the same patch. (The top half stays still — a hint of the capstone's
// parallax-top / vertical-wave-bottom split.)
//
// Opens a real window so the live gate path keeps compiling on every CI platform. SLOW drift only — no
// strobing (photosensitivity).

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "gbcpp/clock.h"
#include "gbcpp/draw_state.h"
#include "gbcpp/engine_config.h"
#include "gbcpp/geometry.h"
#include "gbcpp/input.h"
#include "gbcpp/palette.h"
#include "gbcpp/renderer.h"
#include "gbcpp/run_loop.h"
#include "gbcpp/sdl_platform.h"
#include "gbcpp/windowed_host.h"

namespace {
using namespace gbcpp;
constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;
}  // namespace

int main() {
    SDL_SetMainReady();
    const EngineConfig config{.window = {.title = "GBCPP — ENG-2.F: vertical wave in a region"}};
    SteadyClock clock;
    RunLoop     loop{clock, config.timing};
    SdlPlatform platform{config};
    Renderer    renderer{platform.device(), platform.window(), config.viewport};
    renderer.setSamplingMode(config.enhancements.sampling);

    std::array<std::uint8_t, 64> grid{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            grid[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId atlas = renderer.uploadAtlas(grid.data(), 8, 8);
    const std::array<Rgba8, 3> pal{{{0, 0, 0}, {40, 96, 132}, {168, 226, 252}}};
    const PaletteId p = renderer.uploadPalette(std::span<const Rgba8>(pal));
    const std::array<PaletteId, 1> palSet{p};
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH, TileCell{.tile = 0, .palette = 0});

    bool vertical = true;  // B toggles Axis::Vertical vs Axis::Horizontal
    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::B)) { vertical = !vertical; std::printf("[dev] wave axis: %s\n", vertical ? "Vertical" : "Horizontal"); }
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
        bg.content = TileContent{atlas, std::span<const PaletteId>(palSet),
                                 kMapW, kMapH, std::span<const TileCell>(cells)};
        // The wave, axis-toggled, confined to the bottom half (y ∈ [72,144)).
        bg.effect = ScreenSpaceEffect{
            .kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 4.0f, .frequency = 2.5f,
            .phase = static_cast<float>(tick) * 0.006f,
            .axis = vertical ? Axis::Vertical : Axis::Horizontal,
            .scope = ScreenSpaceEffectScope::Layer,
            .region = ShapePoints::rectangle({0, 72}, kViewW, kViewH - 72)};
        frame.layers.push_back(bg);

        renderer.renderFrame(frame, alpha);
        ++tick;
    });

    std::printf("ENG-2.F vertical wave — a wave confined to the bottom half; B toggles Vertical vs "
                "Horizontal axis. Select = fullscreen.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
