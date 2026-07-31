// Below-scope emission fields sized to their lens's footprint — the CPU half.
//
// A Below-scope Bloom / Glow is a lens onto the SCENE's light, and it reads its halo inside its own
// silhouette, so the field it needs is that lens's drawn footprint rather than the whole viewport. These
// tests pin what the renderer builds from: which steps ask for a field, what each asks for, and where the
// answer is written back.
//
// Steps do NOT share a field. One sized to a lens's footprint covers only where that lens sits, so two lenses
// authored identically still ask for two fields; the sharing that pays off is the page a reach groups into,
// which these tests do not decide.
//
// All device-free: the arithmetic decides the fields, so the arithmetic is what is asserted.

#include <gtest/gtest.h>

#include <retropp/draw_state.h>
#include <retropp/emission_atlas.h>
#include <retropp/postprocess.h>

#include <cstddef>
#include <span>
#include <vector>

using namespace retropp;

namespace {

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

// A Below-scope lens carrying `effects` as its chain.
Sprite lens(std::string key, std::vector<ScreenSpaceEffect> effects) {
    Sprite s{.key = std::move(key)};
    s.size    = AssetDimensions{8, 8};
    s.effects = std::move(effects);
    return s;
}

// The lens's drawn footprint, as the renderer would hand it over.
constexpr PixelBox kQuad{.x = 40, .y = 24, .w = 20, .h = 16};

// One lens's records and what its below steps ask the atlas for.
struct Asked {
    std::vector<SpriteFxRecord>             records;
    std::vector<SpriteBelowEmissionDemand>  demands;
};

Asked ask(const Sprite& s, std::size_t storeOffset = 0, PixelBox quad = kQuad) {
    Asked out;
    out.records = buildSpriteBelowRecords(s);
    out.demands = collectSpriteBelowEmissionDemands(out.records, storeOffset, quad);
    return out;
}

}  // namespace

// ── Which steps ask, and how many ───────────────────────────────────────────────────────────

TEST(SpriteBelowEmission, TwoLensesAuthoredAlikeAskForTwoFieldsNotOne) {
    // Both lenses author the same extract, threshold and reach — every parameter a field's content depends on
    // — and still get one field each, because each field covers only the footprint of the lens that asked and
    // neither footprint contains the other lens.
    const Sprite a = lens("a", {belowGlow(6.0f)});
    const Sprite b = lens("b", {belowGlow(6.0f)});

    const Asked ra = ask(a, 0, PixelBox{.x = 10, .y = 10, .w = 16, .h = 16});
    const Asked rb = ask(b, 0, PixelBox{.x = 90, .y = 60, .w = 16, .h = 16});

    ASSERT_EQ(ra.demands.size(), 1u);
    ASSERT_EQ(rb.demands.size(), 1u);
    EXPECT_EQ(ra.demands[0].extract, rb.demands[0].extract);
    EXPECT_FLOAT_EQ(ra.demands[0].blurReach, rb.demands[0].blurReach);
    EXPECT_NE(ra.demands[0].field.x, rb.demands[0].field.x);  // each covers its own lens, so neither serves both
}

TEST(SpriteBelowEmission, TwoStepsOnOneLensEachAskForTheirOwn) {
    const Sprite s = lens("s", {belowGlow(6.0f), belowBloom(6.0f)});

    const Asked r = ask(s);

    ASSERT_EQ(r.demands.size(), 2u);
    EXPECT_NE(r.demands[0].storeIndex, r.demands[1].storeIndex);
}

TEST(SpriteBelowEmission, ALensAuthoringNoHaloAsksForNothing) {
    Sprite s{.key = "plain"};
    s.size    = AssetDimensions{8, 8};
    s.effects = {ScreenSpaceEffect{.kind  = ScreenSpaceEffectKind::ColorFill,
                                   .scope = ScreenSpaceEffectScope::Below,
                                   .fill  = Rgba8{20, 40, 80, 255}}};

    const Asked r = ask(s);

    EXPECT_TRUE(r.demands.empty());
}

TEST(SpriteBelowEmission, AStepWithNoStrengthAsksForNothingAndLosesItsIndex) {
    const Sprite s = lens("s", {belowGlow(6.0f, 0, 0)});   // intensity 0 — the chain never engages

    Asked r = ask(s);

    EXPECT_TRUE(r.demands.empty());
    ASSERT_FALSE(r.records.empty());
    EXPECT_FLOAT_EQ(r.records[0].params[2], 0.0f);
    EXPECT_FLOAT_EQ(r.records[0].params[3], 0.0f);
}

TEST(SpriteBelowEmission, AGlowWithNoReachAsksForNothingAndIsZeroed) {
    // The two kinds gate differently and each matches what it renders. A Glow with no reach has no aura at
    // all, so there is nothing to extract; zeroing its strength is what makes the fragment skip it, so it
    // samples nothing whatever happens to be bound.
    const Sprite s = lens("s", {belowGlow(0.0f)});

    const Asked r = ask(s);

    EXPECT_TRUE(r.demands.empty());
    ASSERT_FALSE(r.records.empty());
    EXPECT_FLOAT_EQ(r.records[0].params[2], 0.0f);
    EXPECT_FLOAT_EQ(r.records[0].params[3], 0.0f);
}

TEST(SpriteBelowEmission, ABloomWithNoReachStillAsksBecauseItStillAdds) {
    // The other half of that asymmetry: a Bloom at zero reach still adds its own un-blurred brightpass, so it
    // earns a field. Only an absent intensity gates it away.
    const Asked lit = ask(lens("lit", {belowBloom(0.0f)}));

    ASSERT_EQ(lit.demands.size(), 1u);
    EXPECT_GT(lit.records[0].params[2], 0.0f);

    const Asked dim = ask(lens("dim", {belowBloom(6.0f, 0, 0)}));

    EXPECT_TRUE(dim.demands.empty());
}

TEST(SpriteBelowEmission, AKindThatEmitsNothingKeepsItsOwnParams) {
    // The walk touches emission-consuming records only. A ColorFill's lanes carry its fill, and a collector
    // that zeroed them on the way past would repaint the lens black.
    Sprite s{.key = "s"};
    s.size    = AssetDimensions{8, 8};
    s.effects = {ScreenSpaceEffect{.kind  = ScreenSpaceEffectKind::ColorFill,
                                   .scope = ScreenSpaceEffectScope::Below,
                                   .fill  = Rgba8{.r = 255, .g = 128, .b = 0, .a = 255}}};
    const std::vector<SpriteFxRecord> before = buildSpriteBelowRecords(s);
    ASSERT_FALSE(before.empty());

    const Asked r = ask(s);

    EXPECT_TRUE(r.demands.empty());
    ASSERT_FALSE(r.records.empty());
    for (std::size_t p = 0; p < 4; ++p) EXPECT_FLOAT_EQ(r.records[0].params[p], before[0].params[p]);
}

TEST(SpriteBelowEmission, ALensWithNoFootprintAsksForNothing) {
    // Nothing drawn means nothing to read a halo inside — a lens entirely behind the projection, say.
    const Sprite s = lens("s", {belowBloom(5.0f)});

    Asked r = ask(s, 0, PixelBox{});

    EXPECT_TRUE(r.demands.empty());
    ASSERT_FALSE(r.records.empty());
    EXPECT_FLOAT_EQ(r.records[0].params[2], 0.0f);
}

TEST(SpriteBelowEmission, ARegionConfinedBelowStepAsksToo) {
    // A region grades the scene over shape ∩ silhouette and reads the same field, so it is a realized step
    // like a whole-silhouette one. Collecting only the chain would leave it pointed at a field nobody planned.
    Sprite s{.key = "s"};
    s.size    = AssetDimensions{8, 8};
    s.regions = {Region{.key     = "port",
                        .shape   = ShapePoints::rectangle({0.0f, 0.0f}, 6.0f, 6.0f),
                        .effects = {belowBloom(4.0f)}}};

    const Asked r = ask(s);

    ASSERT_EQ(r.demands.size(), 1u);
    EXPECT_FLOAT_EQ(r.demands[0].blurReach, 4.0f);
}

// ── What a demand carries ───────────────────────────────────────────────────────────────────

TEST(SpriteBelowEmission, TheQuadIsTheLensFootprintVerbatim) {
    const Sprite s = lens("s", {belowGlow(3.0f)});

    const Asked r = ask(s);

    ASSERT_EQ(r.demands.size(), 1u);
    EXPECT_EQ(r.demands[0].field.x, kQuad.x);
    EXPECT_EQ(r.demands[0].field.y, kQuad.y);
    EXPECT_EQ(r.demands[0].field.w, kQuad.w);
    EXPECT_EQ(r.demands[0].field.h, kQuad.h);
}

TEST(SpriteBelowEmission, TheReachIsTheAuthoredRadiusWithNoScaling) {
    // The asymmetry with the region path, and it tracks a real difference: a below effect distorts the SCENE,
    // which is a viewport-resolution image, so its lengths are already viewport pixels. There is no placement
    // scale to apply and no parameter to pass one in.
    for (const float radius : {1.0f, 5.0f, 8.0f, 20.0f}) {
        const Sprite s = lens("s", {belowGlow(radius)});

        const Asked r = ask(s);

        ASSERT_EQ(r.demands.size(), 1u) << "radius " << radius;
        EXPECT_FLOAT_EQ(r.demands[0].blurReach, radius);
        EXPECT_FLOAT_EQ(r.demands[0].field.reach, emissionAtlasSpread(radius));
    }
}

TEST(SpriteBelowEmission, TheExtractIdentityRidesTheDemand) {
    // The field's CONTENT is decided by the extract and the threshold, so both travel with the demand. The
    // extract is an identifier rather than a flag, which is what lets a further extract join additively.
    const Sprite glow  = lens("g", {belowGlow(6.0f, 40)});
    const Sprite bloom = lens("b", {belowBloom(6.0f, 90)});

    const Asked rg = ask(glow);
    const Asked rb = ask(bloom);

    ASSERT_EQ(rg.demands.size(), 1u);
    ASSERT_EQ(rb.demands.size(), 1u);
    EXPECT_EQ(rg.demands[0].extract, EmissionExtract::Glow);
    EXPECT_EQ(rb.demands[0].extract, EmissionExtract::Bloom);
    // Asserted against the record's own lane rather than a hand-normalized number, so the two cannot drift.
    EXPECT_FLOAT_EQ(rg.demands[0].threshold, rg.records[0].params[1]);
    EXPECT_FLOAT_EQ(rb.demands[0].threshold, rb.records[0].params[1]);
    EXPECT_GT(rb.demands[0].threshold, 0.0f);
}

TEST(SpriteBelowEmission, TheStoreIndexIsWhereTheRecordSitsInTheLayer) {
    // A demand names the record to write back to, which is its slice position offset by where the slice was
    // packed. Taking it from the packed slice is what keeps it exact however the lens was authored.
    const Sprite      s      = lens("s", {belowGlow(6.0f), belowBloom(6.0f)});
    const std::size_t offset = 17;

    const Asked r = ask(s, offset);

    ASSERT_EQ(r.demands.size(), 2u);
    EXPECT_EQ(r.demands[0].storeIndex, offset + 0u);
    EXPECT_EQ(r.demands[1].storeIndex, offset + 1u);
}

// ── Seating the answer back ─────────────────────────────────────────────────────────────────

TEST(SpriteBelowEmission, SeatingWritesEachDemandsPackingOrderIndex) {
    const Sprite s = lens("s", {belowGlow(6.0f), belowBloom(6.0f), belowGlow(9.0f)});
    Asked        r = ask(s);
    ASSERT_EQ(r.demands.size(), 3u);
    const std::vector<int> sheetOf{0, 0, 0};

    const int dropped = seatPlacedBelowEmissionFields(r.records, r.demands, sheetOf);

    EXPECT_EQ(dropped, 0);
    for (std::size_t i = 0; i < r.demands.size(); ++i)
        EXPECT_FLOAT_EQ(r.records[r.demands[i].storeIndex].params[3], static_cast<float>(i));
}

TEST(SpriteBelowEmission, SeatingOffsetsByWhereTheLayersFieldsBegin) {
    // A plan spans one layer while the rect table spans the frame, so a layer's first field is not row zero.
    const Sprite s = lens("s", {belowGlow(6.0f), belowBloom(6.0f)});
    Asked        r = ask(s);
    ASSERT_EQ(r.demands.size(), 2u);
    const std::vector<int> sheetOf{0, 0};
    constexpr int          base = 5;

    const int dropped = seatPlacedBelowEmissionFields(r.records, r.demands, sheetOf, base);

    EXPECT_EQ(dropped, 0);
    EXPECT_FLOAT_EQ(r.records[r.demands[0].storeIndex].params[3], static_cast<float>(base));
    EXPECT_FLOAT_EQ(r.records[r.demands[1].storeIndex].params[3], static_cast<float>(base + 1));
}

TEST(SpriteBelowEmission, ADemandNothingCouldPlaceLosesItsStrengthAndIsCounted) {
    const Sprite s = lens("s", {belowGlow(6.0f), belowBloom(6.0f)});
    Asked        r = ask(s);
    ASSERT_EQ(r.demands.size(), 2u);
    const std::vector<int> sheetOf{0, -1};   // the second is larger than any sheet could hold

    const int dropped = seatPlacedBelowEmissionFields(r.records, r.demands, sheetOf);

    EXPECT_EQ(dropped, 1);
    EXPECT_FLOAT_EQ(r.records[r.demands[0].storeIndex].params[3], 0.0f);
    EXPECT_GT(r.records[r.demands[0].storeIndex].params[2], 0.0f);   // the placed one keeps its strength
    EXPECT_FLOAT_EQ(r.records[r.demands[1].storeIndex].params[2], 0.0f);
    EXPECT_FLOAT_EQ(r.records[r.demands[1].storeIndex].params[3], 0.0f);
}

TEST(SpriteBelowEmission, SeatingIgnoresADemandPointingPastTheStore) {
    const Sprite s = lens("s", {belowGlow(6.0f)});
    Asked        r = ask(s, 900);   // an offset no store this small covers
    ASSERT_EQ(r.demands.size(), 1u);
    const std::vector<int> sheetOf{0};

    const int dropped = seatPlacedBelowEmissionFields(r.records, r.demands, sheetOf);

    EXPECT_EQ(dropped, 0);
}
