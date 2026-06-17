// Tilemap-import demo (ENG-2.L) — ONE map, drawn from TWO image atlases mixed together: a FONT sheet
// (text glyphs) and a MENU-border sheet (border glyphs). It opens a window and composites a single
// tile layer showing a menu frame with "HELLO / WORLD" text inside it — the menu border and the text
// come from DIFFERENT sheets, selected per-cell within the one layer (the multi-atlas headline).
//
// The pipeline, end to end:
//   1. loadPng  the two atlas PNGs (examples/assets/tilemap_demo_font.png + _menu.png) and upload each
//      → its own AtlasId. They are IMAGES, never byte arrays.
//   2. loadMapPng the 16-bit grayscale map PNG (_map.png) → an IndexGrid of raw catalog indices.
//   3. Declare a TileCatalog: each entry names a SHEET + a SLOT (8px cell) + a PALETTE + a flip. The
//      menu border uses the FLIP-IRREDUCIBLE minimum — one corner, one horizontal edge, one vertical
//      edge, one fill — and FLIPS produce every other corner/edge (no transformable tile stored twice).
//   4. assembleTilemap(grid, catalog) → the layer's TileCell array + the deduplicated atlas set (font +
//      menu) + palette set. Feed those straight into a TileContent. The cells' atlasSelect chooses the
//      sheet per cell, so font glyphs and menu glyphs render together in the single layer.
//
// Run it on a dev machine: the window shows a bordered menu box (gold frame) with white "HELLO"/"WORLD"
// text inside, on a dark-blue field. The frame's corners/edges are one corner + one edge tile each,
// flipped; the text is a separate font sheet — both in one map. Static image (no animation). Select =
// fullscreen, A = cycle window scale.

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <vector>

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

namespace {
using namespace retropp;

std::string assetPath(const char* name) {
    const char* base = SDL_GetBasePath();  // SDL-owned, do not free (SDL3)
    return (base ? std::string{base} : std::string{}) + "assets/" + name;
}
}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{.window = {.title = "Retro++ — tilemap import demo (font + menu, one map)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    renderer.setSamplingMode(config.enhancements.sampling);
    int windowScale = config.enhancements.windowScale;

    // 1. Load the two atlas IMAGES and upload each → its own sheet (opaque: the dark-blue index-0 box
    //    background unifies the field, so no transparent index is needed).
    AtlasId fontSheet{}, menuSheet{};
    IndexGrid map;
    try {
        const LoadedImage font = loadPng(assetPath("tilemap_demo_font.png"));
        const LoadedImage menu = loadPng(assetPath("tilemap_demo_menu.png"));
        fontSheet = renderer.uploadAtlas(font.indices.data(), font.width, font.height);
        menuSheet = renderer.uploadAtlas(menu.indices.data(), menu.width, menu.height);
        // 2. Load the 16-bit grayscale map → raw catalog indices.
        map = loadMapPng(assetPath("tilemap_demo_map.png"));
    } catch (const std::exception& e) {
        std::printf("demo: could not load tilemap-demo assets: %s\n", e.what());
        return 1;
    }

    // Two 2-entry palettes. Index 0 is the box-background dark blue in BOTH, so the field is seamless;
    // index 1 is the gold border in the menu palette, the white text in the font palette.
    const std::array<Rgba8, 2> menuColours{{ {20, 28, 64}, {235, 200, 90} }};   // bg, gold
    const std::array<Rgba8, 2> textColours{{ {20, 28, 64}, {245, 245, 255} }};  // bg, white
    const PaletteId menuPal = renderer.uploadPalette(std::span<const Rgba8>(menuColours));
    const PaletteId textPal = renderer.uploadPalette(std::span<const Rgba8>(textColours));

    // 3. The TileCatalog (mirrors examples/assets/gen_tilemap_demo.py). Ids are SPARSE 16-bit values
    //    spread across the range (index*4369) — what the 16-bit map carries. Menu border = one corner
    //    + one h-edge + one v-edge + one fill; FLIPS make all the other positions.
    TileCatalog cat;
    cat.entries = {
        {.id = 0,     .sheet = menuSheet, .slot = 3, .palette = menuPal},                              // fill
        {.id = 4369,  .sheet = menuSheet, .slot = 0, .palette = menuPal},                              // corner TL
        {.id = 8738,  .sheet = menuSheet, .slot = 0, .palette = menuPal, .flipX = true},               // corner TR
        {.id = 13107, .sheet = menuSheet, .slot = 0, .palette = menuPal, .flipY = true},               // corner BL
        {.id = 17476, .sheet = menuSheet, .slot = 0, .palette = menuPal, .flipX = true, .flipY = true},// corner BR
        {.id = 21845, .sheet = menuSheet, .slot = 1, .palette = menuPal},                              // top edge
        {.id = 26214, .sheet = menuSheet, .slot = 1, .palette = menuPal, .flipY = true},               // bottom edge
        {.id = 30583, .sheet = menuSheet, .slot = 2, .palette = menuPal},                              // left edge
        {.id = 34952, .sheet = menuSheet, .slot = 2, .palette = menuPal, .flipX = true},               // right edge
        {.id = 39321, .sheet = fontSheet, .slot = 1, .palette = textPal},                              // H
        {.id = 43690, .sheet = fontSheet, .slot = 2, .palette = textPal},                              // E
        {.id = 48059, .sheet = fontSheet, .slot = 3, .palette = textPal},                              // L
        {.id = 52428, .sheet = fontSheet, .slot = 4, .palette = textPal},                              // O
        {.id = 56797, .sheet = fontSheet, .slot = 5, .palette = textPal},                              // W
        {.id = 61166, .sheet = fontSheet, .slot = 6, .palette = textPal},                              // R
        {.id = 65535, .sheet = fontSheet, .slot = 7, .palette = textPal},                              // D
    };

    // 4. Import the map: cells + the deduplicated atlas set (font + menu) + palette set. Kept alive for
    //    the program's duration (the TileContent points its spans at these vectors).
    AssembledTilemap assembled;
    try {
        assembled = assembleTilemap(map, cat);
    } catch (const std::exception& e) {
        std::printf("demo: assembleTilemap failed: %s\n", e.what());
        return 1;
    }
    std::printf("tilemap import: %dx%d tiles, %zu sheets mixed in one map (font + menu), %zu palettes.\n",
                assembled.widthInTiles, assembled.heightInTiles, assembled.atlases.size(), assembled.palettes.size());

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

    // One static tile layer mixing both sheets. The atlas set + palette set come straight from the
    // import; the cells' atlasSelect picks font vs menu per cell.
    FrameDrawState frame;
    loop.setRender([&](float alpha) {
        frame.layers.clear();
        DrawLayer layer{};
        layer.id   = "MenuAndText";
        layer.z    = 0;
        layer.size = PixelSize{config.viewport.width, config.viewport.height};
        // One-call sugar: asTileContent() fills cells/atlases/palettes/dims from the assembled tilemap.
        // (The manual TileContent{ .cells=…, .atlases=… } path stays available for layers that mutate
        // their tiles on the fly.) Blank wrap = finite map (exactly fills the viewport).
        layer.content = assembled.asTileContent(TileWrap::Blank);
        frame.layers.push_back(std::move(layer));
        renderer.renderFrame(frame, alpha);
    });

    std::printf("tilemap import demo — one map, two image atlases (font + menu) mixed; close to quit. "
                "Select = fullscreen, A = cycle window scale.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
