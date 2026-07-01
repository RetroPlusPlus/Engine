#include "retropp/interpolation.h"

#include <gtest/gtest.h>

#include <span>
#include <variant>
#include <vector>

namespace retropp {
namespace {

// ── Pure lerps ───────────────────────────────────────────────────────────────────────

TEST(InterpolationLerps, LerpRoundAtEndpointsAndMidpoint) {
    EXPECT_EQ(lerpRound(0, 100, 0.0f), 0);
    EXPECT_EQ(lerpRound(0, 100, 1.0f), 100);
    EXPECT_EQ(lerpRound(0, 100, 0.5f), 50);
    EXPECT_EQ(lerpRound(-4, 4, 0.5f), 0);       // straddles zero
    EXPECT_EQ(lerpRound(0, -10, 0.5f), -5);     // rounds toward nearest on the negative side
    static_assert(lerpRound(0, 10, 0.5f) == 5);
}

TEST(InterpolationLerps, LerpFloat) {
    EXPECT_FLOAT_EQ(lerpF(0.0f, 1.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(lerpF(0.0f, 1.0f, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(lerpF(0.0f, 1.0f, 0.25f), 0.25f);
    static_assert(lerpF(2.0f, 4.0f, 0.5f) == 3.0f);
}

TEST(InterpolationLerps, LerpScrollPerAxis) {
    EXPECT_EQ(lerpScroll(LayerScroll{0, 0}, LayerScroll{100, 40}, 0.5f), (LayerScroll{50, 20}));
    EXPECT_EQ(lerpScroll(LayerScroll{0, 0}, LayerScroll{100, 40}, 0.0f), (LayerScroll{0, 0}));
}

TEST(InterpolationLerps, LerpTransformPerCoefficient) {
    const Transform a = Transform::identity();
    const Transform b = Transform::translation(10.0f, 20.0f);
    const Transform mid = lerpTransform(a, b, 0.5f);
    EXPECT_FLOAT_EQ(mid.m02, 5.0f);
    EXPECT_FLOAT_EQ(mid.m12, 10.0f);
    EXPECT_FLOAT_EQ(mid.m00, 1.0f);  // unchanged coefficients stay put
}

// ── Mirror reconcile + interpolate ─────────────────────────────────────────────────────

[[nodiscard]] DrawLayer tileLayer(LayerId id, int sx, int sy, float a = 1.0f) {
    return DrawLayer{.id = id, .label = "L", .z = 0, .scroll = {sx, sy}, .alpha = a};
}
[[nodiscard]] FrameDrawState frameWith(std::vector<DrawLayer> layers) {
    FrameDrawState f;
    f.layers = std::move(layers);
    return f;
}

TEST(Interpolator, ReconcileMountsThenCommitsPrevCur) {
    Interpolator interp;
    interp.reconcile(frameWith({tileLayer(LayerId{7}, 0, 0)}));
    ASSERT_TRUE(interp.layerPrev(LayerId{7}).has_value());
    EXPECT_EQ(interp.layerPrev(LayerId{7})->scroll, (LayerScroll{0, 0}));
    EXPECT_EQ(interp.layerCur(LayerId{7})->scroll, (LayerScroll{0, 0}));
    EXPECT_TRUE(interp.layerChanged(LayerId{7}));  // a mount counts as changed (no prior upload)

    interp.reconcile(frameWith({tileLayer(LayerId{7}, 100, 0)}));
    EXPECT_EQ(interp.layerPrev(LayerId{7})->scroll, (LayerScroll{0, 0}));    // prev <- old cur
    EXPECT_EQ(interp.layerCur(LayerId{7})->scroll, (LayerScroll{100, 0}));   // cur  <- submission
    EXPECT_TRUE(interp.layerChanged(LayerId{7}));
}

TEST(Interpolator, UnchangedIdIsNotFlaggedChanged) {
    Interpolator interp;
    interp.reconcile(frameWith({tileLayer(LayerId{7}, 100, 0)}));
    interp.reconcile(frameWith({tileLayer(LayerId{7}, 100, 0)}));  // same motion again
    EXPECT_FALSE(interp.layerChanged(LayerId{7}));
}

TEST(Interpolator, NewIdMountsAndGoneIdUnmounts) {
    Interpolator interp;
    interp.reconcile(frameWith({tileLayer(LayerId{1}, 0, 0), tileLayer(LayerId{2}, 0, 0)}));
    EXPECT_EQ(interp.layerCount(), 2u);

    interp.reconcile(frameWith({tileLayer(LayerId{1}, 0, 0)}));  // id 2 absent → unmounts
    EXPECT_EQ(interp.layerCount(), 1u);
    EXPECT_TRUE(interp.layerCur(LayerId{1}).has_value());
    EXPECT_FALSE(interp.layerCur(LayerId{2}).has_value());
}

TEST(Interpolator, MatchedLayerEasesBetweenPrevAndSubmission) {
    Interpolator interp;
    interp.reconcile(frameWith({tileLayer(LayerId{7}, 0, 0, 1.0f)}));
    const FrameDrawState f2 = frameWith({tileLayer(LayerId{7}, 100, 40, 0.0f)});
    interp.reconcile(f2);

    const FrameDrawState& out = interp.interpolate(f2, 0.5f);
    ASSERT_EQ(out.layers.size(), 1u);
    EXPECT_EQ(out.layers[0].scroll, (LayerScroll{50, 20}));
    EXPECT_FLOAT_EQ(out.layers[0].alpha, 0.5f);
}

TEST(Interpolator, SpawnSnapsToSubmission) {
    Interpolator interp;
    const FrameDrawState f = frameWith({tileLayer(LayerId{9}, 100, 0)});
    interp.reconcile(f);  // first sight → prev == cur, so it snaps
    const FrameDrawState& out = interp.interpolate(f, 0.5f);
    EXPECT_EQ(out.layers[0].scroll, (LayerScroll{100, 0}));
}

TEST(Interpolator, EmptyMirrorSnapsEverything) {
    Interpolator interp;  // nothing reconciled
    const FrameDrawState f = frameWith({tileLayer(LayerId{9}, 100, 0)});
    const FrameDrawState& out = interp.interpolate(f, 0.5f);
    EXPECT_EQ(out.layers[0].scroll, (LayerScroll{100, 0}));  // no history → snap to submission
}

TEST(Interpolator, BetweenTicksTheMirrorIsUntouched) {
    Interpolator interp;
    interp.reconcile(frameWith({tileLayer(LayerId{7}, 0, 0)}));
    const FrameDrawState f2 = frameWith({tileLayer(LayerId{7}, 100, 0)});
    interp.reconcile(f2);
    const LayerScroll prevBefore = interp.layerPrev(LayerId{7})->scroll;
    const LayerScroll curBefore  = interp.layerCur(LayerId{7})->scroll;

    (void)interp.interpolate(f2, 0.3f);  // several renders between ticks — no reconcile
    (void)interp.interpolate(f2, 0.7f);

    EXPECT_EQ(interp.layerPrev(LayerId{7})->scroll, prevBefore);
    EXPECT_EQ(interp.layerCur(LayerId{7})->scroll, curBefore);
}

TEST(Interpolator, MatchedSpriteEasesByItsId) {
    Interpolator interp;
    const std::vector<Sprite> s1{Sprite{.id = SpriteId{3}, .x = 0, .y = 0}};
    const std::vector<Sprite> s2{Sprite{.id = SpriteId{3}, .x = 80, .y = 40}};

    FrameDrawState f1;
    f1.layers.push_back(DrawLayer{.id = LayerId{1}, .label = "s",
                                  .content = SpriteContent{std::span<const Sprite>(s1)}});
    interp.reconcile(f1);

    FrameDrawState f2;
    f2.layers.push_back(DrawLayer{.id = LayerId{1}, .label = "s",
                                  .content = SpriteContent{std::span<const Sprite>(s2)}});
    interp.reconcile(f2);

    const FrameDrawState& out = interp.interpolate(f2, 0.5f);
    ASSERT_EQ(out.layers.size(), 1u);
    const auto& sprites = std::get<SpriteContent>(out.layers[0].content).sprites;
    ASSERT_EQ(sprites.size(), 1u);
    EXPECT_EQ(sprites[0].x, 40);
    EXPECT_EQ(sprites[0].y, 20);
}

}  // namespace
}  // namespace retropp
