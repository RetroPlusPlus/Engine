#include "retropp/tilemap.h"

#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace retropp {
namespace {

constexpr AtlasId   A0{0};
constexpr AtlasId   A5{5};   // a different sheet (non-contiguous AtlasId, like a real second loadAtlas)
constexpr PaletteId P0{0};
constexpr PaletteId P1{40};  // arbitrary flat palette offset

// ONE map built from ONE catalog mixes tiles from SEVERAL sheets, looked up by SPARSE 16-bit id; each
// cell names its sheet + palette DIRECTLY (no per-layer set, no select). The ids here (4369, 65535)
// are above 255 — the sparse 16-bit values a map must carry to exercise the 16-bit decode at all.
TEST(AssembleTilemap, MixesMultipleSheetsBySparseId) {
    TileCatalog cat;
    cat.entries = {
        {.id = 0,     .sheet = A0, .slot = 10, .palette = P0},                    // id 0     → sheet A0
        {.id = 4369,  .sheet = A5, .slot = 3,  .palette = P1, .flipX = true},     // id 4369  → A5, flipped
        {.id = 65535, .sheet = A0, .slot = 10, .palette = P0, .flipY = true},     // id 65535 → A0 reused
    };
    const IndexGrid map{2, 2, {0, 4369, 65535, 0}};

    const AssembledTilemap built = assembleTilemap(map, cat);

    EXPECT_EQ(built.widthInTiles, 2);
    EXPECT_EQ(built.heightInTiles, 2);
    ASSERT_EQ(built.cells.size(), 4u);

    // cell 0 (id 0): A0 slot 10, palette P0, no flip.
    EXPECT_EQ(built.cells[0].tile, 10);
    EXPECT_EQ(built.cells[0].atlas, A0);
    EXPECT_EQ(built.cells[0].palette, P0);
    EXPECT_FALSE(built.cells[0].flipX);
    // cell 1 (id 4369): A5 slot 3, palette P1, flipX — THE multi-sheet, sparse-16-bit-id cell.
    EXPECT_EQ(built.cells[1].tile, 3);
    EXPECT_EQ(built.cells[1].atlas, A5);
    EXPECT_EQ(built.cells[1].palette, P1);
    EXPECT_TRUE(built.cells[1].flipX);
    // cell 2 (id 65535): A0 slot 10 reused via flipY.
    EXPECT_EQ(built.cells[2].tile, 10);
    EXPECT_EQ(built.cells[2].atlas, A0);
    EXPECT_TRUE(built.cells[2].flipY);
    // cell 3 (id 0 again): A0.
    EXPECT_EQ(built.cells[3].atlas, A0);
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
    for (const TileCell& c : built.cells) {
        EXPECT_EQ(c.tile, 7);     // one source slot for all three
        EXPECT_EQ(c.atlas, A0);   // one sheet
    }
    EXPECT_FALSE(built.cells[0].flipX);
    EXPECT_TRUE(built.cells[1].flipX);
    EXPECT_TRUE(built.cells[2].flipX);
    EXPECT_TRUE(built.cells[2].flipY);
}

// Rotation rides from the catalog entry onto the emitted cell, alongside the flips — one source slot
// serves every orientation of square art (the four corners of a box from one corner tile).
TEST(AssembleTilemap, RotationReusesTheSameSlot) {
    TileCatalog cat;
    cat.entries = {
        {.id = 0, .sheet = A0, .slot = 7, .palette = P0},                                     // corner as-is
        {.id = 1, .sheet = A0, .slot = 7, .palette = P0, .rotation = Rotation::Rot90},        // +90°
        {.id = 2, .sheet = A0, .slot = 7, .palette = P0, .rotation = Rotation::Rot180},       // +180°
        {.id = 3, .sheet = A0, .slot = 7, .palette = P0, .flipX = true, .rotation = Rotation::Rot270},
    };
    const IndexGrid map{4, 1, {0, 1, 2, 3}};
    const AssembledTilemap built = assembleTilemap(map, cat);

    ASSERT_EQ(built.cells.size(), 4u);
    for (const TileCell& c : built.cells) {
        EXPECT_EQ(c.tile, 7);     // one source slot for all four
        EXPECT_EQ(c.atlas, A0);
    }
    EXPECT_EQ(built.cells[0].rotation, Rotation::None);
    EXPECT_EQ(built.cells[1].rotation, Rotation::Rot90);
    EXPECT_EQ(built.cells[2].rotation, Rotation::Rot180);
    EXPECT_EQ(built.cells[3].rotation, Rotation::Rot270);
    EXPECT_TRUE(built.cells[3].flipX);   // rotation composes with the flip
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

// No per-layer cap: a map may mix far more than the former 16 sheets — 20 distinct sheets here, each
// carried as a direct handle, with no throw.
TEST(AssembleTilemap, MixesManySheetsWithNoCap) {
    TileCatalog cat;
    std::vector<std::uint16_t> values;
    for (std::uint16_t i = 0; i < 20; ++i) {
        cat.entries.push_back({.id = i, .sheet = static_cast<AtlasId>(i), .slot = 0, .palette = P0});
        values.push_back(i);
    }
    const IndexGrid map{static_cast<int>(values.size()), 1, values};
    const AssembledTilemap built = assembleTilemap(map, cat);
    ASSERT_EQ(built.cells.size(), 20u);
    EXPECT_EQ(built.cells[0].atlas, static_cast<AtlasId>(0));
    EXPECT_EQ(built.cells[19].atlas, static_cast<AtlasId>(19));
}

TEST(AssembleTilemap, EmptyGridYieldsEmptyResult) {
    TileCatalog cat;
    cat.entries = {{.id = 0, .sheet = A0, .slot = 0, .palette = P0}};
    const AssembledTilemap built = assembleTilemap(IndexGrid{0, 0, {}}, cat);
    EXPECT_TRUE(built.cells.empty());
    EXPECT_EQ(built.widthInTiles, 0);
}

// tiles(): the single-combo convenience — fills the repeated atlas + palette over a run of slots,
// returning plain mutable cells (no flip, ready to edit).
TEST(Tiles, FillsAtlasAndPaletteOverSlots) {
    const std::vector<TileCell> cells = tiles(A5, P1, {5, 6, 7});
    ASSERT_EQ(cells.size(), 3u);
    EXPECT_EQ(cells[0].tile, 5);
    EXPECT_EQ(cells[1].tile, 6);
    EXPECT_EQ(cells[2].tile, 7);
    for (const TileCell& c : cells) {
        EXPECT_EQ(c.atlas, A5);
        EXPECT_EQ(c.palette, P1);
        EXPECT_FALSE(c.flipX);
        EXPECT_FALSE(c.flipY);
    }
}

TEST(Tiles, EmptyListYieldsNoCells) {
    EXPECT_TRUE(tiles(A0, P0, {}).empty());
}

}  // namespace
}  // namespace retropp
