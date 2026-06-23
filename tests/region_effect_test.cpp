#include "retropp/draw_state.h"
#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/geometry.h"
#include "retropp/transform.h"

// Region-confined screen-space effects. Device-free coverage of the CPU side: the shape
// presets (draw_state.h) and the region gate the GPU region_select.frag mirrors (sdPolygon /
// regionContains / regionParams in postprocess.h). The live gate is build-compiled + dev-verified
// across all three backends (the documented CI-headless boundary); these are the failable units.

namespace retropp {
namespace {

constexpr PixelSize kViewport{160, 144};

// ── Presets — geometry ────────────────────────────────────────────────────────────────

TEST(ShapePresets, Circle) {
    const ShapePoints s = ShapePoints::circle({80, 72}, 30);
    EXPECT_EQ(s.points.size(), 1u);
    EXPECT_FLOAT_EQ(s.radius, 30.0f);
    EXPECT_EQ(s.points[0], (Point{80, 72}));
    EXPECT_TRUE(s.transform.isIdentity());
    EXPECT_TRUE(s.hasRegion());
}

TEST(ShapePresets, Capsule) {
    const ShapePoints s = ShapePoints::capsule({10, 10}, {50, 10}, 5);
    EXPECT_EQ(s.points.size(), 2u);
    EXPECT_FLOAT_EQ(s.radius, 5.0f);
    EXPECT_EQ(s.points[1], (Point{50, 10}));
}

TEST(ShapePresets, Triangle) {
    const ShapePoints s = ShapePoints::triangle({0, 0}, {10, 0}, {0, 10});
    EXPECT_EQ(s.points.size(), 3u);
    EXPECT_FLOAT_EQ(s.radius, 0.0f);
}

TEST(ShapePresets, Rectangle) {
    const ShapePoints s = ShapePoints::rectangle({20, 30}, 40, 50);
    EXPECT_EQ(s.points.size(), 4u);
    EXPECT_EQ(s.points[0], (Point{20, 30}));
    EXPECT_EQ(s.points[2], (Point{60, 80}));  // top-left + (w, h)
}

TEST(ShapePresets, RoundedRectangleIsInsetByRadius) {
    const ShapePoints s = ShapePoints::roundedRectangle({0, 0}, 100, 60, 10);
    EXPECT_EQ(s.points.size(), 4u);
    EXPECT_FLOAT_EQ(s.radius, 10.0f);
    EXPECT_EQ(s.points[0], (Point{10, 10}));  // inset by r so the inflated extent is back to w×h
    EXPECT_EQ(s.points[2], (Point{90, 50}));
}

TEST(ShapePresets, RegularPolygonSideCountUnbounded) {
    EXPECT_EQ(ShapePoints::regularPolygon({0, 0}, 10, 2).points.size(), 3u);    // floored at 3
    EXPECT_EQ(ShapePoints::regularPolygon({0, 0}, 10, 6).points.size(), 6u);
    EXPECT_EQ(ShapePoints::regularPolygon({0, 0}, 10, 64).points.size(), 64u);  // NO upper cap
}

TEST(ShapePresets, DefaultHasNoRegion) {
    const ShapePoints s;
    EXPECT_TRUE(s.points.empty());
    EXPECT_FALSE(s.hasRegion());
}

TEST(ScreenSpaceEffect, RegionDefaultsToWholeViewport) {
    const ScreenSpaceEffect e;
    EXPECT_TRUE(e.region.points.empty());
    EXPECT_FALSE(e.region.hasRegion());
}

// ── regionContains — the gate ─────────────────────────────────────────────────────────

TEST(RegionContains, NoRegionIsAlwaysInside) {
    const ShapePoints none;  // count 0 → whole viewport
    EXPECT_TRUE(regionContains({0, 0}, none));
    EXPECT_TRUE(regionContains({159, 143}, none));
}

TEST(RegionContains, CircleInsideAndOutside) {
    const ShapePoints c = ShapePoints::circle({80, 72}, 30);
    EXPECT_TRUE(regionContains({80, 72}, c));         // centre
    EXPECT_TRUE(regionContains({80, 100}, c));        // 28 px below centre, inside r=30
    EXPECT_FALSE(regionContains({80, 110}, c));       // 38 px below centre, outside
    EXPECT_FALSE(regionContains({80, 72 + 31}, c));   // 31 px out — just past the radius
}

TEST(RegionContains, CapsuleHugsItsSegment) {
    const ShapePoints cap = ShapePoints::capsule({40, 72}, {120, 72}, 10);
    EXPECT_TRUE(regionContains({80, 72}, cap));    // on the spine
    EXPECT_TRUE(regionContains({80, 80}, cap));    // 8 px off the spine, inside r=10
    EXPECT_FALSE(regionContains({80, 90}, cap));   // 18 px off, outside
    EXPECT_TRUE(regionContains({40, 72}, cap));    // an endpoint
    EXPECT_FALSE(regionContains({140, 72}, cap));  // 20 px past the far endpoint
}

TEST(RegionContains, RadiusInflationFlipsAPoint) {
    // A bare triangle (radius 0) excludes a point just outside the hypotenuse; inflating includes it.
    const Point probe{55, 5};
    ShapePoints t = ShapePoints::triangle({0, 0}, {50, 0}, {0, 50});
    EXPECT_FALSE(regionContains(probe, t));
    t.radius = 10.0f;
    EXPECT_TRUE(regionContains(probe, t));
}

TEST(RegionContains, RectangleBounds) {
    const ShapePoints r = ShapePoints::rectangle({20, 20}, 40, 40);  // [20,60] × [20,60]
    EXPECT_TRUE(regionContains({40, 40}, r));   // centre
    EXPECT_TRUE(regionContains({21, 21}, r));   // inside near a corner
    EXPECT_FALSE(regionContains({10, 40}, r));  // left of the rect
    EXPECT_FALSE(regionContains({40, 70}, r));  // below the rect
}

TEST(RegionContains, TransformScaleEnlargesShape) {
    // A small circle that excludes a far point; scaling the region 3× about its centre includes it.
    ShapePoints c = ShapePoints::circle({80, 72}, 10);
    const Point probe{80, 95};  // 23 px from centre — outside r=10
    EXPECT_FALSE(regionContains(probe, c));
    c.transform = Transform::scale(3.0f, 3.0f, 80.0f, 72.0f);  // effective r ≈ 30 about the centre
    EXPECT_TRUE(regionContains(probe, c));
}

TEST(RegionContains, TransformTranslationMovesShape) {
    ShapePoints c = ShapePoints::circle({40, 40}, 15);
    EXPECT_FALSE(regionContains({100, 40}, c));
    c.transform = Transform::translation(60, 0);  // shape centre effectively at (100, 40)
    EXPECT_TRUE(regionContains({100, 40}, c));
}

TEST(RegionContains, UnboundedPolygonPointCount) {
    // A 64-gon (well past any former 16-cap) approximating a circle of radius 40 about (80,72).
    const ShapePoints poly = ShapePoints::regularPolygon({80, 72}, 40, 64);
    EXPECT_EQ(poly.points.size(), 64u);
    EXPECT_TRUE(regionContains({80, 72}, poly));    // centre
    EXPECT_TRUE(regionContains({80, 100}, poly));   // 28 px from centre, inside
    EXPECT_FALSE(regionContains({80, 120}, poly));  // 48 px from centre, outside the ~40 radius
}

// A hand-built arbitrary polygon (a concave arrow) — what unbounded points unlock: a shape no preset
// produces. The notch between the barbs is OUTSIDE the shape (concavity), proving the winding SDF.
TEST(RegionContains, ConcaveArbitraryPolygon) {
    ShapePoints arrow;
    arrow.points = {{80, 20}, {120, 60}, {95, 60}, {95, 120}, {65, 120}, {65, 60}, {40, 60}};
    EXPECT_EQ(arrow.points.size(), 7u);
    EXPECT_TRUE(regionContains({80, 100}, arrow));   // in the shaft
    EXPECT_TRUE(regionContains({80, 45}, arrow));    // in the head
    EXPECT_FALSE(regionContains({45, 100}, arrow));  // beside the shaft (concave notch) — outside
    EXPECT_FALSE(regionContains({80, 130}, arrow));  // below the shaft — outside
}

// ── region invert — confine to the OUTSIDE of the shape ───────────────────────────────

TEST(RegionContains, InvertDefaultsToInside) {
    EXPECT_FALSE(ShapePoints::circle({80, 72}, 30).invert);  // the inside is the region by default
}

TEST(RegionContains, InvertFlipsInsideAndOutside) {
    ShapePoints c = ShapePoints::circle({80, 72}, 30);
    EXPECT_TRUE(regionContains({80, 72}, c));    // centre — inside
    EXPECT_FALSE(regionContains({80, 120}, c));  // 48 px out — outside
    c.invert = true;                             // now the region is the OUTSIDE of the circle
    EXPECT_FALSE(regionContains({80, 72}, c));    // centre is no longer in the region
    EXPECT_TRUE(regionContains({80, 120}, c));    // the outside now IS the region
}

TEST(RegionContains, EmptyRegionIgnoresInvert) {
    ShapePoints none;
    none.invert = true;  // no shape → no confinement; invert is moot
    EXPECT_TRUE(regionContains({0, 0}, none));
    EXPECT_TRUE(regionContains({159, 143}, none));
}

// ── regionParams — the cbuffer mirror ─────────────────────────────────────────────────

TEST(RegionParamsResolve, CountsRadiusAndInverseViewport) {
    const ShapePoints c = ShapePoints::circle({80, 72}, 30);
    const RegionParams p = regionParams(c, kViewport);
    EXPECT_EQ(p.count, 1u);
    EXPECT_FLOAT_EQ(p.radius, 30.0f);
    EXPECT_FLOAT_EQ(p.invViewportW, 1.0f / 160.0f);
    EXPECT_FLOAT_EQ(p.invViewportH, 1.0f / 144.0f);
    EXPECT_FLOAT_EQ(p.invRow0[0], 1.0f);  // identity transform → identity inverse
    EXPECT_FLOAT_EQ(p.invRow1[1], 1.0f);
    EXPECT_FLOAT_EQ(p.invRow2[2], 1.0f);
}

TEST(RegionParamsResolve, TransformInverseIsResolved) {
    ShapePoints c = ShapePoints::circle({0, 0}, 10);
    c.transform = Transform::translation(60, 0);  // inverse is translate(-60, 0)
    const RegionParams p = regionParams(c, kViewport);
    EXPECT_FLOAT_EQ(p.invRow0[2], -60.0f);
}

TEST(RegionParamsResolve, ZeroViewportYieldsZeroInverse) {
    const RegionParams p = regionParams(ShapePoints::circle({0, 0}, 1), PixelSize{0, 0});
    EXPECT_FLOAT_EQ(p.invViewportW, 0.0f);
    EXPECT_FLOAT_EQ(p.invViewportH, 0.0f);
}

}  // namespace
}  // namespace retropp
