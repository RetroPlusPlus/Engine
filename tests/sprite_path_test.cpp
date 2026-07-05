#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <vector>

#include "retropp/animation.h"
#include "retropp/curve.h"
#include "retropp/draw_state.h"   // Sprite, Rotation, ObjectKey
#include "retropp/sprite_path.h"
#include "retropp/tween.h"

namespace retropp {
namespace {

using namespace std::chrono_literals;

// SpritePath is the movement orchestrator: it plays one node — a movement spec paced along a curve,
// composed with rotation / scale tween tracks, an animation, and a facing policy, off one clock — and
// writes the composed result into a Sprite. Every check here is a headless value comparison; no GPU.

// One second of the default Game Boy Color cadence rounds to this many ticks — the tween/animation math is
// tick-quantized, so a half-second is 30 ticks and a full pass is 60.
constexpr std::uint64_t kTicksPerSecond = 60;

void expectNear(Vec2 got, Vec2 want, float eps = 0.5f) {
    EXPECT_NEAR(got.x, want.x, eps);
    EXPECT_NEAR(got.y, want.y, eps);
}

// Advance `n` ticks under one mode (each tick re-resolves, exactly as a game's sim loop drives it).
void run(SpritePath& p, int n, PlaybackMode mode = PlaybackMode::loopIndefinitely()) {
    for (int i = 0; i < n; ++i) p.advance(mode);
}

// ── Move-spec resolution ─────────────────────────────────────────────────────────────────────────────

TEST(SpritePathMove, LineInheritsTheStartAsItsOrigin) {
    SpritePath p{.node = {.move = SpritePathMove::to({60.0f, 0.0f})}, .start = {0.0f, 0.0f}};
    p.advance();                               // bakes; parked at the origin (default pacing = Speed 0)
    expectNear(p.position(), {0.0f, 0.0f});    // the origin is the start
}

TEST(SpritePathMove, LineAuthoredOriginOverridesTheStart) {
    SpritePath p{.node = {.move = SpritePathMove::to({40.0f, 40.0f}, {80.0f, 40.0f})},
                 .start = {0.0f, 0.0f}};
    p.advance();
    expectNear(p.position(), {40.0f, 40.0f});  // the authored origin, not the start
}

TEST(SpritePathMove, ThroughPointsPrependsTheOriginAndPassesThroughInOrder) {
    SpritePath p{.node = {.move   = SpritePathMove::through({{40.0f, 0.0f}, {80.0f, 0.0f}}),
                          .pacing = PathPacing::speed(400.0f)},
                 .start = {0.0f, 0.0f}};
    p.restart();                                    // resolve at elapsed 0 — no tick accrued yet
    expectNear(p.position(), {0.0f, 0.0f}, 1.0f);   // begins at the prepended origin (the start)
    run(p, 40, PlaybackMode::single());
    expectNear(p.position(), {80.0f, 0.0f}, 1.5f);  // ends at the last listed point
}

TEST(SpritePathMove, HermiteTravelsFromOriginToDestination) {
    SpritePath p{.node = {.move   = SpritePathMove::hermite({0.0f, 0.0f}, {60.0f, 0.0f},
                                                            {40.0f, 40.0f}, {40.0f, -40.0f}),
                          .pacing = PathPacing::speed(400.0f)},
                 .start = {0.0f, 0.0f}};
    p.restart();                                    // resolve at elapsed 0 — no tick accrued yet
    expectNear(p.position(), {0.0f, 0.0f}, 1.0f);
    run(p, 60, PlaybackMode::single());
    expectNear(p.position(), {60.0f, 0.0f}, 1.5f);
}

TEST(SpritePathMove, OnCurveUsesTheCurveGeometryAndIgnoresTheStart) {
    const Curve c = Curve::line({20.0f, 20.0f}, {30.0f, 20.0f});
    SpritePath  p{.node = {.move = SpritePathMove::onCurve(c)}, .start = {100.0f, 100.0f}};
    p.advance();
    expectNear(p.position(), {20.0f, 20.0f});  // where the curve starts, not the start anchor
}

TEST(SpritePathMove, NamedConstructorsEqualTheAggregateForm) {
    EXPECT_EQ(SpritePathMove::to({50.0f, 10.0f}),
              (SpritePathMove{.kind = SpritePathMove::Kind::Line, .destination = {50.0f, 10.0f}}));
    EXPECT_EQ(SpritePathMove::to({5.0f, 5.0f}, {50.0f, 10.0f}),
              (SpritePathMove{.kind        = SpritePathMove::Kind::Line,
                              .origin      = Vec2{5.0f, 5.0f},
                              .destination = {50.0f, 10.0f}}));
    EXPECT_EQ(SpritePathMove::through({{1.0f, 2.0f}, {3.0f, 4.0f}}),
              (SpritePathMove{.kind = SpritePathMove::Kind::ThroughPoints,
                              .points = {{1.0f, 2.0f}, {3.0f, 4.0f}}}));
    EXPECT_EQ(SpritePathMove::hermite({9.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}),
              (SpritePathMove{.kind               = SpritePathMove::Kind::Hermite,
                              .destination        = {9.0f, 0.0f},
                              .originTangent      = {1.0f, 0.0f},
                              .destinationTangent = {0.0f, 1.0f}}));
}

// ── Composition of the concurrent tracks ──────────────────────────────────────────────────────────────

// A parked path (Speed 0) so movement stays put and the track under test is the only thing changing.
SpritePath parkedPath() {
    return SpritePath{.node = {.move = SpritePathMove::to({10.0f, 0.0f})}, .start = {0.0f, 0.0f}};
}

TEST(SpritePathCompose, RotationTrackResolvesMidPass) {
    SpritePath p = parkedPath();
    p.node.rotationDegrees = Tween<float>::of(0.0f, 90.0f, 1s, Easing::Linear);
    run(p, static_cast<int>(kTicksPerSecond / 2));  // halfway through a linear 0→90 pass
    EXPECT_NEAR(p.rotationDegrees(), 45.0f, 0.5f);
}

TEST(SpritePathCompose, ScaleTrackResolvesMidPass) {
    SpritePath p = parkedPath();
    p.node.scale = Tween<Vec2>::of({1.0f, 1.0f}, {2.0f, 2.0f}, 1s, Easing::Linear);
    run(p, static_cast<int>(kTicksPerSecond / 2));
    expectNear(p.scaleValue(), {1.5f, 1.5f}, 0.02f);
}

TEST(SpritePathCompose, RotationAndScaleRunConcurrentlyOffOneClock) {
    SpritePath p = parkedPath();
    p.node.rotationDegrees = Tween<float>::of(0.0f, 90.0f, 1s, Easing::Linear);
    p.node.scale           = Tween<Vec2>::of({1.0f, 1.0f}, {3.0f, 3.0f}, 1s, Easing::Linear);
    run(p, static_cast<int>(kTicksPerSecond / 2));
    EXPECT_NEAR(p.rotationDegrees(), 45.0f, 0.5f);
    expectNear(p.scaleValue(), {2.0f, 2.0f}, 0.02f);
}

TEST(SpritePathCompose, RotationTrackSingleModeRestsAtItsFinalValue) {
    SpritePath p = parkedPath();
    p.node.rotationDegrees = Tween<float>::of(0.0f, 90.0f, 1s, Easing::Linear);
    p.node.rotationMode    = PlaybackMode::single();
    run(p, static_cast<int>(kTicksPerSecond * 2));  // well past the single pass
    EXPECT_NEAR(p.rotationDegrees(), 90.0f, 0.01f);
}

TEST(SpritePathCompose, RotationTrackLoopModeWrapsBackToTheStart) {
    SpritePath p = parkedPath();
    p.node.rotationDegrees = Tween<float>::of(0.0f, 90.0f, 1s, Easing::Linear);
    p.node.rotationMode    = PlaybackMode::loopIndefinitely();
    run(p, static_cast<int>(kTicksPerSecond));  // exactly one pass → wraps to posInPass 0
    EXPECT_NEAR(p.rotationDegrees(), 0.0f, 0.01f);
}

TEST(SpritePathCompose, AbsentTracksContributeIdentity) {
    SpritePath p = parkedPath();  // no rotation track, no scale track, FacingPolicy::None
    run(p, 10);
    EXPECT_NEAR(p.rotationDegrees(), 0.0f, 0.01f);
    expectNear(p.scaleValue(), {1.0f, 1.0f}, 0.001f);
}

// ── Facing policies ──────────────────────────────────────────────────────────────────────────────────

// A parked path facing along a straight line in a chosen direction (the tangent is that direction).
SpritePath facingLine(Vec2 direction, FacingPolicy policy) {
    SpritePath p{.node = {.move = SpritePathMove::to(direction), .facing = policy}, .start = {0.0f, 0.0f}};
    return p;
}

TEST(SpritePathFacing, RotateToFacingEastIsZeroDegrees) {
    SpritePath p = facingLine({10.0f, 0.0f}, FacingPolicy::RotateToFacing);
    p.advance();
    EXPECT_NEAR(p.rotationDegrees(), 0.0f, 0.01f);
}

TEST(SpritePathFacing, RotateToFacingSouthIsNinetyDegrees) {
    SpritePath p = facingLine({0.0f, 10.0f}, FacingPolicy::RotateToFacing);
    p.advance();
    EXPECT_NEAR(p.rotationDegrees(), 90.0f, 0.01f);  // +y is 90° in the top-left-origin clockwise space
}

TEST(SpritePathFacing, RotateToFacingWestIsOneEightyDegrees) {
    SpritePath p = facingLine({-10.0f, 0.0f}, FacingPolicy::RotateToFacing);
    p.advance();
    EXPECT_NEAR(std::abs(p.rotationDegrees()), 180.0f, 0.01f);
}

TEST(SpritePathFacing, RotateToFacingNorthIsMinusNinetyDegrees) {
    SpritePath p = facingLine({0.0f, -10.0f}, FacingPolicy::RotateToFacing);
    p.advance();
    EXPECT_NEAR(p.rotationDegrees(), -90.0f, 0.01f);
}

TEST(SpritePathFacing, RotateToFacingSumsWithTheRotationTrack) {
    SpritePath p = facingLine({10.0f, 0.0f}, FacingPolicy::RotateToFacing);  // facing contributes 0°
    p.node.rotationDegrees = Tween<float>::of(30.0f, 30.0f, 1s, Easing::Linear);  // constant +30°
    p.advance();
    EXPECT_NEAR(p.rotationDegrees(), 30.0f, 0.01f);  // 0 (facing east) + 30 (track)
}

TEST(SpritePathFacing, FlipXIsTrueTravellingTowardNegativeX) {
    SpritePath p = facingLine({-10.0f, 0.0f}, FacingPolicy::FlipX);
    p.advance();
    EXPECT_TRUE(p.flipX());
}

TEST(SpritePathFacing, FlipXIsFalseTravellingTowardPositiveX) {
    SpritePath p = facingLine({10.0f, 0.0f}, FacingPolicy::FlipX);
    p.advance();
    EXPECT_FALSE(p.flipX());
}

TEST(SpritePathFacing, FlipXHoldsItsPreviousValueWhileTheHorizontalComponentIsZero) {
    SpritePath p = facingLine({0.0f, 10.0f}, FacingPolicy::FlipX);  // purely vertical tangent (x == 0)
    p.sample.flipX = true;   // an already-mirrored sprite
    p.advance();
    EXPECT_TRUE(p.flipX());  // vertical travel does not flip it back

    SpritePath q = facingLine({0.0f, 10.0f}, FacingPolicy::FlipX);
    q.sample.flipX = false;
    q.advance();
    EXPECT_FALSE(q.flipX());
}

TEST(SpritePathFacing, NoneLeavesRotationAndFlipUntouched) {
    SpritePath p = facingLine({-10.0f, 0.0f}, FacingPolicy::None);
    p.advance();
    EXPECT_NEAR(p.rotationDegrees(), 0.0f, 0.01f);
    EXPECT_FALSE(p.flipX());
}

// ── applyTo — the write table ────────────────────────────────────────────────────────────────────────

TEST(SpritePathApplyTo, QuantizesPositionIncludingNegativeCoordinates) {
    SpritePath p = parkedPath();
    p.advance();
    p.sample.position = {10.6f, -3.4f};  // white-box: pin an exact float position
    Sprite s{.key = "mover"};
    p.applyTo(s);
    EXPECT_EQ(s.x, 11);  // std::lround(10.6)
    EXPECT_EQ(s.y, -3);  // std::lround(-3.4)
}

TEST(SpritePathApplyTo, WritesTheFrameArtFieldsWhenAnAnimationTrackIsPresent) {
    const Animation anim{.frames = {AnimationFrame{.atlas    = AtlasId{7},
                                                   .slot     = AssetSlot{.tile = 5,
                                                                         .dimensions = AssetDimensions::GameBoy8x16},
                                                   .palette  = PaletteId{3},
                                                   .duration = 1s}}};
    SpritePath p = parkedPath();
    p.node.animation = &anim;
    p.advance();
    Sprite s{.key = "mover"};
    p.applyTo(s);
    EXPECT_EQ(s.atlas, AtlasId{7});
    EXPECT_EQ(s.tile, 5);
    EXPECT_EQ(s.size, AssetDimensions::GameBoy8x16);
    EXPECT_EQ(s.palette, PaletteId{3});
}

TEST(SpritePathApplyTo, ComposesScaleThenRotationAboutTheDefaultCentrePivot) {
    SpritePath p = parkedPath();
    p.node.rotationDegrees = Tween<float>::of(90.0f, 90.0f, 1s, Easing::Linear);  // constant 90°
    p.node.scale           = Tween<Vec2>::of({2.0f, 2.0f}, {2.0f, 2.0f}, 1s, Easing::Linear);  // constant 2×
    p.advance();
    Sprite s{.key = "mover"};  // default size 8×8 → centre pivot (4, 4)
    p.applyTo(s);
    const Transform expected =
        Transform::scale(2.0f, 2.0f, 4.0f, 4.0f).then(Transform::rotation(90.0f, 4.0f, 4.0f));
    EXPECT_EQ(s.transform, expected);
}

TEST(SpritePathApplyTo, HonoursAnExplicitPivotOverride) {
    SpritePath p = parkedPath();
    p.node.rotationDegrees = Tween<float>::of(45.0f, 45.0f, 1s, Easing::Linear);
    p.node.pivot           = Vec2{0.0f, 0.0f};
    p.advance();
    Sprite s{.key = "mover"};
    p.applyTo(s);
    const Transform expected =
        Transform::scale(1.0f, 1.0f, 0.0f, 0.0f).then(Transform::rotation(45.0f, 0.0f, 0.0f));
    EXPECT_EQ(s.transform, expected);
}

TEST(SpritePathApplyTo, DefaultPivotUsesTheFrameSizeAfterTheFrameWrite) {
    const Animation anim{.frames = {AnimationFrame{.atlas    = AtlasId{1},
                                                   .slot     = AssetSlot{.tile = 0,
                                                                         .dimensions = AssetDimensions::GameBoy8x16},
                                                   .palette  = PaletteId{0},
                                                   .duration = 1s}}};
    SpritePath p = parkedPath();
    p.node.animation       = &anim;
    p.node.rotationDegrees = Tween<float>::of(90.0f, 90.0f, 1s, Easing::Linear);
    p.advance();
    Sprite s{.key = "mover"};
    p.applyTo(s);  // size becomes 8×16 → centre pivot (4, 8)
    const Transform expected =
        Transform::scale(1.0f, 1.0f, 4.0f, 8.0f).then(Transform::rotation(90.0f, 4.0f, 8.0f));
    EXPECT_EQ(s.transform, expected);
}

TEST(SpritePathApplyTo, LeavesUndeclaredFieldsUntouched) {
    SpritePath p = parkedPath();  // no animation, no tracks, FacingPolicy::None
    p.advance();
    Sprite s{.key = "keepme"};
    s.alpha            = 0.5f;
    s.flipY            = true;
    s.rotation         = Rotation::Rot90;
    const Transform t0 = Transform::translation(3.0f, 4.0f);
    s.transform        = t0;
    const ObjectKey saved = s.key;

    p.applyTo(s);

    EXPECT_EQ(s.key, saved);              // reconciliation identity is never rewritten
    EXPECT_FLOAT_EQ(s.alpha, 0.5f);
    EXPECT_TRUE(s.flipY);
    EXPECT_EQ(s.rotation, Rotation::Rot90);
    EXPECT_EQ(s.transform, t0);           // no track declared → transform left as the game set it
}

// ── sample() ↔ applyTo parity ────────────────────────────────────────────────────────────────────────

TEST(SpritePathParity, ApplyToMatchesTheSampleOnTheSharedValues) {
    const Animation anim{.frames = {AnimationFrame{.atlas    = AtlasId{2},
                                                   .slot     = AssetSlot{.tile = 9,
                                                                         .dimensions = AssetDimensions::GameBoy8x8},
                                                   .palette  = PaletteId{4},
                                                   .duration = 1s}}};
    SpritePath p{.node = {.move      = SpritePathMove::to({-40.0f, 0.0f}),
                          .pacing    = PathPacing::speed(200.0f),
                          .facing    = FacingPolicy::FlipX,
                          .animation = &anim},
                 .start = {0.0f, 0.0f}};
    run(p, 6);
    Sprite s{.key = "mover"};
    p.applyTo(s);
    EXPECT_EQ(s.x, static_cast<int>(std::lround(p.position().x)));
    EXPECT_EQ(s.y, static_cast<int>(std::lround(p.position().y)));
    EXPECT_EQ(s.flipX, p.flipX());
    EXPECT_EQ(s.tile, p.frame()->slot.tile);
}

// ── The cursor ────────────────────────────────────────────────────────────────────────────────────────

TEST(SpritePathCursor, AdvanceAccruesOnlyWhilePlaying) {
    SpritePath p{.node = {.move = SpritePathMove::to({60.0f, 0.0f}), .pacing = PathPacing::speed(120.0f)},
                 .start = {0.0f, 0.0f}};
    run(p, 5);
    const std::uint64_t at5   = p.elapsedTicks;
    const Vec2          pos5  = p.position();
    p.pause();
    run(p, 5);
    EXPECT_EQ(p.elapsedTicks, at5);      // paused → no accrual
    expectNear(p.position(), pos5, 0.001f);
    p.play();
    run(p, 5);
    EXPECT_EQ(p.elapsedTicks, at5 + 5);  // resumed
}

TEST(SpritePathCursor, StopRewindsToTheStartAndPauses) {
    SpritePath p{.node = {.move = SpritePathMove::to({60.0f, 0.0f}), .pacing = PathPacing::speed(120.0f)},
                 .start = {10.0f, 10.0f}};
    run(p, 10);
    p.stop();
    EXPECT_EQ(p.elapsedTicks, 0u);
    EXPECT_FALSE(p.playing);
    expectNear(p.position(), {10.0f, 10.0f});  // back at the origin
}

TEST(SpritePathCursor, RestartRewindsAndResumes) {
    SpritePath p{.node = {.move = SpritePathMove::to({60.0f, 0.0f}), .pacing = PathPacing::speed(120.0f)},
                 .start = {10.0f, 10.0f}};
    run(p, 10);
    p.restart();
    EXPECT_EQ(p.elapsedTicks, 0u);
    EXPECT_TRUE(p.playing);
    expectNear(p.position(), {10.0f, 10.0f});
}

TEST(SpritePathCursor, SeekJumpsToAWallTimeOffset) {
    SpritePath p{.node = {.move = SpritePathMove::to({60.0f, 0.0f}), .pacing = PathPacing::speed(60.0f)},
                 .start = {0.0f, 0.0f}};
    p.advance();
    p.seek(500ms);  // half a second at 60 px/s ≈ 30 px along the line
    expectNear(p.position(), {30.0f, 0.0f}, 1.5f);
}

TEST(SpritePathCursor, BakesLazilyOnTheFirstAdvance) {
    SpritePath p{.node = {.move = SpritePathMove::to({10.0f, 0.0f})}, .start = {0.0f, 0.0f}};
    EXPECT_FALSE(p.baked);
    p.advance();
    EXPECT_TRUE(p.baked);
}

TEST(SpritePathCursor, RestartRebakesForARepath) {
    SpritePath p{.node = {.move = SpritePathMove::to({100.0f, 0.0f})}, .start = {0.0f, 0.0f}};
    run(p, 5);
    // Re-path: assign a node whose move starts somewhere the old geometry never did, then restart.
    p.node = SpritePathNode{.move = SpritePathMove::to({20.0f, 20.0f}, {20.0f, 80.0f})};
    p.restart();
    expectNear(p.position(), {20.0f, 20.0f});  // the new origin, so the arc re-baked
}

TEST(SpritePathCursor, FinishedUnderSingleButNotUnderLoop) {
    SpritePath single{.node = {.move = SpritePathMove::to({30.0f, 0.0f}), .pacing = PathPacing::speed(300.0f)},
                      .start = {0.0f, 0.0f}};
    run(single, 30, PlaybackMode::single());
    EXPECT_TRUE(single.finished());

    SpritePath loop{.node = {.move = SpritePathMove::to({30.0f, 0.0f}), .pacing = PathPacing::speed(300.0f)},
                    .start = {0.0f, 0.0f}};
    run(loop, 30, PlaybackMode::loopIndefinitely());
    EXPECT_FALSE(loop.finished());
}

TEST(SpritePathCursor, ParkedPacingStaysAtTheOrigin) {
    SpritePath p{.node = {.move = SpritePathMove::to({60.0f, 0.0f})},  // default pacing = Speed 0
                 .start = {12.0f, 34.0f}};
    run(p, 50);
    expectNear(p.position(), {12.0f, 34.0f});
    EXPECT_FLOAT_EQ(p.distance(), 0.0f);
}

TEST(SpritePathCursor, DegenerateEmptyMoveDoesNotCrashAndFinishesUnderSingle) {
    SpritePath p{.node = {.move = SpritePathMove::through({})},  // origin prepended → one point → empty curve
                 .start = {0.0f, 0.0f}};
    run(p, 5, PlaybackMode::single());
    EXPECT_TRUE(p.finished());       // a zero-length path is immediately finished under a finite mode
    EXPECT_FLOAT_EQ(p.distance(), 0.0f);
}

}  // namespace
}  // namespace retropp
