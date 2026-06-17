#pragma once

#include <cstdint>
#include <vector>

#include "retropp/draw_state.h"  // TileCell, AtlasId
#include "retropp/image.h"       // IndexGrid, AtlasId
#include "retropp/palette.h"     // PaletteId

namespace retropp {

// ── Tilemap image import: a map IndexGrid + a TileCatalog → a tile layer's cells (ENG-2.L) ───────
//
// A TileCatalog is a plain declared registry of distinct tiles a map can place. Each entry names a
// SHEET (an AtlasId — any loaded atlas), a SLOT within it (the 8px atlas cell index, i.e. an
// AssetSlot::tile), the PALETTE that colours it, and a flip. Catalog ENTRY INDEX i is the raw value
// a map pixel holds: a 16-bit grayscale map PNG decodes (loadMapPng → IndexGrid) to a grid of these
// indices, and buildTilemap() turns that grid into the layer's TileCell array.
//
// The whole point of ENG-2.L: the catalog spans MANY sheets, and ONE map mixes tiles from several of
// them. buildTilemap collects the distinct sheets a map actually uses into the layer's atlas SET
// (TileContent::atlases) and sets each cell's atlasSelect accordingly — so a font sheet and a menu
// sheet (say) render together in a single tile layer. Palettes dedup the same way. No tile is ever
// repeated that a flip can produce: an entry reuses a sheet slot with flipX/flipY to get the mirror.
struct TileCatalogEntry {
    std::uint16_t id     = 0;       // the map VALUE that selects this tile — identity, first field. Ids
                                    // are arbitrary 16-bit values (SPARSE: a map can spread them across
                                    // the whole 0..65535 range, which is exactly what exercises a 16-bit
                                    // map — values above 255 an 8-bit decode would misread).
    AtlasId       sheet{};          // which loaded atlas this tile comes from
    std::uint16_t slot   = 0;       // the 8px cell index within that sheet (an AssetSlot::tile)
    PaletteId     palette{};        // which uploaded palette colours this tile
    bool          flipX  = false;
    bool          flipY  = false;
};

// The catalog: a map VALUE selects the entry whose `id` matches it (NOT positional — ids are sparse).
// A plain declared object — build it inline with designated initializers.
struct TileCatalog {
    std::vector<TileCatalogEntry> entries;
};

// The result of assembling a map: the layer's TileCell array plus the deduplicated atlas + palette
// SETS the cells select within (feed straight into a TileContent — atlases→atlases, palettes→
// palettes, cells→cells). Owns its vectors; a TileContent points its spans at them, so a AssembledTilemap
// must outlive the renderFrame() that consumes the TileContent (the usual game-owned-data lifetime).
struct AssembledTilemap {
    std::vector<TileCell>  cells;          // row-major widthInTiles * heightInTiles
    std::vector<AtlasId>   atlases;        // the layer's atlas set (TileContent::atlases) — first-seen order
    std::vector<PaletteId> palettes;       // the layer's palette set (TileContent::palettes) — first-seen order
    int                    widthInTiles  = 0;
    int                    heightInTiles = 0;

    // Optional syntactic sugar: a TileContent wired to display this build — fills cells/atlases/palettes/
    // dimensions in one call instead of threading each by hand. `wrap` is the one display choice the
    // build doesn't carry. The returned spans point INTO this BuiltTilemap, so it must outlive the
    // TileContent (same game-owned-data lifetime as the fields). The manual cells/atlases/palettes path
    // on TileContent stays first-class — set them directly when a layer mutates its tilemap on the fly.
    [[nodiscard]] TileContent asTileContent(TileWrap wrap = TileWrap::Repeat) const {
        return TileContent{
            .palettes      = palettes,
            .widthInTiles  = widthInTiles,
            .heightInTiles = heightInTiles,
            .cells         = cells,
            .wrap          = wrap,
            .atlases       = atlases,
        };
    }
};

// Assemble a map IndexGrid into a tile layer's data via a TileCatalog. Each grid value selects the
// entry whose `id` matches (sparse 16-bit ids); the entry's sheet/palette join first-seen-ordered SETS
// (so a map mixing several sheets yields one atlas set with each cell's atlasSelect pointing into it),
// and the entry's slot/flip ride onto the cell. Throws std::out_of_range if a grid value matches no
// catalog id, std::invalid_argument on a duplicate catalog id, and std::length_error if the map's
// distinct sheets or palettes exceed kAtlasSetSlots / kPaletteSetSlots (a single tile layer's set
// capacity). A degenerate (empty) grid yields an empty result.
[[nodiscard]] AssembledTilemap assembleTilemap(const IndexGrid& map, const TileCatalog& catalog);

}  // namespace retropp
