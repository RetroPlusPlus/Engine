#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"

// Device-free coverage of the per-row effect data table's CPU mirror: the flat row-data store's texel
// addressing (rowDataStoreTexel) and the per-effect stacking layout (stackRowTables), plus the
// ScreenSpaceEffect::paramTable default. The live GPU upload + in-shader Load is build-compiled and
// dev-verified across the three backends; these are the failable units.

namespace retropp {
namespace {

// ── rowDataStoreTexel — a table row's texel in the flat store ──────────────────────────

TEST(RowDataStoreTexel, AddressesColumnZeroAtStackedRow) {
    EXPECT_EQ(rowDataStoreTexel(0, 0), (PaletteTexel{0, 0}));
    EXPECT_EQ(rowDataStoreTexel(5, 0), (PaletteTexel{0, 5}));   // row offsets down within a table at storeY 0
    EXPECT_EQ(rowDataStoreTexel(0, 10), (PaletteTexel{0, 10})); // a table at storeY 10
    EXPECT_EQ(rowDataStoreTexel(3, 10), (PaletteTexel{0, 13})); // storeY + row
}

// ── stackRowTables — vertical stacking layout ─────────────────────────────────────────

TEST(StackRowTables, SingleTableStartsAtZero) {
    const std::array<std::uint32_t, 1> counts{7};
    const std::vector<RowTableLoc> locs = stackRowTables(counts);
    ASSERT_EQ(locs.size(), 1u);
    EXPECT_EQ(locs[0], (RowTableLoc{0, 7}));
}

TEST(StackRowTables, TablesStackWithoutOverlap) {
    const std::array<std::uint32_t, 3> counts{4, 6, 2};
    const std::vector<RowTableLoc> locs = stackRowTables(counts);
    ASSERT_EQ(locs.size(), 3u);
    EXPECT_EQ(locs[0], (RowTableLoc{0, 4}));
    EXPECT_EQ(locs[1], (RowTableLoc{4, 6}));   // starts where table 0 ended
    EXPECT_EQ(locs[2], (RowTableLoc{10, 2}));  // 4 + 6
}

TEST(StackRowTables, TotalHeightIsSumOfRows) {
    const std::array<std::uint32_t, 3> counts{4, 6, 2};
    const std::vector<RowTableLoc> locs = stackRowTables(counts);
    const RowTableLoc& last = locs.back();
    EXPECT_EQ(last.storeY + last.rows, 12u);  // the store's total height
}

TEST(StackRowTables, EmptyTableContributesNoHeight) {
    const std::array<std::uint32_t, 4> counts{4, 0, 3, 0};
    const std::vector<RowTableLoc> locs = stackRowTables(counts);
    ASSERT_EQ(locs.size(), 4u);
    EXPECT_EQ(locs[0], (RowTableLoc{0, 4}));
    EXPECT_EQ(locs[1], (RowTableLoc{4, 0}));  // a no-table effect: zero rows at the current y
    EXPECT_EQ(locs[2], (RowTableLoc{4, 3}));  // table 1 added nothing, so table 2 starts at 4
    EXPECT_EQ(locs[3], (RowTableLoc{7, 0}));
}

TEST(StackRowTables, NoTablesYieldEmptyLayout) {
    const std::vector<std::uint32_t> none;
    EXPECT_TRUE(stackRowTables(none).empty());  // a frame using no tables → an empty store layout
}

TEST(StackRowTables, Deterministic) {
    const std::array<std::uint32_t, 3> counts{4, 6, 2};
    EXPECT_EQ(stackRowTables(counts), stackRowTables(counts));  // same effect set → same stacking
}

// ── ScreenSpaceEffect::paramTable default ─────────────────────────────────────────────

TEST(ScreenSpaceEffectParamTable, DefaultsEmpty) {
    const ScreenSpaceEffect e;
    EXPECT_TRUE(e.paramTable.empty());  // no table ⇒ identity (unchanged output)
}

TEST(ScreenSpaceEffectParamTable, CarriesAnInlineSpan) {
    const std::array<Vec4, 3> rows{Vec4{1, 0, 0, 0}, Vec4{2, 0, 0, 0}, Vec4{3, 0, 0, 0}};
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom, .paramTable = rows};
    ASSERT_EQ(e.paramTable.size(), 3u);
    EXPECT_EQ(e.paramTable[1].x, 2.0f);
}

}  // namespace
}  // namespace retropp
