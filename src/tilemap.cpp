#include "retropp/tilemap.h"

#include <stdexcept>
#include <string>
#include <unordered_map>

namespace retropp {

AssembledTilemap assembleTilemap(const IndexGrid& map, const TileCatalog& catalog) {
    AssembledTilemap out;
    out.widthInTiles  = map.width;
    out.heightInTiles = map.height;
    if (map.width <= 0 || map.height <= 0) return out;  // degenerate grid → empty (valid)

    // Ids are SPARSE 16-bit values, so look entries up BY id (not by position). Duplicate ids are a
    // catalog authoring error — two tiles can't answer to the same map value.
    std::unordered_map<std::uint16_t, const TileCatalogEntry*> byId;
    byId.reserve(catalog.entries.size());
    for (const TileCatalogEntry& e : catalog.entries) {
        if (!byId.emplace(e.id, &e).second) {
            throw std::invalid_argument("assembleTilemap: duplicate TileCatalog id " + std::to_string(e.id));
        }
    }

    // Each grid value selects its entry by id; the entry's sheet, palette, slot, flip, and rotation ride
    // directly onto the cell — no per-layer set, no select.
    out.cells.reserve(map.values.size());
    for (const std::uint16_t v : map.values) {
        const auto it = byId.find(v);
        if (it == byId.end()) {
            throw std::out_of_range("assembleTilemap: map value " + std::to_string(v) +
                                    " has no TileCatalog entry");
        }
        const TileCatalogEntry& e = *it->second;
        out.cells.push_back(TileCell{.tile = e.slot, .atlas = e.sheet, .palette = e.palette,
                                     .flipX = e.flipX, .flipY = e.flipY, .rotation = e.rotation});
    }
    return out;
}

std::vector<TileCell> tiles(AtlasId atlas, PaletteId palette,
                            std::initializer_list<std::uint16_t> slots) {
    std::vector<TileCell> cells;
    cells.reserve(slots.size());
    for (const std::uint16_t slot : slots) {
        cells.push_back(TileCell{.tile = slot, .atlas = atlas, .palette = palette});
    }
    return cells;
}

}  // namespace retropp
