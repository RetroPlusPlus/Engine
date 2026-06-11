// ENG-2.B.2.c.1 manual runtime demo — the smallest real host that exercises the live platform +
// INDEXED-tile/palette compositor + SPRITE path + blit: open a window, upload an indexed tile
// atlas + a separate sprite atlas + a set of distinct palettes, composite a continuously-
// scrolling tile layer (z=0) with a sprite layer above it (z=10), blit it integer-scaled and
// letterboxed onto the swapchain at display refresh, and route keyboard + gamepad input through
// to the tick callback. Run it on a dev machine and confirm: the window shows a scrolling indexed
// pattern in REAL COLOUR (4×4-tile regions in different palettes, flipped cells mirrored) with
// sprites composited above it — different SpriteSize, drawing from different palettes, h/v-flipped
// sprites mirrored, colour-index-0 pixels TRANSPARENT (the background shows through the sprites'
// holes), all moving each frame; resizing re-letterboxes it, the close button quits, and pressing
// a mapped button prints a line.
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
#include <cstdlib>
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

// The sprite atlas: a 4×2-cell (32×16 px) INDEXED atlas. The top-left 2×2 cell block (16×16 px)
// holds a diamond "blob"; cell 2 (top row, third column, 8×8 px) holds a small cross. Both use
// palette index 0 for transparent pixels (interior holes too) so OBJ transparency is unmistakable
// against the scrolling background. A 16×16 sprite reads tile 0 (the full diamond); an 8×8 sprite
// reads tile 2 (the cross).
constexpr int kSprAtlasW = 32;
constexpr int kSprAtlasH = 16;

std::vector<std::uint8_t> buildSpriteAtlas() {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(kSprAtlasW) * kSprAtlasH, 0);
    auto at = [&](int x, int y) -> std::uint8_t& {
        return a[static_cast<std::size_t>(y) * kSprAtlasW + x];
    };

    // 16×16 diamond, top-left block: Manhattan distance from the centre → colour bands.
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int d = std::abs(x - 8) + std::abs(y - 8);
            std::uint8_t idx = 0;
            if (d <= 4)      idx = 3;
            else if (d <= 6) idx = 2;
            else if (d <= 8) idx = 1;
            at(x, y) = idx;
        }
    }
    at(5, 6) = 0;  at(6, 6) = 0;    // left eye  (interior transparent hole)
    at(10, 6) = 0; at(11, 6) = 0;   // right eye

    // 8×8 cross at cell 2 (origin (16,0)): two arms in index 3, a transparent 2×2 centre hole.
    const int ox = 16, oy = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool arm = (x >= 3 && x <= 4) || (y >= 3 && y <= 4);
            at(ox + x, oy + y) = arm ? std::uint8_t{3} : std::uint8_t{0};
        }
    }
    at(ox + 3, oy + 3) = 0; at(ox + 4, oy + 3) = 0;  // centre hole
    at(ox + 3, oy + 4) = 0; at(ox + 4, oy + 4) = 0;
    return a;
}

// Triangle wave in [lo, hi] over the frame counter — bounces a sprite without <cmath>.
int triWave(int t, int lo, int hi) {
    const int span   = hi - lo;
    if (span <= 0) return lo;
    const int period = 2 * span;
    const int p      = ((t % period) + period) % period;
    return lo + (p < span ? p : period - p);
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

    // A second INDEXED atlas for the sprite layer (its own art, with index-0 transparent regions),
    // coloured through the SAME palette set as the tiles. A 16×16 sprite reads tile 0 (the diamond,
    // spanning a contiguous 2×2 atlas cell block); an 8×8 sprite reads tile 2 (the cross).
    const std::vector<std::uint8_t> spriteAtlasPixels = buildSpriteAtlas();
    const AtlasId spriteAtlas = renderer.uploadAtlas(spriteAtlasPixels.data(), kSprAtlasW, kSprAtlasH);

    // The sprites the demo animates: a mix of SpriteSize, palette-select, and flips — including
    // sprites with interior index-0 holes so transparency is unmistakable. Positions are updated
    // each frame in the render callback. Kept alive for the program's duration; the sprite layer
    // references this vector.
    std::vector<Sprite> sprites(4);
    sprites[0].size = SpriteSize::Snes16x16; sprites[0].tile = 0; sprites[0].palette = 1;
    sprites[1].size = SpriteSize::Snes16x16; sprites[1].tile = 0; sprites[1].palette = 2;
    sprites[1].flipX = true;
    sprites[2].size = SpriteSize::GameBoy8x8; sprites[2].tile = 2; sprites[2].palette = 3;
    sprites[3].size = SpriteSize::GameBoy8x8; sprites[3].tile = 2; sprites[3].palette = 0;
    sprites[3].flipY = true;

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
    int tick    = 0;
    loop.setRender([&](float alpha) {
        frame.layers.clear();

        // z=0: the scrolling indexed tile background.
        DrawLayer bg{};
        bg.id      = LayerId{0};
        bg.z       = 0;
        bg.size    = PixelSize{160, 144};
        bg.scroll  = LayerScroll{scrollX, scrollY};
        bg.alpha   = 1.0f;
        bg.content = TileContent{atlas, std::span<const PaletteId>(paletteSet),
                                 kMapW, kMapH, std::span<const TileCell>(cells)};
        frame.layers.push_back(std::move(bg));

        // z=10: the sprite layer, composited above the background. Screen-fixed scroll {0,0}; the
        // sprites move via their own x/y. 16×16 sprites bounce widely; the 8×8 crosses orbit.
        sprites[0].x = triWave(tick,          0, 144); sprites[0].y = triWave(tick / 2,        0, 128);
        sprites[1].x = triWave(tick + 80,     0, 144); sprites[1].y = triWave(tick / 2 + 40,   0, 128);
        sprites[2].x = triWave(tick * 2,      0, 152); sprites[2].y = 20 + triWave(tick,       0,  60);
        sprites[3].x = triWave(tick * 2 + 60, 0, 152); sprites[3].y = 100 - triWave(tick,      0,  60);

        DrawLayer spr{};
        spr.id      = LayerId{1};
        spr.z       = 10;
        spr.size    = PixelSize{160, 144};
        spr.scroll  = LayerScroll{0, 0};
        spr.alpha   = 1.0f;
        spr.content = SpriteContent{spriteAtlas, std::span<const PaletteId>(paletteSet),
                                    std::span<const Sprite>(sprites)};
        frame.layers.push_back(std::move(spr));

        renderer.renderFrame(frame, alpha);

        ++scrollX;                    // continuous horizontal scroll
        if ((scrollX & 1) == 0) ++scrollY;  // gentle diagonal drift
        ++tick;
    });

    std::printf("ENG-2.B.2.c.1 sprites demo — scrolling tiles with sprites composited above; "
                "close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
