#include "retropp/tilemap.h"

#include <stdexcept>
#include <string>
#include <unordered_map>

namespace retropp {

namespace {

// First-seen dedup: return the index of `value` in `set`, appending it if new. The returned index is
// the per-cell SELECT (atlasSelect / palette-select) into the layer's set. Throws std::length_error
// (naming the resource) once the set would exceed `cap` — a single tile layer's set capacity.
template <typename T>
std::uint8_t internOrThrow(std::vector<T>& set, T value, std::size_t cap, const char* what) {
    for (std::size_t i = 0; i < set.size(); ++i) {
        if (set[i] == value) return static_cast<std::uint8_t>(i);
    }
    if (set.size() >= cap) {
        throw std::length_error(std::string{"assembleTilemap: map uses more distinct "} + what +
                                " than a tile layer's set holds (" + std::to_string(cap) + ")");
    }
    set.push_back(value);
    return static_cast<std::uint8_t>(set.size() - 1);
}

}  // namespace

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

    out.cells.reserve(map.values.size());
    for (const std::uint16_t v : map.values) {
        const auto it = byId.find(v);
        if (it == byId.end()) {
            throw std::out_of_range("assembleTilemap: map value " + std::to_string(v) +
                                    " has no TileCatalog entry");
        }
        const TileCatalogEntry& e = *it->second;
        const std::uint8_t atlasSel = internOrThrow(out.atlases, e.sheet, kAtlasSetSlots, "sheets");
        const std::uint8_t palSel   = internOrThrow(out.palettes, e.palette, kPaletteSetSlots, "palettes");
        out.cells.push_back(TileCell{e.slot, palSel, e.flipX, e.flipY, atlasSel});
    }
    return out;
}

}  // namespace retropp
