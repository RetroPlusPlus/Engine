#include "retropp/tilemap.h"

#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace retropp {
namespace {

constexpr AtlasId   A0{0};
constexpr AtlasId   A5{5};   // a different sheet (non-contiguous AtlasId, like a real second loadAtlas)
constexpr PaletteId P0{0};
constexpr PaletteId P1{40};  // arbitrary flat palette offset (ENG-2.K)

// The ENG-2.L headline: ONE map built from ONE catalog mixes tiles from SEVERAL sheets, looked up by
// SPARSE 16-bit id. The distinct sheets dedup into the layer's atlas set in first-seen order; each
// cell's atlasSelect indexes that set. The ids here (4369, 65535) are above 255 — the sparse 16-bit
// values that a map must carry to exercise the 16-bit decode at all.
TEST(AssembleTilemap, MixesMultipleSheetsIntoOneAtlasSetBySparseId) {
    TileCatalog cat;
    cat.entries = {
        {.id = 0,     .sheet = A0, .slot = 10, .palette = P0},                    // id 0     → sheet A0
        {.id = 4369,  .sheet = A5, .slot = 3,  .palette = P1, .flipX = true},     // id 4369  → A5, flipped
        {.id = 65535, .sheet = A0, .slot = 10, .palette = P0, .flipY = true},     // id 65535 → A0 reused
    };
    const IndexGrid map{2, 2, {0, 4369, 65535, 0}};

    const AssembledTilemap built = assembleTilemap(map, cat);

    // Atlas set = the distinct sheets, first-seen order: A0 then A5.
    ASSERT_EQ(built.atlases.size(), 2u);
    EXPECT_EQ(built.atlases[0], A0);
    EXPECT_EQ(built.atlases[1], A5);
    // Palette set, first-seen: P0 then P1.
    ASSERT_EQ(built.palettes.size(), 2u);
    EXPECT_EQ(built.palettes[0], P0);
    EXPECT_EQ(built.palettes[1], P1);

    EXPECT_EQ(built.widthInTiles, 2);
    EXPECT_EQ(built.heightInTiles, 2);
    ASSERT_EQ(built.cells.size(), 4u);

    // cell 0 (id 0): A0 slot 10, palette select 0, no flip → atlasSelect 0.
    EXPECT_EQ(built.cells[0].tile, 10);
    EXPECT_EQ(built.cells[0].palette, 0);
    EXPECT_EQ(built.cells[0].atlasSelect, 0);
    EXPECT_FALSE(built.cells[0].flipX);
    // cell 1 (id 4369): A5 → atlasSelect 1, palette 1, flipX. THE multi-sheet, sparse-16-bit-id cell.
    EXPECT_EQ(built.cells[1].tile, 3);
    EXPECT_EQ(built.cells[1].palette, 1);
    EXPECT_EQ(built.cells[1].atlasSelect, 1);
    EXPECT_TRUE(built.cells[1].flipX);
    // cell 2 (id 65535): A0 slot 10 reused via flipY → atlasSelect back to 0 (NOT a new set slot).
    EXPECT_EQ(built.cells[2].tile, 10);
    EXPECT_EQ(built.cells[2].atlasSelect, 0);
    EXPECT_TRUE(built.cells[2].flipY);
    // cell 3 (id 0 again): A0.
    EXPECT_EQ(built.cells[3].atlasSelect, 0);
}

// A flip reuses a sheet slot rather than adding a tile — the catalog never repeats a transformable tile.
TEST(AssembleTilemap, FlipsReuseTheSameSlot) {
    TileCatalog cat;
    cat.entries = {
        {.id = 100, .sheet = A0, .slot = 7, .palette = P0},                           // corner
        {.id = 101, .sheet = A0, .slot = 7, .palette = P0, .flipX = true},            // mirrored X
        {.id = 102, .sheet = A0, .slot = 7, .palette = P0, .flipX = true, .flipY = true},  // mirrored both
    };
    const IndexGrid map{3, 1, {100, 101, 102}};
    const AssembledTilemap built = assembleTilemap(map, cat);

    ASSERT_EQ(built.cells.size(), 3u);
    for (const TileCell& c : built.cells) EXPECT_EQ(c.tile, 7);  // one source slot for all three
    EXPECT_FALSE(built.cells[0].flipX);
    EXPECT_TRUE(built.cells[1].flipX);
    EXPECT_TRUE(built.cells[2].flipX);
    EXPECT_TRUE(built.cells[2].flipY);
    EXPECT_EQ(built.atlases.size(), 1u);  // one sheet
}

TEST(AssembleTilemap, UnknownMapValueThrows) {
    TileCatalog cat;
    cat.entries = {{.id = 0, .sheet = A0, .slot = 0, .palette = P0}};  // only id 0 defined
    const IndexGrid map{1, 1, {7}};                                     // value 7 has no entry
    EXPECT_THROW((void)assembleTilemap(map, cat), std::out_of_range);
}

TEST(AssembleTilemap, DuplicateIdThrows) {
    TileCatalog cat;
    cat.entries = {
        {.id = 5, .sheet = A0, .slot = 0, .palette = P0},
        {.id = 5, .sheet = A5, .slot = 1, .palette = P1},  // same id twice
    };
    EXPECT_THROW((void)assembleTilemap(IndexGrid{1, 1, {5}}, cat), std::invalid_argument);
}

TEST(AssembleTilemap, TooManyDistinctSheetsThrows) {
    // 17 distinct sheets in one map exceeds kAtlasSetSlots (16) → length_error.
    TileCatalog cat;
    std::vector<std::uint16_t> values;
    for (std::uint16_t i = 0; i <= kAtlasSetSlots; ++i) {  // 0..16 = 17 entries/sheets
        cat.entries.push_back({.id = i, .sheet = static_cast<AtlasId>(i), .slot = 0, .palette = P0});
        values.push_back(i);
    }
    const IndexGrid map{static_cast<int>(values.size()), 1, values};
    EXPECT_THROW((void)assembleTilemap(map, cat), std::length_error);
}

TEST(AssembleTilemap, EmptyGridYieldsEmptyResult) {
    TileCatalog cat;
    cat.entries = {{.id = 0, .sheet = A0, .slot = 0, .palette = P0}};
    const AssembledTilemap built = assembleTilemap(IndexGrid{0, 0, {}}, cat);
    EXPECT_TRUE(built.cells.empty());
    EXPECT_TRUE(built.atlases.empty());
    EXPECT_EQ(built.widthInTiles, 0);
}

}  // namespace
}  // namespace retropp
