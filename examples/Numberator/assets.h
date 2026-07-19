// Numberator — asset loading. Every asset is a PNG the build EMBEDS (baked into the binary): the shared
// palette image, the chrome tile sheet, the chrome map, the key sprites, and the glyph font. The chrome
// layer is assembled here from the map PNG + a TileCatalog (the engine's image-driven tilemap path), so
// the layout lives in the map, not in code.
#pragma once

#include <array>
#include <vector>

#include "retropp/image.h"     // AssetSlot, AtlasId
#include "retropp/palette.h"   // PaletteId
#include "retropp/tilemap.h"   // AssembledTilemap

namespace retropp {
class Renderer;
}

namespace numberator {

struct Assets {
    retropp::PaletteId palette{};   // one shared palette (entry 0 is alpha 0 = transparent)
    retropp::AtlasId   chrome{};    // the chrome tile sheet
    retropp::AtlasId   buttons{};   // the key sprites
    retropp::AtlasId   font{};      // the glyph sprites
    retropp::AtlasId   closeBox{};  // the title bar's close-box sprite

    std::array<retropp::AssetSlot, 2> buttonSlots{};  // 0 = number key, 1 = function key
    std::vector<retropp::AssetSlot>   glyphSlots;       // 20 glyphs, in font order
    retropp::AssetSlot                closeBoxSlot{};   // the close box (its one sprite cell)
    retropp::AssembledTilemap         chromeMap;        // the assembled chrome layer (its cells back a span)
};

// Load + embed every asset and assemble the chrome tilemap. Throws std::runtime_error on a load failure.
Assets loadAssets(retropp::Renderer& renderer);

}  // namespace numberator
