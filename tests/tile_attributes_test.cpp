#include "gbcpp/draw_state.h"
#include "gbcpp/palette.h"

#include <array>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

namespace gbcpp {

// The R32_UINT tilemap cell the tile shader unpacks: [tile:16][palette:8][flipX:1][flipY:1]
// [reserved:6]. packTileCell / unpackTileCell are the constexpr mirror of that GPU layout; the
// shader unpacks the identical bits.

TEST(TileCell, PackLayoutHasLockedBitPositions) {
    // tile in the low 16 bits.
    EXPECT_EQ(packTileCell(TileCell{0x1234, 0, false, false}), 0x00001234u);
    // palette in bits 16..23.
    EXPECT_EQ(packTileCell(TileCell{0, 0xAB, false, false}), 0x00AB0000u);
    // flipX is bit 24, flipY is bit 25.
    EXPECT_EQ(packTileCell(TileCell{0, 0, true, false}), 0x01000000u);
    EXPECT_EQ(packTileCell(TileCell{0, 0, false, true}), 0x02000000u);
    // combined.
    EXPECT_EQ(packTileCell(TileCell{0xFFFF, 0xFF, true, true}), 0x03FFFFFFu);
}

TEST(TileCell, UnpackReadsEachField) {
    const TileCell c = unpackTileCell(0x03ABCDEFu);
    EXPECT_EQ(c.tile, 0xCDEF);
    EXPECT_EQ(c.palette, 0xAB);
    EXPECT_TRUE(c.flipX);
    EXPECT_TRUE(c.flipY);
}

TEST(TileCell, PackUnpackRoundTripsAcrossFieldRanges) {
    for (std::uint32_t tile : {std::uint32_t{0}, std::uint32_t{1}, std::uint32_t{0x7FFF},
                              std::uint32_t{0x8000}, std::uint32_t{0xFFFF}}) {
        for (std::uint32_t pal : {std::uint32_t{0}, std::uint32_t{1}, std::uint32_t{7},
                                 std::uint32_t{0x80}, std::uint32_t{0xFF}}) {
            for (int fx = 0; fx < 2; ++fx) {
                for (int fy = 0; fy < 2; ++fy) {
                    const TileCell in{static_cast<std::uint16_t>(tile),
                                      static_cast<std::uint8_t>(pal),
                                      fx != 0, fy != 0};
                    const TileCell out = unpackTileCell(packTileCell(in));
                    EXPECT_EQ(out.tile, in.tile);
                    EXPECT_EQ(out.palette, in.palette);
                    EXPECT_EQ(out.flipX, in.flipX);
                    EXPECT_EQ(out.flipY, in.flipY);
                }
            }
        }
    }
}

TEST(TileCell, ReservedBitsAreZeroAndUnusedByUnpack) {
    // Nothing the packer writes touches bits 26..31; and unpack ignores them (forward room for
    // `priority` in ENG-2.B.2.c without a format change).
    EXPECT_EQ(packTileCell(TileCell{0xFFFF, 0xFF, true, true}) & 0xFC000000u, 0u);
    const TileCell c = unpackTileCell(0xFC000000u);  // only reserved bits set
    EXPECT_EQ(c.tile, 0);
    EXPECT_EQ(c.palette, 0);
    EXPECT_FALSE(c.flipX);
    EXPECT_FALSE(c.flipY);
}

TEST(TileCell, PackIsConstexpr) {
    constexpr std::uint32_t packed = packTileCell(TileCell{0x00C8, 0x05, false, true});
    static_assert((packed & 0xFFFF) == 0x00C8, "tile bits");
    static_assert(((packed >> 16) & 0xFF) == 0x05, "palette bits");
    static_assert(((packed >> 25) & 1) == 1, "flipY bit");
    EXPECT_EQ(unpackTileCell(packed).tile, 0x00C8);
}

// paletteSetRows mirrors the compositor's per-layer uSetRows fill: slot i = the store row of
// palettes[i] (a PaletteId's underlying value IS its row), 0 beyond the set.

TEST(PaletteSetRows, EmptySetIsAllZeroAndValid) {
    const auto rows = paletteSetRows(std::span<const PaletteId>{});
    for (std::uint32_t r : rows) EXPECT_EQ(r, 0u);
}

TEST(PaletteSetRows, MapsEachHandleToItsStoreRow) {
    const std::array<PaletteId, 3> set{PaletteId{5}, PaletteId{2}, PaletteId{9}};
    const auto rows = paletteSetRows(std::span<const PaletteId>(set));
    EXPECT_EQ(rows[0], 5u);
    EXPECT_EQ(rows[1], 2u);
    EXPECT_EQ(rows[2], 9u);
    EXPECT_EQ(rows[3], 0u);  // beyond the set
    EXPECT_EQ(rows[kPaletteSetSlots - 1], 0u);
}

TEST(PaletteSetRows, AllowsRepeatedHandles) {
    const std::array<PaletteId, 4> set{PaletteId{7}, PaletteId{7}, PaletteId{0}, PaletteId{7}};
    const auto rows = paletteSetRows(std::span<const PaletteId>(set));
    EXPECT_EQ(rows[0], 7u);
    EXPECT_EQ(rows[1], 7u);
    EXPECT_EQ(rows[2], 0u);
    EXPECT_EQ(rows[3], 7u);
}

TEST(PaletteSetRows, TruncatesSetsLargerThanK) {
    std::array<PaletteId, kPaletteSetSlots + 4> set{};
    for (std::size_t i = 0; i < set.size(); ++i) set[i] = static_cast<PaletteId>(i + 1);
    const auto rows = paletteSetRows(std::span<const PaletteId>(set));
    EXPECT_EQ(rows.size(), kPaletteSetSlots);
    EXPECT_EQ(rows[0], 1u);
    EXPECT_EQ(rows[kPaletteSetSlots - 1], static_cast<std::uint32_t>(kPaletteSetSlots));
}

}  // namespace gbcpp
