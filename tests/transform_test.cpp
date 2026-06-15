#include <gtest/gtest.h>

#include "retropp/transform.h"

namespace retropp {
namespace {

constexpr float kTol = 1e-4f;

// Field-wise comparison with tolerance — the rotation/perspective cases carry float error EXPECT_EQ
// can't take.
void expectTransformNear(const Transform& got, const Transform& want, float tol = kTol) {
    EXPECT_NEAR(got.m00, want.m00, tol);
    EXPECT_NEAR(got.m01, want.m01, tol);
    EXPECT_NEAR(got.m02, want.m02, tol);
    EXPECT_NEAR(got.m10, want.m10, tol);
    EXPECT_NEAR(got.m11, want.m11, tol);
    EXPECT_NEAR(got.m12, want.m12, tol);
    EXPECT_NEAR(got.m20, want.m20, tol);
    EXPECT_NEAR(got.m21, want.m21, tol);
    EXPECT_NEAR(got.m22, want.m22, tol);
}

// ── Identity ─────────────────────────────────────────────────────────────────────────────

TEST(Transform, DefaultIsIdentity) {
    constexpr Transform t{};
    EXPECT_TRUE(t.isIdentity());
    EXPECT_TRUE(t.isAffine());
    EXPECT_EQ(t, Transform::identity());
}

TEST(Transform, IdentityMapsPointsToThemselves) {
    constexpr Transform t = Transform::identity();
    EXPECT_FLOAT_EQ(t.applyX(7.0f, 11.0f), 7.0f);
    EXPECT_FLOAT_EQ(t.applyY(7.0f, 11.0f), 11.0f);
}

// ── Affine ops (the named constructors all produce affine transforms) ──────────────────────

TEST(Transform, NamedConstructorsAreAffine) {
    EXPECT_TRUE(Transform::translation(5.0f, -3.0f).isAffine());
    EXPECT_TRUE(Transform::scale(2.0f, 3.0f).isAffine());
    EXPECT_TRUE(Transform::skew(1.0f, 0.0f).isAffine());
    EXPECT_TRUE(Transform::rotation(33.0f).isAffine());
}

TEST(Transform, TranslationOffsetsThePoint) {
    constexpr Transform t = Transform::translation(5.0f, -3.0f);
    EXPECT_FLOAT_EQ(t.applyX(10.0f, 10.0f), 15.0f);
    EXPECT_FLOAT_EQ(t.applyY(10.0f, 10.0f), 7.0f);
    EXPECT_FALSE(t.isIdentity());
}

TEST(Transform, ScaleAboutOrigin) {
    constexpr Transform t = Transform::scale(2.0f, 3.0f);
    EXPECT_FLOAT_EQ(t.applyX(4.0f, 5.0f), 8.0f);
    EXPECT_FLOAT_EQ(t.applyY(4.0f, 5.0f), 15.0f);
}

TEST(Transform, ScaleAboutPivotFixesThePivot) {
    constexpr Transform t = Transform::scale(2.0f, 2.0f, 10.0f, 10.0f);
    EXPECT_FLOAT_EQ(t.applyX(10.0f, 10.0f), 10.0f);   // pivot fixed
    EXPECT_FLOAT_EQ(t.applyY(10.0f, 10.0f), 10.0f);
    EXPECT_FLOAT_EQ(t.applyX(11.0f, 10.0f), 12.0f);   // one unit right → two
}

TEST(Transform, NegativeScaleMirrorsAboutPivot) {
    constexpr Transform t = Transform::scale(-1.0f, 1.0f, 80.0f, 0.0f);
    EXPECT_FLOAT_EQ(t.applyX(80.0f, 0.0f), 80.0f);   // pivot fixed
    EXPECT_FLOAT_EQ(t.applyX(90.0f, 0.0f), 70.0f);   // reflected across x=80
}

TEST(Transform, HorizontalSkewSlantsByY) {
    constexpr Transform t = Transform::skew(1.0f, 0.0f);  // x' = x + y
    EXPECT_FLOAT_EQ(t.applyX(0.0f, 5.0f), 5.0f);
    EXPECT_FLOAT_EQ(t.applyY(0.0f, 5.0f), 5.0f);
}

TEST(Transform, Rotate90AboutOrigin) {
    const Transform t = Transform::rotation(90.0f);
    EXPECT_NEAR(t.applyX(1.0f, 0.0f), 0.0f, kTol);   // top-left-origin clockwise: +X → (0,1)
    EXPECT_NEAR(t.applyY(1.0f, 0.0f), 1.0f, kTol);
}

TEST(Transform, RotateAboutPivotFixesThePivot) {
    const Transform t = Transform::rotation(37.0f, 80.0f, 72.0f);
    EXPECT_NEAR(t.applyX(80.0f, 72.0f), 80.0f, kTol);
    EXPECT_NEAR(t.applyY(80.0f, 72.0f), 72.0f, kTol);
}

TEST(Transform, Rotate360IsNearIdentity) {
    expectTransformNear(Transform::rotation(360.0f), Transform::identity());
}

// ── Perspective (the Mode-7-style floor) ──────────────────────────────────────────────────

TEST(Transform, PerspectiveForeshortensWithDepth) {
    // w = 1 + 0.01*y: content y=0 maps unchanged; content y=100 has w=2 → halved (recedes).
    constexpr Transform t = Transform::perspective(0.0f, 0.01f);
    EXPECT_FALSE(t.isAffine());
    EXPECT_FLOAT_EQ(t.applyY(0.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(t.applyY(40.0f, 100.0f), 50.0f);   // 100 / 2
    EXPECT_FLOAT_EQ(t.applyX(40.0f, 100.0f), 20.0f);   //  40 / 2
}

// ── Composition ──────────────────────────────────────────────────────────────────────────

TEST(Transform, ThenAppliesThisFirstThenNext) {
    // Translate (+10, 0) THEN scale ×2 about origin: (1,1) → (11,1) → (22,2).
    constexpr Transform t = Transform::translation(10.0f, 0.0f).then(Transform::scale(2.0f, 2.0f));
    EXPECT_FLOAT_EQ(t.applyX(1.0f, 1.0f), 22.0f);
    EXPECT_FLOAT_EQ(t.applyY(1.0f, 1.0f), 2.0f);
}

TEST(Transform, ThenMatchesSequentialApply_Affine) {
    const Transform a = Transform::rotation(25.0f, 3.0f, 4.0f);
    const Transform b = Transform::scale(1.5f, 0.5f, 1.0f, 1.0f);
    const Transform ab = a.then(b);
    const float px = 9.0f, py = 2.0f;
    EXPECT_NEAR(ab.applyX(px, py), b.applyX(a.applyX(px, py), a.applyY(px, py)), kTol);
    EXPECT_NEAR(ab.applyY(px, py), b.applyY(a.applyX(px, py), a.applyY(px, py)), kTol);
}

TEST(Transform, ThenMatchesSequentialApply_Perspective) {
    const Transform a = Transform::perspective(0.002f, 0.004f);
    const Transform b = Transform::rotation(18.0f, 80.0f, 72.0f);
    const Transform ab = a.then(b);
    const float px = 30.0f, py = 90.0f;
    EXPECT_NEAR(ab.applyX(px, py), b.applyX(a.applyX(px, py), a.applyY(px, py)), 1e-3f);
    EXPECT_NEAR(ab.applyY(px, py), b.applyY(a.applyX(px, py), a.applyY(px, py)), 1e-3f);
}

// ── Inverse ──────────────────────────────────────────────────────────────────────────────

TEST(Transform, InverseRoundTripsToIdentity_Affine) {
    const Transform t = Transform::rotation(31.0f, 50.0f, 40.0f)
                            .then(Transform::scale(2.0f, 0.5f))
                            .then(Transform::translation(7.0f, -9.0f));
    expectTransformNear(t.then(t.inverse()), Transform::identity());
    expectTransformNear(t.inverse().then(t), Transform::identity());
}

TEST(Transform, InverseUndoesAPoint_Affine) {
    const Transform t = Transform::rotation(63.0f, 12.0f, 34.0f);
    const Transform inv = t.inverse();
    const float fx = t.applyX(20.0f, 5.0f), fy = t.applyY(20.0f, 5.0f);
    EXPECT_NEAR(inv.applyX(fx, fy), 20.0f, kTol);
    EXPECT_NEAR(inv.applyY(fx, fy), 5.0f, kTol);
}

TEST(Transform, InverseUndoesAPoint_Perspective) {
    // The whole point: the tile fragment uses inverse() on a PERSPECTIVE transform, so it must
    // round-trip a point through the perspective divide.
    const Transform t = Transform::perspective(0.001f, 0.003f).then(Transform::rotation(20.0f, 80.0f, 72.0f));
    const Transform inv = t.inverse();
    const float fx = t.applyX(64.0f, 110.0f), fy = t.applyY(64.0f, 110.0f);
    EXPECT_NEAR(inv.applyX(fx, fy), 64.0f, 1e-2f);
    EXPECT_NEAR(inv.applyY(fx, fy), 110.0f, 1e-2f);
}

TEST(Transform, SingularInverseFallsBackToIdentity) {
    constexpr Transform degenerate = Transform::scale(0.0f, 1.0f);  // det == 0
    EXPECT_TRUE(degenerate.inverse().isIdentity());
}

// ── Compile-time (the constexpr subset folds) ────────────────────────────────────────────

TEST(Transform, ConstexprSubsetFoldsAtCompileTime) {
    static_assert(Transform::identity().isIdentity());
    static_assert(Transform::translation(2.0f, 3.0f).applyX(0.0f, 0.0f) == 2.0f);
    static_assert(Transform::scale(2.0f, 2.0f).applyX(3.0f, 0.0f) == 6.0f);
    static_assert(Transform::scale(2.0f, 1.0f).inverse().applyX(6.0f, 0.0f) == 3.0f);
    static_assert(Transform::perspective(0.0f, 0.01f).applyY(0.0f, 100.0f) == 50.0f);
    static_assert(Transform::translation(1.0f, 0.0f).then(Transform::translation(2.0f, 0.0f))
                      == Transform::translation(3.0f, 0.0f));
    SUCCEED();
}

}  // namespace
}  // namespace retropp
