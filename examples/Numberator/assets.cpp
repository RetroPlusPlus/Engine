#include "assets.h"

#include <cstdint>

#include "retropp/asset_policy.h"  // AssetPolicy
#include "retropp/geometry.h"      // AssetDimensions
#include "retropp/renderer.h"      // Renderer, AtlasManifest

namespace numberator {

using namespace retropp;

namespace {
// The spread 16-bit id for a chrome role — the SAME formula the map was written with
// (gen_numberator_assets.py: map_id(role) = role * 65535 / 12). Keeps the catalog and the map in sync.
constexpr std::uint16_t roleId(int role) { return static_cast<std::uint16_t>(role * 65535 / 12); }
}  // namespace

Assets loadAssets(Renderer& r) {
    Assets a;

    a.palette = r.loadPaletteImage("examples/Numberator/assets/numberator_palette.png",
                                   ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);

    const AtlasManifest chrome =
        r.loadAtlas("examples/Numberator/assets/numberator_chrome.png", AssetDimensions::GameBoy8x8,
                    ContentKind::Tileset, ReadOrder::LeftRightThenDown, 0, TransparentIndices::None, 0, AssetPolicy::Embed);
    a.chrome = chrome.atlasId;

    const AtlasManifest buttons =
        r.loadAtlas("examples/Numberator/assets/numberator_buttons.png", AssetDimensions{48, 40},
                    ContentKind::SpriteSeries, ReadOrder::LeftRightThenDown, 0, TransparentIndices::None, 0, AssetPolicy::Embed);
    a.buttons     = buttons.atlasId;
    a.buttonSlots = {buttons.slots.at(0), buttons.slots.at(1)};

    const AtlasManifest font =
        r.loadAtlas("examples/Numberator/assets/numberator_font.png", AssetDimensions{24, 32},
                    ContentKind::SpriteSeries, ReadOrder::LeftRightThenDown, 0, TransparentIndices::None, 0, AssetPolicy::Embed);
    a.font       = font.atlasId;
    a.glyphSlots = font.slots;

    // Index 0 is transparent on this sheet, so the sprite's silhouette (the click target) is the
    // drawn box, not its 16x16 cell.
    const AtlasManifest closeBox =
        r.loadAtlas("examples/Numberator/assets/numberator_closebox.png", AssetDimensions{16, 16},
                    ContentKind::SpriteSeries, ReadOrder::LeftRightThenDown, 0, TransparentIndices::GameBoy, 0, AssetPolicy::Embed);
    a.closeBox     = closeBox.atlasId;
    a.closeBoxSlot = closeBox.slots.at(0);

    // The catalog mirrors the gen script's ROLES: each role's spread id selects a chrome tile slot + flip.
    // The four well corners are ONE corner tile flipped four ways; the edges flip likewise.
    TileCatalog cat;
    cat.entries = {
        {.id = roleId(0),  .sheet = a.chrome, .slot = 0, .palette = a.palette},                            // body
        {.id = roleId(1),  .sheet = a.chrome, .slot = 1, .palette = a.palette},                            // title fill
        {.id = roleId(2),  .sheet = a.chrome, .slot = 2, .palette = a.palette},                            // title separator
        {.id = roleId(3),  .sheet = a.chrome, .slot = 3, .palette = a.palette},                            // close box
        {.id = roleId(4),  .sheet = a.chrome, .slot = 4, .palette = a.palette},                            // well face
        {.id = roleId(5),  .sheet = a.chrome, .slot = 5, .palette = a.palette},                            // well corner
        {.id = roleId(6),  .sheet = a.chrome, .slot = 5, .palette = a.palette, .flipX = true},             // corner, top-right
        {.id = roleId(7),  .sheet = a.chrome, .slot = 5, .palette = a.palette, .flipY = true},             // corner, bottom-left
        {.id = roleId(8),  .sheet = a.chrome, .slot = 5, .palette = a.palette, .flipX = true, .flipY = true},  // bottom-right
        {.id = roleId(9),  .sheet = a.chrome, .slot = 6, .palette = a.palette},                            // h-edge (top)
        {.id = roleId(10), .sheet = a.chrome, .slot = 6, .palette = a.palette, .flipY = true},             // h-edge (bottom)
        {.id = roleId(11), .sheet = a.chrome, .slot = 7, .palette = a.palette},                            // v-edge (left)
        {.id = roleId(12), .sheet = a.chrome, .slot = 7, .palette = a.palette, .flipX = true},             // v-edge (right)
    };

    a.chromeMap = assembleTilemap(
        loadMapPng("examples/Numberator/assets/numberator_map.png", AssetPolicy::Embed), cat);
    return a;
}

}  // namespace numberator
