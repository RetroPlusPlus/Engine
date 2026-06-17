#include "retropp/draw_state.h"
#include "retropp/palette.h"

#include <array>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

namespace retropp {

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
    // combined (all fields set).
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

TEST(TileCell, ReservedBitsAboveFlipsAreUnused) {
    // Bits 26..31 remain reserved: nothing the packer writes touches them, and unpack ignores them.
    EXPECT_EQ(packTileCell(TileCell{0xFFFF, 0xFF, true, true}) & 0xFC000000u, 0u);
    const TileCell c = unpackTileCell(0xFC000000u);  // only the reserved bits set
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

// paletteSetOffsets mirrors the compositor's per-layer uSetOffsets fill: slot i = the flat offset
// of palettes[i] (a PaletteId's underlying value IS its flat offset into the palette store), 0
// beyond the set.

TEST(PaletteSetOffsets, EmptySetIsAllZeroAndValid) {
    const auto offsets = paletteSetOffsets(std::span<const PaletteId>{});
    for (std::uint32_t o : offsets) EXPECT_EQ(o, 0u);
}

TEST(PaletteSetOffsets, MapsEachHandleToItsFlatOffset) {
    const std::array<PaletteId, 3> set{PaletteId{5}, PaletteId{2}, PaletteId{9}};
    const auto offsets = paletteSetOffsets(std::span<const PaletteId>(set));
    EXPECT_EQ(offsets[0], 5u);
    EXPECT_EQ(offsets[1], 2u);
    EXPECT_EQ(offsets[2], 9u);
    EXPECT_EQ(offsets[3], 0u);  // beyond the set
    EXPECT_EQ(offsets[kPaletteSetSlots - 1], 0u);
}

TEST(PaletteSetOffsets, AllowsRepeatedHandles) {
    const std::array<PaletteId, 4> set{PaletteId{7}, PaletteId{7}, PaletteId{0}, PaletteId{7}};
    const auto offsets = paletteSetOffsets(std::span<const PaletteId>(set));
    EXPECT_EQ(offsets[0], 7u);
    EXPECT_EQ(offsets[1], 7u);
    EXPECT_EQ(offsets[2], 0u);
    EXPECT_EQ(offsets[3], 7u);
}

TEST(PaletteSetOffsets, TruncatesSetsLargerThanK) {
    std::array<PaletteId, kPaletteSetSlots + 4> set{};
    for (std::size_t i = 0; i < set.size(); ++i) set[i] = static_cast<PaletteId>(i + 1);
    const auto offsets = paletteSetOffsets(std::span<const PaletteId>(set));
    EXPECT_EQ(offsets.size(), kPaletteSetSlots);
    EXPECT_EQ(offsets[0], 1u);
    EXPECT_EQ(offsets[kPaletteSetSlots - 1], static_cast<std::uint32_t>(kPaletteSetSlots));
}

// Arbitrary-size palettes (ENG-2.K): a PaletteId is a flat offset, so it carries values far beyond
// the former 256-colour cap. The set/sprite resolvers pass any 32-bit offset through unchanged.
TEST(PaletteSetOffsets, CarriesOffsetsBeyondTheFormer256Cap) {
    const std::array<PaletteId, 3> set{PaletteId{300}, PaletteId{70000}, PaletteId{1000000}};
    const auto offsets = paletteSetOffsets(std::span<const PaletteId>(set));
    EXPECT_EQ(offsets[0], 300u);
    EXPECT_EQ(offsets[1], 70000u);     // beyond an 8-bit index
    EXPECT_EQ(offsets[2], 1000000u);   // beyond a 16-bit index
}

// paletteStoreTexel mirrors the shaders' flat palette lookup: (offset + index) wraps to (flat % W,
// flat / W) in the W-wide flat store. A palette may straddle rows; capacity is W × height, not 256.

TEST(PaletteStoreTexel, MapsFlatPositionIntoTheWideStore) {
    // Within the first row.
    EXPECT_EQ(paletteStoreTexel(0u, 5u, 16384u), (PaletteTexel{5u, 0u}));
    EXPECT_EQ(paletteStoreTexel(300u, 7u, 16384u), (PaletteTexel{307u, 0u}));
}

TEST(PaletteStoreTexel, WrapsAcrossRows) {
    // W = 256 here (a small store) to exercise the row wrap cheaply. flat 256 → (0,1), 257 → (1,1).
    EXPECT_EQ(paletteStoreTexel(250u, 6u, 256u), (PaletteTexel{0u, 1u}));
    EXPECT_EQ(paletteStoreTexel(250u, 7u, 256u), (PaletteTexel{1u, 1u}));
    // A palette straddling a row boundary: offset 254, indices 0..3 cross from row 0 into row 1.
    EXPECT_EQ(paletteStoreTexel(254u, 1u, 256u), (PaletteTexel{255u, 0u}));
    EXPECT_EQ(paletteStoreTexel(254u, 2u, 256u), (PaletteTexel{0u, 1u}));
}

TEST(PaletteStoreTexel, AddressesArbitraryLargePalettes) {
    // A colour index of 65535 into a palette at offset 200000, store width 16384.
    const std::uint32_t flat = 200000u + 65535u;  // 265535
    EXPECT_EQ(paletteStoreTexel(200000u, 65535u, 16384u),
              (PaletteTexel{flat % 16384u, flat / 16384u}));
}

TEST(PaletteStoreTexel, IsConstexpr) {
    static_assert(paletteStoreTexel(0u, 0u, 16384u) == PaletteTexel{0u, 0u});
    static_assert(paletteStoreTexel(16384u, 0u, 16384u) == PaletteTexel{0u, 1u});
    SUCCEED();
}

}  // namespace retropp
