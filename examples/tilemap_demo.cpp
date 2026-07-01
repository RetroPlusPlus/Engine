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
//   4. assembleTilemap(grid, catalog) → the layer's TileCell array, each cell naming its own sheet +
//      palette directly. Feed that straight into a TileContent. Because each cell names its sheet, font
//      glyphs and menu glyphs render together in the single layer.
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
}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{.window = {.title = "Retro++ — tilemap import demo (font + menu, one map)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    int windowScale = config.enhancements.windowScale;

    // 1. Load the two atlas IMAGES the proper way — loadAtlas decodes the PNG AND slices it into
    //    addressable cells, returning an AtlasManifest { atlas, slots }. (uploadAtlas is only for byte
    //    arrays you specify yourself; a PNG always goes through loadAtlas so its slots come from the
    //    slicing config, never from raw indices.) Both sheets are 8x8 tilesets, read left→right.
    //    Opaque: the dark-blue index-0 box background unifies the field, so no transparent index.
    AtlasManifest fontAtlas, menuAtlas;
    IndexGrid map;
    try {
        fontAtlas = renderer.loadAtlas("assets/tilemap_demo_font.png",
                                       AssetDimensions::GameBoy8x8, ContentKind::Tileset,
                                       ReadOrder::LeftRightThenDown);
        menuAtlas = renderer.loadAtlas("assets/tilemap_demo_menu.png",
                                       AssetDimensions::GameBoy8x8, ContentKind::Tileset,
                                       ReadOrder::LeftRightThenDown);
        // 2. Load the 16-bit grayscale map → raw catalog ids.
        map = loadMapPng("assets/tilemap_demo_map.png");
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
    //    spread across the range (index*4369) — what the 16-bit map carries. `sheet` is a manifest's
    //    atlas and `slot` is one of that manifest's carved slots (`manifest[n].tile`) — the proper
    //    loadAtlas-sliced workflow. Menu border = one corner + one h-edge + one v-edge + one fill;
    //    FLIPS make all the other positions.
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

    // 4. Import the map: the layer's TileCell array, each cell naming its own sheet + palette directly
    //    (font + menu mixed). Kept alive for the program's duration (the TileContent points its span at
    //    this vector).
    AssembledTilemap assembled;
    try {
        assembled = assembleTilemap(map, cat);
    } catch (const std::exception& e) {
        std::printf("demo: assembleTilemap failed: %s\n", e.what());
        return 1;
    }
    std::printf("tilemap import: %dx%d tiles, %zu cells mixing font + menu sheets in one map.\n",
                assembled.widthInTiles, assembled.heightInTiles, assembled.cells.size());

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

    // One static tile layer mixing both sheets. Each assembled cell names its own sheet + palette
    // directly, so font glyphs and menu glyphs render together in the one layer.
    FrameDrawState frame;
    loop.setRender([&]() {
        frame.layers.clear();
        DrawLayer layer{.key = "MenuAndText"};
        layer.z    = 0;
        layer.size = PixelSize{config.viewport.width, config.viewport.height};
        // One-call sugar: asTileContent() fills cells/dims from the assembled tilemap. (The manual
        // TileContent{ .cells = … } path stays available for layers that mutate their tiles on the
        // fly.) Blank wrap = finite map (exactly fills the viewport).
        layer.content = assembled.asTileContent(TileWrap::Blank);
        frame.layers.push_back(std::move(layer));
        renderer.renderFrame(frame);
    });

    std::printf("tilemap import demo — one map, two image atlases (font + menu) mixed; close to quit. "
                "Select = fullscreen, A = cycle window scale.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
