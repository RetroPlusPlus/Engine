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
    const EngineConfig config{.window = {.title = "Retro++ — ENG-2.F: vertical wave in a region"}};
    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};

    std::array<std::uint8_t, 64> grid{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            grid[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId atlas = renderer.uploadAtlas(grid.data(), 8, 8);
    const std::array<Rgba8, 3> pal{{{0, 0, 0}, {40, 96, 132}, {168, 226, 252}}};
    const PaletteId p = renderer.uploadPalette(std::span<const Rgba8>(pal));
    // Each cell names its sheet (`atlas`) and palette directly — there is no per-layer set.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH,
                                TileCell{.tile = 0, .atlas = atlas, .palette = p});

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
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(cells)};
        // The wave, axis-toggled, confined to the bottom half (y ∈ [72,144)).
        bg.regions = {Region{
            .shape   = ShapePoints::rectangle({0, 72}, kViewW, kViewH - 72),
            .effects = {ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 4.0f, .frequency = 2.5f,
                .phase = static_cast<float>(tick) * 0.006f,
                .axis = vertical ? Axis::Vertical : Axis::Horizontal,
                .scope = ScreenSpaceEffectScope::Layer}}}};
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
