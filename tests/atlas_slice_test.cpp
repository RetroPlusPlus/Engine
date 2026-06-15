// ENG-2.G — atlas asset ingestion: the device-free slicer + the read-order permutations + the
// manifest ergonomics. Pure geometry (sliceLayout depends only on geometry.h), so this whole suite
// is headless — no window, no GPU device. It pins the permutation correctness exactly (every one of
// the 8 read orders over a known grid), the content kinds, the remainder-drop + degenerate guards,
// the named presets, and the AtlasManifest's count()/operator[].

#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/image.h"
#include "retropp/renderer.h"  // AtlasManifest (the ergonomics test; no device is created)

using namespace retropp;

namespace {

// The carved tile-index sequence — the read order made directly comparable.
std::vector<int> tiles(std::span<const AssetSlot> slots) {
    std::vector<int> t;
    t.reserve(slots.size());
    for (const AssetSlot& s : slots) t.push_back(static_cast<int>(s.tile));
    return t;
}

constexpr AssetDimensions k8x8 = AssetDimensions::GameBoy8x8;

}  // namespace

// ── The two user examples, by name ───────────────────────────────────────────────────────────────

// A 16×8 image (two 8×8 cells side by side), Tileset, western order → slots { tile 0 }, { tile 1 }.
TEST(SliceLayout, UserExampleHorizontalPairLeftRightThenDown) {
    const auto slots = sliceLayout(PixelSize{16, 8}, k8x8, ContentKind::Tileset,
                                   ReadOrder::LeftRightThenDown);
    ASSERT_EQ(slots.size(), 2u);
    EXPECT_EQ(slots[0], (AssetSlot{0, k8x8}));
    EXPECT_EQ(slots[1], (AssetSlot{1, k8x8}));
}

// A 16×16 image (a 2×2 cell grid), column-major TopBottomThenRight → tile order 0, 2, 1, 3.
TEST(SliceLayout, UserExampleSquareColumnMajor) {
    const auto slots = sliceLayout(PixelSize{16, 16}, k8x8, ContentKind::Tileset,
                                   ReadOrder::TopBottomThenRight);
    EXPECT_EQ(tiles(slots), (std::vector<int>{0, 2, 1, 3}));
}

// ── All 8 read orders over a known 3×2 (cols×rows) cell grid (image 24×16, asset 8×8) ────────────
// cellsAcross = 24/8 = 3, so the natural tile index of cell (col,row) is row*3 + col:
//     (0,0)=0 (1,0)=1 (2,0)=2
//     (0,1)=3 (1,1)=4 (2,1)=5
// Each order's emitted tile sequence is asserted exactly — the permutation-correctness core.

namespace {
auto slice3x2(ReadOrder order) {
    return tiles(sliceLayout(PixelSize{24, 16}, k8x8, ContentKind::Tileset, order));
}
}  // namespace

TEST(SliceLayoutReadOrder, LeftRightThenDown) {
    EXPECT_EQ(slice3x2(ReadOrder::LeftRightThenDown), (std::vector<int>{0, 1, 2, 3, 4, 5}));
}
TEST(SliceLayoutReadOrder, RightLeftThenDown) {
    EXPECT_EQ(slice3x2(ReadOrder::RightLeftThenDown), (std::vector<int>{2, 1, 0, 5, 4, 3}));
}
TEST(SliceLayoutReadOrder, LeftRightThenUp) {
    EXPECT_EQ(slice3x2(ReadOrder::LeftRightThenUp), (std::vector<int>{3, 4, 5, 0, 1, 2}));
}
TEST(SliceLayoutReadOrder, RightLeftThenUp) {
    EXPECT_EQ(slice3x2(ReadOrder::RightLeftThenUp), (std::vector<int>{5, 4, 3, 2, 1, 0}));
}
TEST(SliceLayoutReadOrder, TopBottomThenRight) {
    EXPECT_EQ(slice3x2(ReadOrder::TopBottomThenRight), (std::vector<int>{0, 3, 1, 4, 2, 5}));
}
TEST(SliceLayoutReadOrder, BottomTopThenRight) {
    EXPECT_EQ(slice3x2(ReadOrder::BottomTopThenRight), (std::vector<int>{3, 0, 4, 1, 5, 2}));
}
TEST(SliceLayoutReadOrder, TopBottomThenLeft) {
    EXPECT_EQ(slice3x2(ReadOrder::TopBottomThenLeft), (std::vector<int>{2, 5, 1, 4, 0, 3}));
}
TEST(SliceLayoutReadOrder, BottomTopThenLeft) {
    EXPECT_EQ(slice3x2(ReadOrder::BottomTopThenLeft), (std::vector<int>{5, 2, 4, 1, 3, 0}));
}

// ── Content kinds ────────────────────────────────────────────────────────────────────────────────

// Single → exactly one slot covering the WHOLE image (dimensions = image size), tile 0, regardless
// of assetSize / order.
TEST(SliceLayout, SingleIsOneWholeImageSlot) {
    const auto slots = sliceLayout(PixelSize{24, 16}, k8x8, ContentKind::Single,
                                   ReadOrder::LeftRightThenDown);
    ASSERT_EQ(slots.size(), 1u);
    EXPECT_EQ(slots[0], (AssetSlot{0, AssetDimensions{24, 16}}));
}

// Single ignores assetSize entirely — an asset size that would be rejected for a grid still yields
// the one whole-image slot.
TEST(SliceLayout, SingleIgnoresAssetSize) {
    const auto slots = sliceLayout(PixelSize{20, 12}, AssetDimensions{6, 6}, ContentKind::Single,
                                   ReadOrder::LeftRightThenDown);
    ASSERT_EQ(slots.size(), 1u);
    EXPECT_EQ(slots[0], (AssetSlot{0, AssetDimensions{20, 12}}));
}

// Tileset and SpriteSeries carve identically — distinct names over the same grid math.
TEST(SliceLayout, TilesetAndSpriteSeriesAreIdentical) {
    const PixelSize img{24, 16};
    const auto a = sliceLayout(img, k8x8, ContentKind::Tileset, ReadOrder::TopBottomThenLeft);
    const auto b = sliceLayout(img, k8x8, ContentKind::SpriteSeries, ReadOrder::TopBottomThenLeft);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.size(), 6u);
}

// A larger asset slices into a coarser grid: a 16×16 image with 16×16 assets → one 16×16 slot whose
// tile is still on the 8px cell grid (a 16×16 sprite spans a 2×2 cell block from cell 0).
TEST(SliceLayout, LargerAssetSpansMultipleCells) {
    const auto slots = sliceLayout(PixelSize{32, 16}, AssetDimensions::Snes16x16,
                                   ContentKind::SpriteSeries, ReadOrder::LeftRightThenDown);
    // grid 2 cols × 1 row of 16×16; cellsAcross = 32/8 = 4 → cell-0 and cell-2 top-lefts.
    ASSERT_EQ(slots.size(), 2u);
    EXPECT_EQ(slots[0], (AssetSlot{0, AssetDimensions{16, 16}}));
    EXPECT_EQ(slots[1], (AssetSlot{2, AssetDimensions{16, 16}}));
}

// ── count: carve only the real assets, dropping trailing empty cells ─────────────────────────────

// A positive count emits only the first `count` slots in read order (a sheet with room for 6 cells but
// only 4 real frames → 4 slots).
TEST(SliceLayout, CountCapsToTheFirstNInReadOrder) {
    const auto slots = sliceLayout(PixelSize{24, 16}, k8x8, ContentKind::Tileset,
                                   ReadOrder::LeftRightThenDown, /*count=*/4);
    EXPECT_EQ(tiles(slots), (std::vector<int>{0, 1, 2, 3}));
}

// count composes with the read order — the first `count` of the chosen traversal.
TEST(SliceLayout, CountRespectsReadOrder) {
    const auto slots = sliceLayout(PixelSize{24, 16}, k8x8, ContentKind::Tileset,
                                   ReadOrder::RightLeftThenDown, /*count=*/3);
    EXPECT_EQ(tiles(slots), (std::vector<int>{2, 1, 0}));  // first 3 of {2,1,0,5,4,3}
}

// count == 0 is the default — the whole grid.
TEST(SliceLayout, CountZeroCarvesTheWholeGrid) {
    const auto all  = sliceLayout(PixelSize{24, 16}, k8x8, ContentKind::Tileset,
                                  ReadOrder::LeftRightThenDown);
    const auto zero = sliceLayout(PixelSize{24, 16}, k8x8, ContentKind::Tileset,
                                  ReadOrder::LeftRightThenDown, /*count=*/0);
    EXPECT_EQ(all.size(), 6u);
    EXPECT_EQ(all, zero);
}

// A count past the grid capacity is clamped to capacity (and logged) — never invents empty slots.
TEST(SliceLayout, CountBeyondCapacityClampsToCapacity) {
    const auto slots = sliceLayout(PixelSize{24, 16}, k8x8, ContentKind::Tileset,
                                   ReadOrder::LeftRightThenDown, /*count=*/10);
    EXPECT_EQ(slots.size(), 6u);  // the grid only holds 6
}

// Single ignores count — it is always exactly one whole-image slot.
TEST(SliceLayout, SingleIgnoresCount) {
    const auto slots = sliceLayout(PixelSize{24, 16}, k8x8, ContentKind::Single,
                                   ReadOrder::LeftRightThenDown, /*count=*/3);
    ASSERT_EQ(slots.size(), 1u);
    EXPECT_EQ(slots[0], (AssetSlot{0, AssetDimensions{24, 16}}));
}

// ── Remainder + degenerate guards ──────────────────────────────────────────────────────────────

// A trailing partial column that does not complete a cell is dropped — full cells only (logged).
TEST(SliceLayout, NonDivisibleRemainderDropped) {
    const auto slots = sliceLayout(PixelSize{20, 8}, k8x8, ContentKind::Tileset,
                                   ReadOrder::LeftRightThenDown);
    EXPECT_EQ(tiles(slots), (std::vector<int>{0, 1}));  // the trailing 4px column dropped
}

TEST(SliceLayout, AssetNotMultipleOfCellGridIsEmpty) {
    EXPECT_TRUE(sliceLayout(PixelSize{16, 16}, AssetDimensions{6, 8}, ContentKind::Tileset,
                            ReadOrder::LeftRightThenDown)
                    .empty());
}

TEST(SliceLayout, AssetLargerThanImageIsEmpty) {
    EXPECT_TRUE(sliceLayout(PixelSize{8, 8}, AssetDimensions::Snes16x16, ContentKind::Tileset,
                            ReadOrder::LeftRightThenDown)
                    .empty());
}

TEST(SliceLayout, NonPositiveImageIsEmpty) {
    EXPECT_TRUE(sliceLayout(PixelSize{0, 8}, k8x8, ContentKind::Tileset,
                            ReadOrder::LeftRightThenDown)
                    .empty());
    EXPECT_TRUE(sliceLayout(PixelSize{8, -8}, k8x8, ContentKind::Single,
                            ReadOrder::LeftRightThenDown)
                    .empty());
}

TEST(SliceLayout, NonPositiveAssetIsEmpty) {
    EXPECT_TRUE(sliceLayout(PixelSize{16, 16}, AssetDimensions{0, 8}, ContentKind::Tileset,
                            ReadOrder::LeftRightThenDown)
                    .empty());
}

// ── Named presets + grid constant ──────────────────────────────────────────────────────────────

TEST(ReadOrder, PresetsCarryTheirFillAndDirections) {
    using F = ReadOrder::Fill;
    using H = ReadOrder::HorizontalDir;
    using V = ReadOrder::VerticalDir;
    EXPECT_EQ(ReadOrder::LeftRightThenDown,  (ReadOrder{F::Rows,    H::LeftToRight, V::TopToBottom}));
    EXPECT_EQ(ReadOrder::RightLeftThenDown,  (ReadOrder{F::Rows,    H::RightToLeft, V::TopToBottom}));
    EXPECT_EQ(ReadOrder::LeftRightThenUp,    (ReadOrder{F::Rows,    H::LeftToRight, V::BottomToTop}));
    EXPECT_EQ(ReadOrder::RightLeftThenUp,    (ReadOrder{F::Rows,    H::RightToLeft, V::BottomToTop}));
    EXPECT_EQ(ReadOrder::TopBottomThenRight, (ReadOrder{F::Columns, H::LeftToRight, V::TopToBottom}));
    EXPECT_EQ(ReadOrder::BottomTopThenRight, (ReadOrder{F::Columns, H::LeftToRight, V::BottomToTop}));
    EXPECT_EQ(ReadOrder::TopBottomThenLeft,  (ReadOrder{F::Columns, H::RightToLeft, V::TopToBottom}));
    EXPECT_EQ(ReadOrder::BottomTopThenLeft,  (ReadOrder{F::Columns, H::RightToLeft, V::BottomToTop}));
}

TEST(ReadOrder, DefaultIsWesternLeftRightThenDown) {
    EXPECT_EQ(ReadOrder{}, ReadOrder::LeftRightThenDown);
}

TEST(SliceLayout, AtlasCellGridIsEightPixels) {
    EXPECT_EQ(kAtlasCellPx, 8);
}

// ── AtlasManifest ergonomics (no device — a hand-built slot vector) ──────────────────────────────

TEST(AtlasManifest, CountAndIndexOverHandBuiltSlots) {
    AtlasManifest m{static_cast<AtlasId>(7),
                    {AssetSlot{0, k8x8}, AssetSlot{1, k8x8}, AssetSlot{2, k8x8}}};
    EXPECT_EQ(m.count(), 3u);
    EXPECT_EQ(m[0], (AssetSlot{0, k8x8}));
    EXPECT_EQ(m[2], (AssetSlot{2, k8x8}));
    EXPECT_EQ(m.atlas, static_cast<AtlasId>(7));
}

// ── ENG-2.H: grouped-grid kinds + AnimationSeries manifest grouping ────────────────────────────────

// SingleAnimation carves identically to a Tileset — it IS grid slicing (the name conveys intent and
// drives manifest grouping; it does not change the carve).
TEST(SliceLayout, SingleAnimationCarvesIdenticallyToTileset) {
    const PixelSize img{24, 16};
    const auto tileset = sliceLayout(img, k8x8, ContentKind::Tileset, ReadOrder::LeftRightThenDown);
    const auto anim    = sliceLayout(img, k8x8, ContentKind::SingleAnimation, ReadOrder::LeftRightThenDown);
    EXPECT_EQ(anim, tileset);
}

// AnimationSeries also carves flat — the multi-animation structure is a manifest concern, not a carve.
TEST(SliceLayout, AnimationSeriesCarvesIdenticallyToTileset) {
    const PixelSize img{32, 16};
    const auto tileset = sliceLayout(img, k8x8, ContentKind::Tileset, ReadOrder::LeftRightThenDown);
    const auto series  = sliceLayout(img, k8x8, ContentKind::AnimationSeries, ReadOrder::LeftRightThenDown);
    EXPECT_EQ(series, tileset);
    EXPECT_EQ(series.size(), 8u);  // 4×2 grid
}

// A 4×2 grid (tiles 0..7) grouped by 2 → 4 animations of 2 frames each; by 4 → 2 animations of 4.
TEST(AtlasManifest, AnimationSeriesGroupingByTwo) {
    auto slots = sliceLayout(PixelSize{32, 16}, k8x8, ContentKind::AnimationSeries,
                             ReadOrder::LeftRightThenDown);
    AtlasManifest m{static_cast<AtlasId>(3), std::move(slots), /*framesPerAnimation=*/2};
    ASSERT_EQ(m.groupCount(), 4u);
    EXPECT_EQ(tiles(m.group(0)), (std::vector<int>{0, 1}));
    EXPECT_EQ(tiles(m.group(3)), (std::vector<int>{6, 7}));
}

TEST(AtlasManifest, AnimationSeriesGroupingByFour) {
    auto slots = sliceLayout(PixelSize{32, 16}, k8x8, ContentKind::AnimationSeries,
                             ReadOrder::LeftRightThenDown);
    AtlasManifest m{static_cast<AtlasId>(3), std::move(slots), /*framesPerAnimation=*/4};
    ASSERT_EQ(m.groupCount(), 2u);
    EXPECT_EQ(tiles(m.group(0)), (std::vector<int>{0, 1, 2, 3}));
    EXPECT_EQ(tiles(m.group(1)), (std::vector<int>{4, 5, 6, 7}));
}

// framesPerAnimation == 0 (the non-series default) → ungrouped: groupCount 0, group() throws.
TEST(AtlasManifest, UngroupedHasNoGroupsAndGroupThrows) {
    AtlasManifest m{static_cast<AtlasId>(1),
                    {AssetSlot{0, k8x8}, AssetSlot{1, k8x8}, AssetSlot{2, k8x8}}};  // framesPerAnimation = 0
    EXPECT_EQ(m.groupCount(), 0u);
    EXPECT_THROW((void)m.group(0), std::out_of_range);
}

// An out-of-range group index throws even when grouped.
TEST(AtlasManifest, GroupOutOfRangeThrows) {
    AtlasManifest m{static_cast<AtlasId>(1),
                    {AssetSlot{0, k8x8}, AssetSlot{1, k8x8}, AssetSlot{2, k8x8}, AssetSlot{3, k8x8}}, 2};
    ASSERT_EQ(m.groupCount(), 2u);
    EXPECT_THROW((void)m.group(2), std::out_of_range);
}

// The count cap composes with grouping: carve only the first 6 of an 8-cell grid, group by 2 → 3 groups.
TEST(AtlasManifest, CountCapComposesWithGrouping) {
    auto slots = sliceLayout(PixelSize{32, 16}, k8x8, ContentKind::AnimationSeries,
                             ReadOrder::LeftRightThenDown, /*count=*/6);
    ASSERT_EQ(slots.size(), 6u);
    AtlasManifest m{static_cast<AtlasId>(5), std::move(slots), 2};
    EXPECT_EQ(m.groupCount(), 3u);  // grouping applies to the capped result
    EXPECT_EQ(tiles(m.group(2)), (std::vector<int>{4, 5}));
}
