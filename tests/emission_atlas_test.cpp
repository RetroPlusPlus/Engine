// Emission atlas planning — the geometry that keeps one field's blur out of another's content.
//
// A field's rect is its consumer's quad grown by a margin of ceil(reach) + 2, and a field is read inside
// that quad or one texel past it — a bilinear sample at the quad's edge draws support from the first ring.
// So the property these tests exist for is per-rect: every placed rect's inner box is inset by its own
// margin, which exceeds its reach by more than a texel, and therefore no blur tap can leave the rect it
// started in from the inner box OR from that first ring. Rects pack adjacently; the margin alone is the
// isolation.
//
// Everything here is device-free — the arithmetic decides where a field lives, so the arithmetic is what
// is asserted. Cases 1 and 2 are properties over a whole plan rather than hand-picked layouts, because a
// packer that happens to separate one chosen pair proves nothing about the next.

#include <gtest/gtest.h>

#include <retropp/emission_atlas.h>

#include <cmath>
#include <span>
#include <vector>

using namespace retropp;

namespace {

EmissionDemand demand(int x, int y, int w, int h, float reach) {
    return EmissionDemand{.x = x, .y = y, .w = w, .h = h, .reach = reach};
}

EmissionAtlasPlan planOf(const std::vector<EmissionDemand>& demands, int maxSize) {
    return planEmissionAtlas(std::span<const EmissionDemand>{demands}, maxSize);
}

// Case 1 as a property: every placed rect is its demand's quad inflated by a margin that exceeds the
// reach by more than one texel, on all four sides — so a tap from the inner box stays inside the rect, and
// so does a tap from the first ring outside it, which is where a bilinear read's support lands.
void expectMarginProperty(const std::vector<EmissionDemand>& demands, const EmissionAtlasPlan& plan) {
    ASSERT_EQ(plan.placements.size(), demands.size());
    for (std::size_t i = 0; i < demands.size(); ++i) {
        const EmissionPlacement& p = plan.placements[i];
        if (p.page < 0) continue;
        const EmissionDemand& d      = demands[i];
        const int             margin = emissionMargin(d.reach);
        EXPECT_GE(margin, static_cast<int>(std::ceil(d.reach > 0.0f ? d.reach : 0.0f)) + 2)
            << "demand " << i << " margin does not clear its reach and a ring texel";
        EXPECT_GT(static_cast<float>(margin), d.reach + 1.0f)
            << "demand " << i << " tap from the read's own support can leave its rect";
        EXPECT_EQ(p.rectW, d.w + 2 * margin) << "demand " << i << " inner box not inset horizontally";
        EXPECT_EQ(p.rectH, d.h + 2 * margin) << "demand " << i << " inner box not inset vertically";
    }
}

// Case 2 as a property: no two placed rects share a texel.
void expectNoOverlap(const EmissionAtlasPlan& plan) {
    for (std::size_t a = 0; a < plan.placements.size(); ++a) {
        const EmissionPlacement& ra = plan.placements[a];
        if (ra.page < 0) continue;
        for (std::size_t b = a + 1; b < plan.placements.size(); ++b) {
            const EmissionPlacement& rb = plan.placements[b];
            if (rb.page < 0) continue;
            const bool disjoint = ra.rectX + ra.rectW <= rb.rectX || rb.rectX + rb.rectW <= ra.rectX ||
                                  ra.rectY + ra.rectH <= rb.rectY || rb.rectY + rb.rectH <= ra.rectY;
            EXPECT_TRUE(disjoint) << "rects " << a << " and " << b << " overlap";
        }
    }
}

int placedCount(const EmissionAtlasPlan& plan) {
    int n = 0;
    for (const EmissionPlacement& p : plan.placements)
        if (p.page >= 0) ++n;
    return n;
}

bool isPowerOfTwo(int v) { return v > 0 && (v & (v - 1)) == 0; }

}  // namespace

// ── The isolation property ──────────────────────────────────────────────────────────────────

TEST(EmissionAtlas, EveryPlacedRectInsetsItsContentPastItsReachAndARingTexel) {
    // A mixed plan: several reaches, several sizes, enough members per page to open shelves.
    std::vector<EmissionDemand> demands;
    for (int i = 0; i < 12; ++i)
        demands.push_back(demand(i * 7, i * 5, 20 + i * 3, 16 + (i % 4) * 9, static_cast<float>(i % 3) * 6.5f));

    const EmissionAtlasPlan plan = planOf(demands, 1024);

    EXPECT_EQ(placedCount(plan), 12);
    expectMarginProperty(demands, plan);
}

TEST(EmissionAtlas, PlacedRectsNeverOverlap) {
    std::vector<EmissionDemand> demands;
    for (int i = 0; i < 24; ++i)
        demands.push_back(demand(0, 0, 30 + (i % 5) * 11, 24 + (i % 7) * 8, static_cast<float>(i % 4) * 4.0f));

    const EmissionAtlasPlan plan = planOf(demands, 1024);

    EXPECT_EQ(placedCount(plan), 24);
    expectNoOverlap(plan);
}

TEST(EmissionAtlas, ASingleFieldSitsItsMarginInsideItsRectAndItsBand) {
    const std::vector<EmissionDemand> demands{demand(10, 20, 40, 30, 5.0f)};

    const EmissionAtlasPlan plan = planOf(demands, 512);

    const int margin = emissionMargin(5.0f);
    EXPECT_EQ(margin, 7);
    ASSERT_EQ(plan.placements.size(), 1u);
    const EmissionPlacement& p = plan.placements[0];
    ASSERT_EQ(p.page, 0);
    EXPECT_EQ(p.rectW, 40 + 2 * margin);
    EXPECT_EQ(p.rectH, 30 + 2 * margin);

    // The content is `margin` inside the rect, and the rect starts at the band's own origin.
    ASSERT_EQ(plan.pages.size(), 1u);
    const EmissionPage& page = plan.pages[0];
    EXPECT_EQ(p.rectX + margin - page.x, margin);
    EXPECT_EQ(p.rectY + margin - page.y, margin);
}

// ── Pages ───────────────────────────────────────────────────────────────────────────────────

TEST(EmissionAtlas, FieldsOfOneReachShareAPageAndFieldsOfAnotherDoNot) {
    const std::vector<EmissionDemand> same{demand(0, 0, 32, 32, 6.0f), demand(0, 0, 32, 32, 6.0f)};
    const EmissionAtlasPlan           sharedPlan = planOf(same, 512);
    EXPECT_EQ(sharedPlan.pages.size(), 1u);
    EXPECT_EQ(sharedPlan.placements[0].page, sharedPlan.placements[1].page);

    const std::vector<EmissionDemand> differing{demand(0, 0, 32, 32, 6.0f), demand(0, 0, 32, 32, 10.0f)};
    const EmissionAtlasPlan           splitPlan = planOf(differing, 512);
    EXPECT_EQ(splitPlan.pages.size(), 2u);
    EXPECT_NE(splitPlan.placements[0].page, splitPlan.placements[1].page);
}

TEST(EmissionAtlas, ReachesOneUlpApartAreTwoPages) {
    const float                       reach = 6.0f;
    const float                       nudged = std::nextafter(reach, 7.0f);
    const std::vector<EmissionDemand> demands{demand(0, 0, 32, 32, reach), demand(0, 0, 32, 32, nudged)};

    const EmissionAtlasPlan plan = planOf(demands, 512);

    EXPECT_NE(reach, nudged);
    EXPECT_EQ(plan.pages.size(), 2u);
    EXPECT_NE(plan.placements[0].page, plan.placements[1].page);
}

TEST(EmissionAtlas, PageOrderIsFirstAppearanceNotSortedByReach) {
    const std::vector<EmissionDemand> demands{demand(0, 0, 32, 32, 8.0f), demand(0, 0, 32, 32, 3.0f),
                                              demand(0, 0, 32, 32, 8.0f), demand(0, 0, 32, 32, 3.0f)};

    const EmissionAtlasPlan plan = planOf(demands, 512);

    ASSERT_EQ(plan.pages.size(), 2u);
    EXPECT_FLOAT_EQ(plan.pages[0].reach, 8.0f);
    EXPECT_FLOAT_EQ(plan.pages[1].reach, 3.0f);
    EXPECT_EQ(plan.placements[0].page, 0);
    EXPECT_EQ(plan.placements[1].page, 1);
}

// ── Determinism and ordering ────────────────────────────────────────────────────────────────

TEST(EmissionAtlas, TheSameInputPlansIdentically) {
    std::vector<EmissionDemand> demands;
    for (int i = 0; i < 16; ++i)
        demands.push_back(demand(i, i * 2, 20 + (i % 6) * 13, 20 + (i % 5) * 7, static_cast<float>(i % 3) * 5.0f));

    const EmissionAtlasPlan first  = planOf(demands, 1024);
    const EmissionAtlasPlan second = planOf(demands, 1024);

    EXPECT_EQ(first.width, second.width);
    EXPECT_EQ(first.height, second.height);
    EXPECT_EQ(first.dropped, second.dropped);
    ASSERT_EQ(first.pages.size(), second.pages.size());
    ASSERT_EQ(first.placements.size(), second.placements.size());
    for (std::size_t i = 0; i < first.placements.size(); ++i) {
        EXPECT_EQ(first.placements[i].rectX, second.placements[i].rectX);
        EXPECT_EQ(first.placements[i].rectY, second.placements[i].rectY);
        EXPECT_EQ(first.placements[i].page, second.placements[i].page);
    }
}

TEST(EmissionAtlas, PlacementsStayParallelToTheInputThoughPackingSorts) {
    // Packing sorts a page tallest-first; the returned placements must still index like the input.
    const std::vector<EmissionDemand> demands{demand(0, 0, 40, 10, 2.0f), demand(0, 0, 40, 90, 2.0f),
                                              demand(0, 0, 40, 50, 2.0f)};

    const EmissionAtlasPlan plan = planOf(demands, 512);

    const int margin = emissionMargin(2.0f);
    ASSERT_EQ(plan.placements.size(), 3u);
    EXPECT_EQ(plan.placements[0].rectH, 10 + 2 * margin);
    EXPECT_EQ(plan.placements[1].rectH, 90 + 2 * margin);
    EXPECT_EQ(plan.placements[2].rectH, 50 + 2 * margin);
}

// ── Degenerate and boundary demands ─────────────────────────────────────────────────────────

TEST(EmissionAtlas, AReachOfZeroOrLessStillEarnsTheTwoTexelSkirt) {
    const std::vector<EmissionDemand> demands{demand(0, 0, 32, 32, 0.0f), demand(0, 0, 32, 32, -4.0f)};

    const EmissionAtlasPlan plan = planOf(demands, 512);

    EXPECT_EQ(emissionMargin(0.0f), 2);
    EXPECT_EQ(emissionMargin(-4.0f), 2);
    EXPECT_EQ(placedCount(plan), 2);
    EXPECT_EQ(plan.placements[0].rectW, 36);
    EXPECT_EQ(plan.placements[0].rectH, 36);
    EXPECT_EQ(plan.dropped, 0);
    expectMarginProperty(demands, plan);
}

TEST(EmissionAtlas, AFieldLargerThanTheAtlasIsDroppedAndTheRestStillPlace) {
    const std::vector<EmissionDemand> demands{demand(0, 0, 32, 32, 4.0f), demand(0, 0, 1000, 32, 4.0f),
                                              demand(0, 0, 32, 1000, 4.0f), demand(0, 0, 48, 48, 4.0f)};

    const EmissionAtlasPlan plan = planOf(demands, 256);

    EXPECT_EQ(plan.dropped, 2);
    EXPECT_EQ(plan.placements[1].page, -1);
    EXPECT_EQ(plan.placements[2].page, -1);
    EXPECT_GE(plan.placements[0].page, 0);
    EXPECT_GE(plan.placements[3].page, 0);
    expectMarginProperty(demands, plan);
}

TEST(EmissionAtlas, AFieldWithNoAreaIsDroppedNotPlacedAtZeroSize) {
    const std::vector<EmissionDemand> demands{demand(0, 0, 0, 32, 4.0f), demand(0, 0, 32, -5, 4.0f),
                                              demand(0, 0, 32, 32, 4.0f)};

    const EmissionAtlasPlan plan = planOf(demands, 256);

    EXPECT_EQ(plan.dropped, 2);
    EXPECT_EQ(plan.placements[0].page, -1);
    EXPECT_EQ(plan.placements[0].rectW, 0);
    EXPECT_EQ(plan.placements[1].page, -1);
    EXPECT_EQ(plan.placements[1].rectH, 0);
    EXPECT_GE(plan.placements[2].page, 0);
}

TEST(EmissionAtlas, NoDemandPlansNothing) {
    const std::vector<EmissionDemand> demands{};

    const EmissionAtlasPlan plan = planOf(demands, 1024);

    EXPECT_EQ(plan.width, 0);
    EXPECT_EQ(plan.height, 0);
    EXPECT_TRUE(plan.pages.empty());
    EXPECT_TRUE(plan.placements.empty());
    EXPECT_EQ(plan.dropped, 0);
}

// ── Atlas sizing and overflow ───────────────────────────────────────────────────────────────

TEST(EmissionAtlas, DimensionsArePowersOfTwoThatHoldEveryPlacedRect) {
    std::vector<EmissionDemand> demands;
    for (int i = 0; i < 20; ++i)
        demands.push_back(demand(0, 0, 50 + (i % 3) * 20, 40 + (i % 4) * 15, static_cast<float>(i % 2) * 7.0f));

    const EmissionAtlasPlan plan = planOf(demands, 2048);

    EXPECT_TRUE(isPowerOfTwo(plan.width)) << "width " << plan.width;
    EXPECT_TRUE(isPowerOfTwo(plan.height)) << "height " << plan.height;
    for (const EmissionPlacement& p : plan.placements) {
        if (p.page < 0) continue;
        EXPECT_LE(p.rectX + p.rectW, plan.width);
        EXPECT_LE(p.rectY + p.rectH, plan.height);
    }
}

TEST(EmissionAtlas, ContentThatExactlyFillsTheAtlasPlacesWithoutDropping) {
    // Four 128x128 rects (124 + a 2-texel skirt each side) tile a 256x256 atlas precisely.
    const std::vector<EmissionDemand> demands{demand(0, 0, 124, 124, 0.0f), demand(0, 0, 124, 124, 0.0f),
                                              demand(0, 0, 124, 124, 0.0f), demand(0, 0, 124, 124, 0.0f)};

    const EmissionAtlasPlan plan = planOf(demands, 256);

    EXPECT_EQ(plan.dropped, 0);
    EXPECT_EQ(placedCount(plan), 4);
    EXPECT_EQ(plan.width, 256);
    EXPECT_EQ(plan.height, 256);
    expectNoOverlap(plan);
}

TEST(EmissionAtlas, DemandBeyondTheAtlasOverflowsAndWhatFitsIsStillIsolated) {
    // One more than the previous case holds, at the same ceiling.
    std::vector<EmissionDemand> demands;
    for (int i = 0; i < 5; ++i) demands.push_back(demand(0, 0, 124, 124, 0.0f));

    const EmissionAtlasPlan plan = planOf(demands, 256);

    EXPECT_EQ(placedCount(plan), 4);
    EXPECT_EQ(plan.dropped, 1);
    expectMarginProperty(demands, plan);
    expectNoOverlap(plan);
}

TEST(EmissionAtlas, TheAtlasWidensRatherThanDroppingWhatAWiderOneWouldHold) {
    // Twenty 128x128 rects stack ten shelves deep at 256 wide — past a 1024 ceiling — but only five
    // shelves at 512. The planner widens rather than dropping the overflow.
    std::vector<EmissionDemand> demands;
    for (int i = 0; i < 20; ++i) demands.push_back(demand(0, 0, 124, 124, 0.0f));

    const EmissionAtlasPlan plan = planOf(demands, 1024);

    EXPECT_EQ(plan.dropped, 0);
    EXPECT_EQ(placedCount(plan), 20);
    EXPECT_EQ(plan.width, 512);
    expectNoOverlap(plan);
}
