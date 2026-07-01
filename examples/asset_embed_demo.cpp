// Asset-embed-policy demo (ENG-2.M.b) — three assets, three policy outcomes, all decided in the code by
// the policy argument alone. Every call passes the same kind of bare logical path; the engine resolves
// it (registry for embedded, asset root for load-from-path) — the developer never builds a path.
//
//   • MAP  → loadMapPng("…map.png")                         — no policy arg ⇒ loadMapPng's per-type
//       default EMBED: baked into THIS binary, decoded from memory, never a loose file.
//   • FONT → loadAtlas("…font.png", …, AssetPolicy::Embed)  — explicit EMBED override of loadAtlas's
//       default: also baked into the binary, never a loose file.
//   • MENU → loadAtlas("…menu.png", …)                      — no policy arg ⇒ loadAtlas's per-type
//       default LOADFROMPATH: rides along beside the binary as a file, read from disk at runtime.
//
// The build acts on those policies automatically — it bakes the two Embed assets and copies the one
// LoadFromPath asset beside the binary. No build rule, no copy rule, no path construction in this code.
// Renders "HELLO / WORLD" in a menu frame. Static image. Select = fullscreen, A = cycle window scale.

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdio>
#include <exception>
#include <span>

#include "retropp/asset_policy.h"
#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/image.h"
#include "retropp/input.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/tilemap.h"
#include "retropp/windowed_host.h"

int main() {
    using namespace retropp;
    SDL_SetMainReady();

    const EngineConfig config{.window = {.title = "Retro++ — asset embed policy (map+font baked, menu rides as a file)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    int windowScale = config.enhancements.windowScale;

    AtlasManifest fontAtlas, menuAtlas;
    IndexGrid     map;
    try {
        // FONT — EMBED (explicit). Bare logical path; the build bakes it, the loader reads it from memory.
        fontAtlas = renderer.loadAtlas("examples/embed_demo_assets/asset_embed_demo_font.png",
                                       AssetDimensions::GameBoy8x8, ContentKind::Tileset,
                                       ReadOrder::LeftRightThenDown, /*count=*/0, TransparentIndices::None,
                                       /*framesPerAnimation=*/0, AssetPolicy::Embed);
        // MENU — LOADFROMPATH (default). Same bare logical path; the engine resolves it to disk.
        menuAtlas = renderer.loadAtlas("examples/embed_demo_assets/asset_embed_demo_menu.png",
                                       AssetDimensions::GameBoy8x8, ContentKind::Tileset,
                                       ReadOrder::LeftRightThenDown);
        // MAP — EMBED (default). Baked, decoded from memory, never read from disk.
        map = loadMapPng("examples/embed_demo_assets/asset_embed_demo_map.png");
    } catch (const std::exception& e) {
        std::printf("demo: could not load assets: %s\n", e.what());
        return 1;
    }

    const std::array<Rgba8, 2> menuColours{{ {20, 28, 64}, {235, 200, 90} }};   // bg, gold
    const std::array<Rgba8, 2> textColours{{ {20, 28, 64}, {245, 245, 255} }};  // bg, white
    const PaletteId menuPal = renderer.uploadPalette(std::span<const Rgba8>(menuColours));
    const PaletteId textPal = renderer.uploadPalette(std::span<const Rgba8>(textColours));

    // Catalog (mirrors examples/embed_demo_assets/gen_embed_demo.py — sparse 16-bit ids = index*4369).
    // Menu border = one corner + one h-edge + one v-edge + one fill; flips make the other positions.
    TileCatalog cat;
    cat.entries = {
        {.id = 0,     .sheet = menuAtlas, .slot = menuAtlas[3].tile, .palette = menuPal},                              // fill
        {.id = 4369,  .sheet = menuAtlas, .slot = menuAtlas[0].tile, .palette = menuPal},                              // corner TL
        {.id = 8738,  .sheet = menuAtlas, .slot = menuAtlas[0].tile, .palette = menuPal, .flipX = true},               // corner TR
        {.id = 13107, .sheet = menuAtlas, .slot = menuAtlas[0].tile, .palette = menuPal, .flipY = true},               // corner BL
        {.id = 17476, .sheet = menuAtlas, .slot = menuAtlas[0].tile, .palette = menuPal, .flipX = true, .flipY = true},// corner BR
        {.id = 21845, .sheet = menuAtlas, .slot = menuAtlas[1].tile, .palette = menuPal},                              // top edge
        {.id = 26214, .sheet = menuAtlas, .slot = menuAtlas[1].tile, .palette = menuPal, .flipY = true},               // bottom edge
        {.id = 30583, .sheet = menuAtlas, .slot = menuAtlas[2].tile, .palette = menuPal},                              // left edge
        {.id = 34952, .sheet = menuAtlas, .slot = menuAtlas[2].tile, .palette = menuPal, .flipX = true},               // right edge
        {.id = 39321, .sheet = fontAtlas, .slot = fontAtlas[1].tile, .palette = textPal},                             // H
        {.id = 43690, .sheet = fontAtlas, .slot = fontAtlas[2].tile, .palette = textPal},                             // E
        {.id = 48059, .sheet = fontAtlas, .slot = fontAtlas[3].tile, .palette = textPal},                             // L
        {.id = 52428, .sheet = fontAtlas, .slot = fontAtlas[4].tile, .palette = textPal},                             // O
        {.id = 56797, .sheet = fontAtlas, .slot = fontAtlas[5].tile, .palette = textPal},                             // W
        {.id = 61166, .sheet = fontAtlas, .slot = fontAtlas[6].tile, .palette = textPal},                             // R
        {.id = 65535, .sheet = fontAtlas, .slot = fontAtlas[7].tile, .palette = textPal},                             // D
    };

    AssembledTilemap assembled;
    try {
        assembled = assembleTilemap(map, cat);
    } catch (const std::exception& e) {
        std::printf("demo: assembleTilemap failed: %s\n", e.what());
        return 1;
    }

    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::Select)) {
            platform.setFullscreen(!platform.isFullscreen());
        }
        if (in.justPressed(Button::A)) {
            windowScale = (windowScale >= 8) ? 1 : windowScale + 1;
            const PixelSize vp{config.viewport.width, config.viewport.height};
            const int eff = fitWindowScale(vp, platform.usableDisplaySize(), windowScale);
            if (!platform.isFullscreen()) platform.setWindowSize(PixelSize{vp.width * eff, vp.height * eff});
        }
    });

    FrameDrawState frame;
    loop.setRender([&]() {
        frame.layers.clear();
        DrawLayer layer{.key = "MenuAndText"};
        layer.z       = 0;
        layer.size    = PixelSize{config.viewport.width, config.viewport.height};
        layer.content = assembled.asTileContent(TileWrap::Blank);
        frame.layers.push_back(std::move(layer));
        renderer.renderFrame(frame);
    });

    std::printf("asset embed demo — map+font baked into the binary, menu loaded from a file; close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
