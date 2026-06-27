// TransparentIndices — the per-sheet structural transparent-index set. Pure CPU tests of the value
// type (no GPU device): the named presets, the of() builder, the 64-index cap, and the 64-bit mask the
// renderer splits across the atlas-region table's two words. The shader-side discard the mask drives is
// covered device-side by the golden readback harness and atlas_transparency_test.

#include <cstdint>

#include <gtest/gtest.h>

#include "retropp/image.h"

namespace {
using namespace retropp;

TEST(TransparentIndices, NoneIsTheEmptySet) {
    EXPECT_EQ(TransparentIndices::None.mask, 0u);
    EXPECT_FALSE(TransparentIndices::None.contains(0));
    EXPECT_FALSE(TransparentIndices::None.contains(5));
}

TEST(TransparentIndices, GameBoyHolesIndexZeroOnly) {
    EXPECT_EQ(TransparentIndices::GameBoy.mask, 1u);
    EXPECT_TRUE(TransparentIndices::GameBoy.contains(0));
    EXPECT_FALSE(TransparentIndices::GameBoy.contains(1));
}

TEST(TransparentIndices, GameBoyColorEqualsGameBoy) {
    EXPECT_EQ(TransparentIndices::GameBoyColor, TransparentIndices::GameBoy);
}

TEST(TransparentIndices, OfBuildsAnArbitrarySet) {
    const TransparentIndices t = TransparentIndices::of({2, 5});
    EXPECT_TRUE(t.contains(2));
    EXPECT_TRUE(t.contains(5));
    EXPECT_FALSE(t.contains(0));
    EXPECT_FALSE(t.contains(3));
    EXPECT_EQ(t.mask, (std::uint64_t{1} << 2) | (std::uint64_t{1} << 5));
}

TEST(TransparentIndices, OfEmptyEqualsNone) {
    EXPECT_EQ(TransparentIndices::of({}), TransparentIndices::None);
}

TEST(TransparentIndices, OfIndexZeroEqualsGameBoy) {
    EXPECT_EQ(TransparentIndices::of({0}), TransparentIndices::GameBoy);
}

TEST(TransparentIndices, HighIndicesLandInTheHighWord) {
    // Indices 32–63 set bits in the high 32 of the mask — the word the renderer maps to region.w; the
    // low word (region.z) holds 0–31.
    const TransparentIndices t = TransparentIndices::of({32, 63});
    EXPECT_TRUE(t.contains(32));
    EXPECT_TRUE(t.contains(63));
    EXPECT_EQ(t.mask >> 32, (std::uint32_t{1} << 0) | (std::uint32_t{1} << 31));
    EXPECT_EQ(t.mask & 0xFFFFFFFFu, 0u);
}

TEST(TransparentIndices, IndicesAtOrAboveSixtyFourAreDropped) {
    // 64 and beyond cannot be a structural hole (they stay expressible only via palette alpha); of()
    // drops them rather than aliasing a low bit.
    EXPECT_EQ(TransparentIndices::of({64}).mask, 0u);
    EXPECT_EQ(TransparentIndices::of({200}).mask, 0u);
    EXPECT_FALSE(TransparentIndices::of({64}).contains(64));
    // A valid index alongside an out-of-range one keeps only the valid bit.
    EXPECT_EQ(TransparentIndices::of({3, 64}).mask, std::uint64_t{1} << 3);
}

TEST(TransparentIndices, NegativeIndicesAreDropped) {
    EXPECT_EQ(TransparentIndices::of({-1}).mask, 0u);
    EXPECT_FALSE(TransparentIndices::of({-1}).contains(-1));
}

TEST(TransparentIndices, ResolvesAtCompileTime) {
    static_assert(TransparentIndices::None.mask == 0u);
    static_assert(TransparentIndices::GameBoy.mask == 1u);
    static_assert(TransparentIndices::of({1, 4}).contains(4));
    static_assert(!TransparentIndices::of({1, 4}).contains(2));
    SUCCEED();
}

}  // namespace
