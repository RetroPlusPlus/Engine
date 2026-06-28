// Controller scrolling — steer a camera across an endless tile field with the d-pad. It opens a
// window and draws a scrolling, indexed-colour tile background using only hand-built art (no asset
// files). It is the companion to docs/guide/getting-started.md (which walks through it block by block)
// AND the engine's canonical "retained" example: it builds the FrameDrawState ONCE, before the loop,
// and the render callback only mutates the layer's scroll each frame (vs. the immediate-mode hosts
// that rebuild the frame every tick — see docs/guide/how-to.md § retained-vs-rebuilt).
//
// Like the other example hosts it is built on every CI platform (so it keeps compiling against the
// live SdlPlatform + Renderer) but never run in CI, which has no display. Run it on a dev machine.

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main. We init SDL
// ourselves inside SdlPlatform.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

using namespace retropp;

int main() {
    SDL_SetMainReady();

    // 1. Configure. A default EngineConfig is the faithful Game Boy Color baseline (160×144 internal
    //    viewport, 59.7275 Hz). Override only what you mean to change — here, the window title.
    const EngineConfig config{.window = {.title = "Retro++ — controller scrolling"}};

    // 2. The four core objects. The PLATFORM owns the OS window + GPU device + input; the RENDERER
    //    draws into the internal viewport and blits it to the window; the RUN LOOP drives fixed-step
    //    ticks; the CLOCK feeds the loop real time. (See docs/guide/concepts.md for how they fit.)
    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    RunLoop     loop{clock};

    // 3. Upload art. An indexed atlas is one palette INDEX per pixel — colour comes from a palette at
    //    render time, never baked into the art. We hand-build a 2-tile atlas (16×8): tile 0 is a
    //    solid block; tile 1 is a bordered block. One 4-colour palette colours both.
    constexpr int kTile = 8, kCols = 2;                 // 16×8 atlas = two 8×8 tiles side by side
    std::array<std::uint8_t, kTile * kTile * kCols> atlas{};
    for (int y = 0; y < kTile; ++y) {
        for (int x = 0; x < kTile; ++x) {
            const bool edge = (x == 0 || y == 0 || x == kTile - 1 || y == kTile - 1);
            atlas[y * (kTile * kCols) + x]             = 1;             // tile 0: solid (index 1)
            atlas[y * (kTile * kCols) + (kTile + x)]   = edge ? 3 : 1;  // tile 1: bright border on 1
        }
    }
    const AtlasId atlasId = renderer.uploadAtlas(atlas.data(), kTile * kCols, kTile);

    const std::array<Rgba8, 4> colours{{{20, 20, 30}, {70, 110, 180}, {0, 0, 0}, {200, 230, 255}}};
    const PaletteId pal = renderer.uploadPalette(std::span<const Rgba8>(colours));

    // 4. A tilemap: a 32×32 grid checkerboarding the two tiles. Each cell names its own sheet + palette
    //    directly (here every cell draws from `atlasId` coloured through `pal`). Kept alive for the
    //    program's life (the layer holds a span into it).
    constexpr int kMapW = 32, kMapH = 32;
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            TileCell& c = cells[static_cast<std::size_t>(y) * kMapW + x];
            c.tile    = static_cast<std::uint16_t>((x + y) % 2);  // alternate tile 0 / tile 1
            c.atlas   = atlasId;
            c.palette = pal;
        }
    }

    // 5. One tile layer, built ONCE and kept across frames. The render callback only scrolls it.
    FrameDrawState frame;
    frame.layers.resize(1);
    DrawLayer& bg = frame.layers[0];
    bg.label   = "background";
    bg.z       = 0;
    bg.size    = PixelSize{config.viewport.width, config.viewport.height};
    bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                             .cells = std::span<const TileCell>(cells)};

    // 6. Wire the loop. The TICK is one logical step (read input, move the camera); the RENDER draws
    //    the current state. Input is sampled per tick, so logic stays deterministic and
    //    frame-rate-independent.
    int camX = 0, camY = 0;
    loop.setTick([&](const InputState& in) {
        if (in.isHeld(Button::Right)) ++camX;
        if (in.isHeld(Button::Left))  --camX;
        if (in.isHeld(Button::Down))  ++camY;
        if (in.isHeld(Button::Up))    --camY;
    });
    loop.setRender([&](float /*alpha*/) {
        frame.layers[0].scroll = LayerScroll{camX, camY};  // the one thing that changes per frame
        renderer.renderFrame(frame, /*alpha=*/0.0f);
    });

    // 7. Run until the window closes. The windowed host pumps OS events, pushes the held buttons into
    //    the loop, and advances it each iteration; the render callback presents inside advance().
    WindowedHost{loop, platform}.run();
    return 0;
}
