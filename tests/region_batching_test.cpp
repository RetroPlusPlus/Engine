#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/transform.h"

// Region batching (postprocess.h): the pure CPU side of the instanced-additive fast path that collapses
// N same-shader additive region-confined effects into ONE render pass. Three device-free units, tested
// here headlessly (the renderer feeds them and consumes their output):
//   regionBatchEligible   — the routing predicate (each disqualifier flips independently)
//   regionBatchInstance   — the per-region instance record (box == regionScissorRect; spine + radius)
//   groupRegionBatches    — the run grouping (contiguous eligible stretches, group by (stage, params),
//                           ≥2 → a run; singletons + ineligible steps stay solo; interleaved compositions)

namespace retropp {
namespace {

constexpr int kW = 160;  // viewport 160×144 → compose grid at scale 1
constexpr int kH = 144;

// A Custom effect on stage `h` with no paramTable — the eligible archetype.
ScreenSpaceEffect customEffect(std::uint32_t h = 0) {
    ScreenSpaceEffect e;
    e.kind         = ScreenSpaceEffectKind::Custom;
    e.customShader = static_cast<PostProcessStageId>(h);
    return e;
}

// ── Eligibility predicate: the eligible archetypes ───────────────────────────────────────────

TEST(RegionBatching, EligibleCircleNormalFullAlpha) {
    EXPECT_TRUE(regionBatchEligible(customEffect(), ShapePoints::circle({80, 72}, 20),
                                    /*alpha=*/1.0f, BlendMode::Normal));
}

TEST(RegionBatching, EligibleCapsule) {
    EXPECT_TRUE(regionBatchEligible(customEffect(), ShapePoints::capsule({20, 20}, {60, 20}, 5),
                                    1.0f, BlendMode::Normal));
}

// ── Eligibility predicate: each disqualifier flips independently ──────────────────────────────

TEST(RegionBatching, BuiltInKindIneligible) {
    ScreenSpaceEffect e;
    e.kind = ScreenSpaceEffectKind::RowDisplacement;  // a built-in is never additive-declared
    EXPECT_FALSE(regionBatchEligible(e, ShapePoints::circle({80, 72}, 20), 1.0f, BlendMode::Normal));
}

TEST(RegionBatching, NonNormalBlendIneligible) {
    // A non-Normal blend reads the destination — one hardware-additive pass can't express it for all N.
    EXPECT_FALSE(regionBatchEligible(customEffect(), ShapePoints::circle({80, 72}, 20),
                                     1.0f, BlendMode::Add));
    EXPECT_FALSE(regionBatchEligible(customEffect(), ShapePoints::circle({80, 72}, 20),
                                     1.0f, BlendMode::Multiply));
}

TEST(RegionBatching, PartialAlphaIneligible) {
    EXPECT_FALSE(regionBatchEligible(customEffect(), ShapePoints::circle({80, 72}, 20),
                                     0.5f, BlendMode::Normal));
}

TEST(RegionBatching, ParamTableIneligible) {
    static constexpr std::array<Vec4, 2> kRows{Vec4{}, Vec4{}};
    ScreenSpaceEffect e = customEffect();
    e.paramTable        = std::span<const Vec4>(kRows);
    EXPECT_FALSE(regionBatchEligible(e, ShapePoints::circle({80, 72}, 20), 1.0f, BlendMode::Normal));
}

TEST(RegionBatching, InvertIneligible) {
    ShapePoints s = ShapePoints::circle({80, 72}, 20);
    s.invert      = true;
    EXPECT_FALSE(regionBatchEligible(customEffect(), s, 1.0f, BlendMode::Normal));
}

TEST(RegionBatching, StrokeIneligible) {
    ShapePoints s   = ShapePoints::circle({80, 72}, 20);
    s.strokeWidth   = 4.0f;
    EXPECT_FALSE(regionBatchEligible(customEffect(), s, 1.0f, BlendMode::Normal));
}

TEST(RegionBatching, TransformIneligible) {
    ShapePoints s = ShapePoints::circle({80, 72}, 20);
    s.transform   = Transform::translation(5.0f, 0.0f);  // non-identity warps the gate
    EXPECT_FALSE(regionBatchEligible(customEffect(), s, 1.0f, BlendMode::Normal));
}

TEST(RegionBatching, PolygonIneligible) {
    // A ≥3-vertex polygon is not the circle/capsule gate the batched fragment replicates.
    EXPECT_FALSE(regionBatchEligible(customEffect(), ShapePoints::triangle({10, 10}, {50, 10}, {10, 40}),
                                     1.0f, BlendMode::Normal));
}

TEST(RegionBatching, WholeReachIneligible) {
    // No shape (whole-reach effect) → 0 points → ineligible (nothing to bound / gate).
    EXPECT_FALSE(regionBatchEligible(customEffect(), ShapePoints{}, 1.0f, BlendMode::Normal));
}

TEST(RegionBatching, ShapelessRectangleIneligible) {
    // A whole-viewport shapeless region synthesizes a 4-point rectangle — excluded (n != 1, 2).
    EXPECT_FALSE(regionBatchEligible(customEffect(),
                                     ShapePoints::rectangle({-4, -4}, kW + 8.0f, kH + 8.0f),
                                     1.0f, BlendMode::Normal));
}

// ── Instance record: box == regionScissorRect, spine + radius ─────────────────────────────────

TEST(RegionBatching, CircleInstanceRecord) {
    const ShapePoints         shape = ShapePoints::circle({80, 72}, 30);
    const RegionBatchInstance r     = regionBatchInstance(shape, 1, kW, kH);
    EXPECT_EQ(r.box, regionScissorRect(shape, 1, kW, kH));  // the single box authority
    EXPECT_EQ(r.p0, (Point{80, 72}));
    EXPECT_EQ(r.p1, (Point{80, 72}));   // a circle repeats its centre as the spine
    EXPECT_FLOAT_EQ(r.radius, 30.0f);
}

TEST(RegionBatching, CapsuleInstanceRecord) {
    const ShapePoints         shape = ShapePoints::capsule({20, 20}, {60, 20}, 5);
    const RegionBatchInstance r     = regionBatchInstance(shape, 1, kW, kH);
    EXPECT_EQ(r.box, regionScissorRect(shape, 1, kW, kH));
    EXPECT_EQ(r.p0, (Point{20, 20}));
    EXPECT_EQ(r.p1, (Point{60, 20}));   // the capsule's two spine vertices
    EXPECT_FLOAT_EQ(r.radius, 5.0f);
}

TEST(RegionBatching, InstanceRecordScalesWithComposeGrid) {
    const ShapePoints shape = ShapePoints::circle({80, 72}, 30);
    const RegionBatchInstance r = regionBatchInstance(shape, 2, kW * 2, kH * 2);
    EXPECT_EQ(r.box, regionScissorRect(shape, 2, kW * 2, kH * 2));  // box in compose-grid px
    EXPECT_EQ(r.p0, (Point{80, 72}));   // spine stays viewport px (composeScale-independent)
    EXPECT_FLOAT_EQ(r.radius, 30.0f);
}

// ── Run grouping ──────────────────────────────────────────────────────────────────────────────

// Fixed uniform-byte payloads so (stage, params) keys can differ deterministically.
constexpr std::array<std::byte, 4> kParamsA{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
constexpr std::array<std::byte, 4> kParamsB{std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}};

BatchStep eligibleStep(std::uint32_t stage, std::span<const std::byte> params) {
    return BatchStep{true, stage, params};
}
BatchStep ineligibleStep() { return BatchStep{false, 0, {}}; }

TEST(RegionBatching, TwoSameKeyBatchIntoOneRun) {
    const std::array steps{eligibleStep(0, kParamsA), eligibleStep(0, kParamsA)};
    const RegionBatchGrouping g = groupRegionBatches(steps);
    ASSERT_EQ(g.runs.size(), 1u);
    EXPECT_EQ(g.runs[0].stage, 0u);
    EXPECT_EQ(g.runs[0].steps, (std::vector<std::uint32_t>{0u, 1u}));
    EXPECT_EQ(g.stepRun, (std::vector<int>{0, 0}));
}

TEST(RegionBatching, SingletonDoesNotBatch) {
    const std::array steps{eligibleStep(0, kParamsA)};
    const RegionBatchGrouping g = groupRegionBatches(steps);
    EXPECT_TRUE(g.runs.empty());
    EXPECT_EQ(g.stepRun, (std::vector<int>{-1}));  // one eligible step → solo per-region path
}

TEST(RegionBatching, DifferentParamsDoNotBatch) {
    const std::array steps{eligibleStep(0, kParamsA), eligibleStep(0, kParamsB)};
    const RegionBatchGrouping g = groupRegionBatches(steps);
    EXPECT_TRUE(g.runs.empty());
    EXPECT_EQ(g.stepRun, (std::vector<int>{-1, -1}));  // same stage, different uniform bytes
}

TEST(RegionBatching, DifferentStageDoesNotBatch) {
    const std::array steps{eligibleStep(0, kParamsA), eligibleStep(1, kParamsA)};
    const RegionBatchGrouping g = groupRegionBatches(steps);
    EXPECT_TRUE(g.runs.empty());
    EXPECT_EQ(g.stepRun, (std::vector<int>{-1, -1}));
}

TEST(RegionBatching, IneligibleBreaksContiguity) {
    // Two same-key eligibles split by an ineligible step are in DIFFERENT stretches → neither batches.
    const std::array steps{eligibleStep(0, kParamsA), ineligibleStep(), eligibleStep(0, kParamsA)};
    const RegionBatchGrouping g = groupRegionBatches(steps);
    EXPECT_TRUE(g.runs.empty());
    EXPECT_EQ(g.stepRun, (std::vector<int>{-1, -1, -1}));
}

TEST(RegionBatching, ThreeSameKeyOneRun) {
    const std::array steps{eligibleStep(0, kParamsA), eligibleStep(0, kParamsA),
                           eligibleStep(0, kParamsA)};
    const RegionBatchGrouping g = groupRegionBatches(steps);
    ASSERT_EQ(g.runs.size(), 1u);
    EXPECT_EQ(g.runs[0].steps, (std::vector<std::uint32_t>{0u, 1u, 2u}));
    EXPECT_EQ(g.stepRun, (std::vector<int>{0, 0, 0}));
}

TEST(RegionBatching, InterleavedTwoStagesTwoRuns) {
    // A₁ B₁ A₂ B₂ within one eligible stretch: grouping by key keeps BOTH compositions on the fast path.
    const std::array steps{eligibleStep(0, kParamsA), eligibleStep(1, kParamsB),
                           eligibleStep(0, kParamsA), eligibleStep(1, kParamsB)};
    const RegionBatchGrouping g = groupRegionBatches(steps);
    ASSERT_EQ(g.runs.size(), 2u);
    EXPECT_EQ(g.runs[0].stage, 0u);
    EXPECT_EQ(g.runs[0].steps, (std::vector<std::uint32_t>{0u, 2u}));  // stage 0 at 0 and 2
    EXPECT_EQ(g.runs[1].stage, 1u);
    EXPECT_EQ(g.runs[1].steps, (std::vector<std::uint32_t>{1u, 3u}));  // stage 1 at 1 and 3
    EXPECT_EQ(g.stepRun, (std::vector<int>{0, 1, 0, 1}));
}

TEST(RegionBatching, StretchesAcrossBoundaryFormSeparateRuns) {
    // AA | ineligible | AA → two runs, one per contiguous stretch (no merge across the boundary).
    const std::array steps{eligibleStep(0, kParamsA), eligibleStep(0, kParamsA), ineligibleStep(),
                           eligibleStep(0, kParamsA), eligibleStep(0, kParamsA)};
    const RegionBatchGrouping g = groupRegionBatches(steps);
    ASSERT_EQ(g.runs.size(), 2u);
    EXPECT_EQ(g.runs[0].steps, (std::vector<std::uint32_t>{0u, 1u}));
    EXPECT_EQ(g.runs[1].steps, (std::vector<std::uint32_t>{3u, 4u}));
    EXPECT_EQ(g.stepRun, (std::vector<int>{0, 0, -1, 1, 1}));
}

TEST(RegionBatching, MixedSingletonAndRunInOneStretch) {
    // Within one stretch: two of stage 0 (a run) + one lone stage 1 (solo). Order preserved.
    const std::array steps{eligibleStep(0, kParamsA), eligibleStep(1, kParamsB),
                           eligibleStep(0, kParamsA)};
    const RegionBatchGrouping g = groupRegionBatches(steps);
    ASSERT_EQ(g.runs.size(), 1u);
    EXPECT_EQ(g.runs[0].stage, 0u);
    EXPECT_EQ(g.runs[0].steps, (std::vector<std::uint32_t>{0u, 2u}));
    EXPECT_EQ(g.stepRun, (std::vector<int>{0, -1, 0}));  // the lone stage-1 step stays solo
}

TEST(RegionBatching, EmptyInputNoRuns) {
    const RegionBatchGrouping g = groupRegionBatches(std::span<const BatchStep>{});
    EXPECT_TRUE(g.runs.empty());
    EXPECT_TRUE(g.stepRun.empty());
}

TEST(RegionBatching, AllIneligibleNoRuns) {
    const std::array steps{ineligibleStep(), ineligibleStep()};
    const RegionBatchGrouping g = groupRegionBatches(steps);
    EXPECT_TRUE(g.runs.empty());
    EXPECT_EQ(g.stepRun, (std::vector<int>{-1, -1}));
}

TEST(RegionBatching, EmptyParamsSpansGroup) {
    // Two parameterless (empty-span) eligibles on the same stage batch — empty spans compare equal.
    const std::array steps{eligibleStep(3, {}), eligibleStep(3, {})};
    const RegionBatchGrouping g = groupRegionBatches(steps);
    ASSERT_EQ(g.runs.size(), 1u);
    EXPECT_EQ(g.runs[0].stage, 3u);
    EXPECT_EQ(g.stepRun, (std::vector<int>{0, 0}));
}

}  // namespace
}  // namespace retropp
