// Below-scope emission fields — the store a scene-facing Bloom / Glow samples.
//
// A Below-scope Bloom / Glow is a lens onto the SCENE's light: the renderer extracts the scene's emission
// once per distinct (kind, threshold, reach), blurs it, and leaves the halo in one layer of a texture array
// that every lens sharing those parameters samples. These tests pin the CPU half of that — which steps need
// a field, which steps share one, where a record's array index lands, and what happens to a step that cannot
// have one. All device-free: the arithmetic decides the passes, so the arithmetic is what is asserted.
//
// The device-backed half (a halo actually visible through a silhouette, two fields in one draw) lives in
// golden_readback_test.cpp.

#include <gtest/gtest.h>

#include <retropp/draw_state.h>
#include <retropp/postprocess.h>

#include <span>
#include <vector>

using namespace retropp;

namespace {

// A Below-scope lens carrying one chain effect.
Sprite lensWith(ScreenSpaceEffect e) {
    e.scope = ScreenSpaceEffectScope::Below;
    Sprite s{.key = "lens"};
    s.effects = {e};
    return s;
}

ScreenSpaceEffect belowGlow(float radius, std::uint8_t threshold = 0, std::uint8_t intensity = 255) {
    return ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Glow,
                             .scope     = ScreenSpaceEffectScope::Below,
                             .radius    = radius,
                             .threshold = threshold,
                             .intensity = intensity};
}

ScreenSpaceEffect belowBloom(float radius, std::uint8_t threshold = 0, std::uint8_t intensity = 255) {
    return ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Bloom,
                             .scope     = ScreenSpaceEffectScope::Below,
                             .radius    = radius,
                             .threshold = threshold,
                             .intensity = intensity};
}

// The array layer a record was pointed at.
int slotOf(const SpriteFxRecord& r) { return static_cast<int>(r.params[3]); }

}  // namespace

// ── Sharing: what makes two lenses one field ────────────────────────────────────────────────

TEST(EmissionFields, LensesSharingKindThresholdAndReachShareOneField) {
    // The whole point of the store: N lenses authored alike cost ONE extract + blur, not N.
    std::vector<EmissionFieldKey> fields;
    Sprite a = lensWith(belowGlow(6.0f));
    Sprite b = lensWith(belowGlow(6.0f));
    std::vector<SpriteFxRecord> ra = buildSpriteBelowRecords(a);
    std::vector<SpriteFxRecord> rb = buildSpriteBelowRecords(b);
    ASSERT_EQ(assignEmissionFields(ra, fields), 0);
    ASSERT_EQ(assignEmissionFields(rb, fields), 0);

    EXPECT_EQ(fields.size(), 1u);
    EXPECT_EQ(slotOf(ra[0]), 0);
    EXPECT_EQ(slotOf(rb[0]), 0);
}

TEST(EmissionFields, DifferingReachThresholdOrKindEachTakeTheirOwnField) {
    // Each of the three key components alone splits the field — the halo's CONTENT differs in each case, so
    // one buffer cannot serve both.
    std::vector<EmissionFieldKey> fields;
    Sprite base   = lensWith(belowGlow(6.0f, 0));
    Sprite wider  = lensWith(belowGlow(9.0f, 0));    // reach differs
    Sprite keyed  = lensWith(belowGlow(6.0f, 128));  // threshold differs
    Sprite bloomy = lensWith(belowBloom(6.0f, 0));   // extract differs

    for (Sprite* s : {&base, &wider, &keyed, &bloomy}) {
        std::vector<SpriteFxRecord> r = buildSpriteBelowRecords(*s);
        ASSERT_EQ(assignEmissionFields(r, fields), 0);
    }
    EXPECT_EQ(fields.size(), 4u);
}

TEST(EmissionFields, OneLensWithTwoDistinctStepsIndexesTwoLayers) {
    // The case the array store exists to keep expressible: a single lens carrying a Bloom and a Glow at
    // differing parameters samples two fields in ONE draw, needing no extra pass and no run split.
    std::vector<EmissionFieldKey> fields;
    Sprite s{.key = "lens"};
    s.effects = {belowBloom(5.0f, 40), belowGlow(12.0f, 0)};
    std::vector<SpriteFxRecord> recs = buildSpriteBelowRecords(s);
    ASSERT_EQ(recs.size(), 2u);
    ASSERT_EQ(assignEmissionFields(recs, fields), 0);

    EXPECT_EQ(fields.size(), 2u);
    EXPECT_NE(slotOf(recs[0]), slotOf(recs[1]));
    EXPECT_EQ(fields[0].extract, EmissionExtract::Bloom);
    EXPECT_EQ(fields[1].extract, EmissionExtract::Glow);
}

// ── The reach is the authored radius, verbatim ──────────────────────────────────────────────

TEST(EmissionFields, ReachIsTheAuthoredRadiusWithNoPlacementScaling) {
    // A below effect's lengths are already viewport pixels — it distorts the scene, which is a
    // viewport-resolution image. So unlike the Layer-scope path (which maps art px → viewport px through the
    // sprite's linear scale), the below reach passes through untouched however the lens is placed.
    std::vector<EmissionFieldKey> fields;
    Sprite s   = lensWith(belowGlow(7.0f));
    s.transform = Transform::scale(4.0f, 4.0f);   // a placement that would scale an art-space reach
    std::vector<SpriteFxRecord> recs = buildSpriteBelowRecords(s);
    ASSERT_EQ(assignEmissionFields(recs, fields), 0);

    ASSERT_EQ(fields.size(), 1u);
    EXPECT_FLOAT_EQ(fields[0].reach, 7.0f);
}

// ── Steps that get no field ─────────────────────────────────────────────────────────────────

TEST(EmissionFields, AGlowWithNoReachNeedsNoFieldAndIsZeroedToContributeNothing) {
    // A Glow with no reach has no aura at all, so it needs no buffer. Zeroing its intensity is what makes
    // the fragment skip it: it then samples nothing, whichever array happens to be bound.
    std::vector<EmissionFieldKey> fields;
    std::vector<SpriteFxRecord> recs = buildSpriteBelowRecords(lensWith(belowGlow(0.0f)));
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_EQ(assignEmissionFields(recs, fields), 0);

    EXPECT_TRUE(fields.empty());
    EXPECT_FLOAT_EQ(recs[0].params[2], 0.0f);
    EXPECT_EQ(slotOf(recs[0]), 0);
}

TEST(EmissionFields, ABloomWithNoReachStillTakesAFieldBecauseItStillAdds) {
    // The kinds gate differently and each matches what it renders. A Bloom at zero reach still adds its own
    // un-blurred brightpass, so it earns a field; only no intensity gates it away.
    std::vector<EmissionFieldKey> fields;
    std::vector<SpriteFxRecord> recs = buildSpriteBelowRecords(lensWith(belowBloom(0.0f)));
    ASSERT_EQ(assignEmissionFields(recs, fields), 0);
    EXPECT_EQ(fields.size(), 1u);
    EXPECT_GT(recs[0].params[2], 0.0f);

    std::vector<EmissionFieldKey> none;
    std::vector<SpriteFxRecord> dim = buildSpriteBelowRecords(lensWith(belowBloom(6.0f, 0, 0)));
    ASSERT_EQ(assignEmissionFields(dim, none), 0);
    EXPECT_TRUE(none.empty());
}

TEST(EmissionFields, PastTheStoresCapacityAStepIsReportedAndContributesNothing) {
    // Capacity is an allocation, so overflow is degenerate authoring rather than a case content meets. It is
    // reported to the caller (which logs it) and the step is zeroed — never silently drawn wrong.
    std::vector<EmissionFieldKey> fields;
    for (std::size_t i = 0; i < kMaxEmissionFields; ++i) {
        std::vector<SpriteFxRecord> r =
            buildSpriteBelowRecords(lensWith(belowGlow(2.0f + static_cast<float>(i))));
        ASSERT_EQ(assignEmissionFields(r, fields), 0);
    }
    ASSERT_EQ(fields.size(), kMaxEmissionFields);

    std::vector<SpriteFxRecord> overflow =
        buildSpriteBelowRecords(lensWith(belowGlow(2.0f + static_cast<float>(kMaxEmissionFields))));
    EXPECT_EQ(assignEmissionFields(overflow, fields), 1);
    EXPECT_EQ(fields.size(), kMaxEmissionFields);   // the store did not grow
    EXPECT_FLOAT_EQ(overflow[0].params[2], 0.0f);   // and the step adds nothing
    EXPECT_EQ(slotOf(overflow[0]), 0);              // pointing at a layer that always exists
}

// ── Region-confined below Bloom ─────────────────────────────────────────────────────────────

TEST(EmissionFields, ARegionConfinedBelowBloomTakesAFieldLikeAChainStep) {
    // A below region is SCENE-sourced — it grades the shared halo under the region's blend — so it samples
    // the same store a chain step does. (A below region Glow is dropped at pack time: its tint needs the
    // record lanes the region's shape occupies.)
    std::vector<EmissionFieldKey> fields;
    Sprite s{.key = "lens"};
    s.regions = {Region{.key    = "rg",
                        .shape  = ShapePoints{.points = {Point{8.0f, 8.0f}}, .radius = 6.0f},
                        .effects = {belowBloom(5.0f)}}};
    std::vector<SpriteFxRecord> recs = buildSpriteBelowRecords(s);
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_NE(recs[0].flags & kSpriteFxIsRegion, 0u);
    ASSERT_EQ(assignEmissionFields(recs, fields), 0);

    EXPECT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields[0].extract, EmissionExtract::Bloom);
}

// ── Non-emitting kinds are untouched ────────────────────────────────────────────────────────

TEST(EmissionFields, AKindThatEmitsNothingNeitherTakesAFieldNorLosesItsParams) {
    // The walk touches emission-consuming records only — a ColorFill's lanes carry its fill and must survive
    // intact.
    std::vector<EmissionFieldKey> fields;
    Sprite s = lensWith(ScreenSpaceEffect{.kind  = ScreenSpaceEffectKind::ColorFill,
                                          .scope = ScreenSpaceEffectScope::Below,
                                          .fill  = Rgba8{.r = 255, .g = 128, .b = 0, .a = 255}});
    std::vector<SpriteFxRecord> recs = buildSpriteBelowRecords(s);
    const SpriteFxRecord before = recs[0];
    ASSERT_EQ(assignEmissionFields(recs, fields), 0);

    EXPECT_TRUE(fields.empty());
    EXPECT_FLOAT_EQ(recs[0].params[0], before.params[0]);
    EXPECT_FLOAT_EQ(recs[0].params[1], before.params[1]);
    EXPECT_FLOAT_EQ(recs[0].params[2], before.params[2]);
    EXPECT_FLOAT_EQ(recs[0].params[3], before.params[3]);
}
