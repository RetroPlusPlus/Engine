// ENG-2.B.2.b manual runtime demo — the smallest real host that exercises the live platform +
// INDEXED-tile/palette compositor + blit path: open a window, upload an indexed tile atlas
// (palette indices, not colour) + a set of distinct palettes, composite a continuously-
// scrolling tile layer whose cells select different palettes and flips, blit it integer-scaled
// and letterboxed onto the swapchain at display refresh, and route keyboard + gamepad input
// through to the tick callback. Run it on a dev machine and confirm: the window shows a
// scrolling indexed pattern rendered in REAL COLOUR, with 4×4-tile regions drawing from
// different palettes and flipped cells visibly mirrored (a centred rect on black bars),
// resizing re-letterboxes it, the close button quits, and pressing a mapped button prints a
// line.
//
// This is also the only target that instantiates SdlPlatform + Renderer in a real run, so it
// keeps the live SDL_GPU pipeline/upload/present path compiling and linking on every CI
// platform even though CI never opens the window.

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main and
// expect SDL's entry shim. We init SDL ourselves inside SdlPlatform.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <utility>
#include <vector>

#include "gbcpp/clock.h"
#include "gbcpp/draw_state.h"
#include "gbcpp/engine_config.h"
#include "gbcpp/input.h"
#include "gbcpp/palette.h"
#include "gbcpp/renderer.h"
#include "gbcpp/run_loop.h"
#include "gbcpp/sdl_platform.h"
#include "gbcpp/windowed_host.h"

namespace {

using namespace gbcpp;

constexpr int kTile      = 8;  // GB tile edge
constexpr int kAtlasCols = 2;  // 2×2-tile atlas → tiles 0..3
constexpr int kAtlasRows = 2;
constexpr int kMapW      = 16; // tilemap dimensions in tiles (wraps under scroll)
constexpr int kMapH      = 16;

// One 8×8 tile of palette INDICES (0..3 → entries of a 4-colour palette). Deliberately
// asymmetric in both axes so h/v flips are unmistakable on screen, and uses all four indices
// so palette differences are visible. 0 is the background index.
constexpr std::array<std::uint8_t, kTile * kTile> kTilePattern{
    1, 1, 1, 1, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 2, 2, 0, 0, 0,
    1, 0, 0, 2, 2, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 3,
    1, 0, 0, 0, 0, 0, 3, 3,
};

// Write the shared index pattern into one atlas tile. The atlas is row-major R8 (one index per
// pixel), laid out as a kAtlasCols×kAtlasRows grid; tile t lives at grid (t % cols, t / cols).
void paintTileIndices(std::vector<std::uint8_t>& atlas, int atlasW, int tileIndex) {
    const int col = tileIndex % kAtlasCols;
    const int row = tileIndex / kAtlasCols;
    for (int y = 0; y < kTile; ++y) {
        for (int x = 0; x < kTile; ++x) {
            const int px = col * kTile + x;
            const int py = row * kTile + y;
            atlas[static_cast<std::size_t>(py) * atlasW + px] =
                kTilePattern[static_cast<std::size_t>(y) * kTile + x];
        }
    }
}

}  // namespace

int main() {
    SDL_SetMainReady();

    // One startup config bundles window + viewport + timing + controller profile; the demo
    // threads its fields into the existing platform / renderer / loop constructors. Defaults
    // are the faithful Game Boy Color baseline — only the window title is overridden here.
    const EngineConfig config{
        .window = {.title = "GBCPP — ENG-2.A EngineConfig bootstrap demo"}};

    SteadyClock clock;
    RunLoop     loop{clock, config.timing};
    SdlPlatform platform{config};
    Renderer    renderer{platform.device(), platform.window(), config.viewport};

    // Build + upload a 2×2-tile INDEXED atlas: every tile carries the same asymmetric index
    // pattern (colour comes from the per-cell palette, not the atlas).
    const int atlasW = kAtlasCols * kTile;
    const int atlasH = kAtlasRows * kTile;
    std::vector<std::uint8_t> atlasIndices(static_cast<std::size_t>(atlasW) * atlasH, 0);
    for (int t = 0; t < kAtlasCols * kAtlasRows; ++t) paintTileIndices(atlasIndices, atlasW, t);
    const AtlasId atlas = renderer.uploadAtlas(atlasIndices.data(), atlasW, atlasH);

    // Four distinct 4-entry palettes (index 0 = dark background, 1..3 = brighter ramp). The
    // same indexed art renders in four different colour schemes depending on the cell's select.
    const std::array<std::array<Rgba8, 4>, 4> paletteColors{{
        {{ {20, 24, 28}, {120, 120, 130}, {185, 190, 200}, {245, 248, 255} }},  // grey
        {{ {28, 16, 16}, {170,  60,  60}, {225, 110,  90}, {255, 210, 170} }},  // warm/red
        {{ {12, 18, 32}, { 60, 100, 200}, {110, 170, 240}, {200, 230, 255} }},  // cool/blue
        {{ {14, 28, 16}, { 60, 160,  90}, {130, 210, 120}, {220, 250, 200} }},  // green
    }};
    std::array<PaletteId, 4> paletteSet{};
    for (std::size_t i = 0; i < paletteColors.size(); ++i) {
        paletteSet[i] = renderer.uploadPalette(std::span<const Rgba8>(paletteColors[i]));
    }

    // A 16×16 map: every cell shows the (single) atlas pattern, but 4×4-tile regions select
    // different palettes and the flip bits vary within each region — so the colour + flip paths
    // are both visible at once. Kept alive for the program's duration; cells references it.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            TileCell& c = cells[static_cast<std::size_t>(y) * kMapW + x];
            c.tile    = static_cast<std::uint16_t>((x ^ y) & 3);
            c.palette = static_cast<std::uint8_t>(((x / 4) + (y / 4)) & 3);  // 4×4 palette blocks
            c.flipX   = (x & 4) != 0;
            c.flipY   = (y & 4) != 0;
        }
    }

    // The labelled buttons the demo prints — the Game Boy set (the demo's active profile).
    // Sized to its entries via to_array: a fixed kButtonCount size would leave value-
    // initialized {Button::Up, nullptr} trailing elements that alias a real Up press and
    // print spurious "(null)" lines.
    constexpr auto kLabels = std::to_array<std::pair<Button, const char*>>({
        {Button::Up, "Up"}, {Button::Down, "Down"}, {Button::Left, "Left"},
        {Button::Right, "Right"}, {Button::A, "A"}, {Button::B, "B"},
        {Button::Start, "Start"}, {Button::Select, "Select"},
    });

    auto familyName = [](ControllerType t) {
        switch (t) {
            case ControllerType::Xbox:        return "Xbox";
            case ControllerType::PlayStation: return "PlayStation";
            case ControllerType::Nintendo:    return "Nintendo";
            case ControllerType::Standard:    return "Standard";
            default:                          return "none";
        }
    };

    ControllerType lastType = ControllerType::Unknown;
    loop.setTick([&](const InputState& in) {
        if (platform.controllerType() != lastType) {  // auto-detected on connect
            lastType = platform.controllerType();
            std::printf("controller: %s\n", familyName(lastType));
        }
        for (const auto& [button, name] : kLabels) {
            if (in.justPressed(button))  std::printf("press   %s\n", name);
            if (in.justReleased(button)) std::printf("release %s\n", name);
        }
    });

    // The game owns the draw state; the render callback rebuilds + scrolls it each advance()
    // (the ENG-1 render-callback contract is unchanged at void(float)).
    FrameDrawState frame;
    int scrollX = 0;
    int scrollY = 0;
    loop.setRender([&](float alpha) {
        frame.layers.clear();
        DrawLayer layer{};
        layer.id      = LayerId{0};
        layer.z       = 0;
        layer.size    = PixelSize{160, 144};
        layer.scroll  = LayerScroll{scrollX, scrollY};
        layer.alpha   = 1.0f;
        layer.content = TileContent{atlas, std::span<const PaletteId>(paletteSet),
                                    kMapW, kMapH, std::span<const TileCell>(cells)};
        frame.layers.push_back(std::move(layer));

        renderer.renderFrame(frame, alpha);

        ++scrollX;                    // continuous horizontal scroll
        if ((scrollX & 1) == 0) ++scrollY;  // gentle diagonal drift
    });

    std::printf("ENG-2.B.2.b indexed/palette compositor demo — close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
