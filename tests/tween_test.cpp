// Value animation: the easing curves, the float-vocabulary lerp, the pure tweenAt resolver,
// and the game-owned TweenPlayer. Entirely device-free — a Tween is plain data and the resolver is a
// pure function of (tween, elapsed ticks, TimingProfile, PlaybackMode), so no window or GPU is created.
// Tick numbers are pinned against the GameBoyColor cadence (ticksForDuration(100ms) == 6, exactly as
// animation_test pins them) so a cadence regression is loud here too.

#include <chrono>

#include <gtest/gtest.h>

#include "retropp/animation.h"  // PlaybackMode
#include "retropp/geometry.h"
#include "retropp/timing.h"
#include "retropp/tween.h"

using namespace retropp;
using namespace std::chrono_literals;

namespace {

const TimingProfile gbc = TimingProfile::GameBoyColor;

// All non-Linear easing presets — for the endpoint / clamp sweeps.
constexpr Easing kShaped[] = {
    Easing::InQuad,  Easing::OutQuad,  Easing::InOutQuad,
    Easing::InCubic, Easing::OutCubic, Easing::InOutCubic,
    Easing::InQuart, Easing::OutQuart, Easing::InOutQuart,
    Easing::InQuint, Easing::OutQuint, Easing::InOutQuint,
    Easing::InSine,  Easing::OutSine,  Easing::InOutSine,
    Easing::InExpo,  Easing::OutExpo,  Easing::InOutExpo,
    Easing::InCirc,    Easing::OutCirc,    Easing::InOutCirc,
    Easing::InBack,    Easing::OutBack,    Easing::InOutBack,
    Easing::InElastic, Easing::OutElastic, Easing::InOutElastic,
    Easing::InBounce,  Easing::OutBounce,  Easing::InOutBounce,
};

}  // namespace

// ── ease — endpoints, identity, clamp, known midpoints, overshoot ────────────────────────────────────

TEST(Ease, EveryShapedPresetHasExactEndpoints) {
    for (const Easing e : kShaped) {
        EXPECT_EQ(ease(e, 0.0f), 0.0f) << "endpoint 0 for preset " << static_cast<int>(e);
        EXPECT_EQ(ease(e, 1.0f), 1.0f) << "endpoint 1 for preset " << static_cast<int>(e);
    }
}

TEST(Ease, LinearIsIdentity) {
    EXPECT_FLOAT_EQ(ease(Easing::Linear, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(ease(Easing::Linear, 0.25f), 0.25f);
    EXPECT_FLOAT_EQ(ease(Easing::Linear, 0.5f), 0.5f);
    EXPECT_FLOAT_EQ(ease(Easing::Linear, 1.0f), 1.0f);
}

TEST(Ease, InputIsClampedToUnitInterval) {
    for (const Easing e : kShaped) {
        EXPECT_EQ(ease(e, 2.0f), ease(e, 1.0f));    // > 1 clamps to the 1-endpoint
        EXPECT_EQ(ease(e, -1.0f), ease(e, 0.0f));   // < 0 clamps to the 0-endpoint
    }
    EXPECT_FLOAT_EQ(ease(Easing::Linear, 3.0f), 1.0f);
    EXPECT_FLOAT_EQ(ease(Easing::Linear, -3.0f), 0.0f);
}

TEST(Ease, KnownQuadMidpoints) {
    EXPECT_FLOAT_EQ(ease(Easing::InQuad, 0.5f), 0.25f);     // t²
    EXPECT_FLOAT_EQ(ease(Easing::OutQuad, 0.5f), 0.75f);    // 1-(1-t)²
    EXPECT_FLOAT_EQ(ease(Easing::InOutQuad, 0.5f), 0.5f);   // symmetric crossover
}

TEST(Ease, InOutQuadIsSymmetricAboutTheMidpoint) {
    // ease(0.25) and 1 - ease(0.75) must agree for a symmetric InOut curve.
    EXPECT_FLOAT_EQ(ease(Easing::InOutQuad, 0.25f), 1.0f - ease(Easing::InOutQuad, 0.75f));
}

TEST(Ease, BackOvershootsBeyondOne) {
    // OutBack rises above 1 in the interior before settling back to 1 at the endpoint.
    EXPECT_GT(ease(Easing::OutBack, 0.8f), 1.0f);
    // InBack dips below 0 near the start (anticipation).
    EXPECT_LT(ease(Easing::InBack, 0.2f), 0.0f);
}

TEST(Ease, ElasticSpringsPastTheTarget) {
    // OutElastic rings above 1 early, then decays in to the endpoint.
    EXPECT_GT(ease(Easing::OutElastic, 0.2f), 1.0f);
    // InElastic winds below 0 late in its build-up (the mirror of OutElastic).
    EXPECT_LT(ease(Easing::InElastic, 0.8f), 0.0f);
}

TEST(Ease, BounceStaysInRangeAndHitsKnownTroughs) {
    // Bounce hops UP onto the target and never overshoots past it — unlike Back / Elastic.
    for (int i = 1; i < 20; ++i) {
        const float v = ease(Easing::OutBounce, static_cast<float>(i) * 0.05f);
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0001f);
    }
    EXPECT_NEAR(ease(Easing::OutBounce, 1.5f / 2.75f), 0.75f, 0.001f);          // first hop's trough
    EXPECT_NEAR(ease(Easing::InBounce, 1.0f - 1.5f / 2.75f), 0.25f, 0.001f);   // In is the reflection
}

// ── lerp — float vocabulary ──────────────────────────────────────────────────────────────────────────

TEST(Lerp, FloatEndpointsAndMidpoint) {
    static_assert(lerp(2.0f, 10.0f, 0.0f) == 2.0f);
    static_assert(lerp(2.0f, 10.0f, 1.0f) == 10.0f);
    static_assert(lerp(2.0f, 10.0f, 0.5f) == 6.0f);
    EXPECT_FLOAT_EQ(lerp(2.0f, 10.0f, 0.25f), 4.0f);
}

TEST(Lerp, Vec2Vec3Vec4ComponentWise) {
    static_assert(lerp(Vec2{0, 0}, Vec2{4, 8}, 0.5f) == Vec2{2, 4});
    static_assert(lerp(Vec3{0, 0, 0}, Vec3{2, 4, 6}, 0.5f) == Vec3{1, 2, 3});
    static_assert(lerp(Vec4{0, 0, 0, 0}, Vec4{2, 4, 6, 8}, 0.25f) == Vec4{0.5f, 1, 1.5f, 2});
    // endpoints
    static_assert(lerp(Vec2{1, 2}, Vec2{9, 9}, 0.0f) == Vec2{1, 2});
    static_assert(lerp(Vec2{1, 2}, Vec2{9, 9}, 1.0f) == Vec2{9, 9});
    SUCCEED();
}

// ── tweenAt — single segment ─────────────────────────────────────────────────────────────────────────

namespace {
// A single linear 0→1 ramp over 100ms (6 ticks). Linear so midpoints are exact.
Tween<float> ramp() { return Tween<float>::of(0.0f, 1.0f, 100ms, Easing::Linear); }
}  // namespace

TEST(TweenAtSingleSegment, EndpointsAndEasedMidpoint) {
    const Tween<float> t = ramp();
    EXPECT_EQ(totalTicks(t, gbc), 6u);
    const auto once = PlaybackMode::single();
    EXPECT_FLOAT_EQ(valueAt(t, 0, gbc, once), 0.0f);   // from at t=0
    EXPECT_FLOAT_EQ(valueAt(t, 3, gbc, once), 0.5f);   // linear midpoint (3/6)
    EXPECT_FLOAT_EQ(valueAt(t, 6, gbc, once), 1.0f);   // to at total (clamped)
}

TEST(TweenAtSingleSegment, EasingShapesTheMidpoint) {
    // InQuad over 6 ticks: at the half-window tick (3) localT = 0.5 → 0.25.
    const Tween<float> t = Tween<float>::of(0.0f, 1.0f, 100ms, Easing::InQuad);
    EXPECT_FLOAT_EQ(valueAt(t, 3, gbc, PlaybackMode::single()), 0.25f);
}

// ── tweenAt — multi-segment join + yoyo ──────────────────────────────────────────────────────────────

namespace {
// The headline yoyo: of(0,1,100ms).then(0,100ms) — 0→1 over 6 ticks, then 1→0 over 6 ticks. Linear so
// the join (peak) and the return midpoint are exact. total == 12; one segment == 6.
Tween<float> yoyo() {
    return Tween<float>::of(0.0f, 1.0f, 100ms, Easing::Linear).then(0.0f, 100ms, Easing::Linear);
}
}  // namespace

TEST(TweenAtMultiSegment, JoinValueIsThePeak) {
    const Tween<float> t = yoyo();
    EXPECT_EQ(totalTicks(t, gbc), 12u);
    const auto loop = PlaybackMode::loopIndefinitely();
    // The segment join sits at posInPass == 6: the start of segment 1, value == seg0.to == 1.
    EXPECT_FLOAT_EQ(valueAt(t, 6, gbc, loop), 1.0f);
}

TEST(TweenAtYoyo, ReturnsToFromAcrossOnePassAndIsMidValueHalfwayBack) {
    const Tween<float> t = yoyo();
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_FLOAT_EQ(valueAt(t, 0,  gbc, loop), 0.0f);  // start anchor
    EXPECT_FLOAT_EQ(valueAt(t, 3,  gbc, loop), 0.5f);  // halfway up (seg0 localT 0.5)
    EXPECT_FLOAT_EQ(valueAt(t, 6,  gbc, loop), 1.0f);  // the peak (join)
    EXPECT_FLOAT_EQ(valueAt(t, 9,  gbc, loop), 0.5f);  // halfway back down (seg1 localT 0.5)
    EXPECT_FLOAT_EQ(valueAt(t, 12, gbc, loop), 0.0f);  // one full pass wraps to `from`
    EXPECT_FALSE(tweenAt(t, 12, gbc, loop).finished);
}

// ── tweenAt — playback modes (mirror playbackAt) ─────────────────────────────────────────────────────

TEST(TweenAtSingle, HoldsFinalToAndFlipsFinishedAtTotal) {
    const Tween<float> t = ramp();  // total 6, to = 1
    const auto once = PlaybackMode::single();
    EXPECT_FALSE(tweenAt(t, 5, gbc, once).finished);
    const TweenSample<float> s = tweenAt(t, 6, gbc, once);
    EXPECT_TRUE(s.finished);
    EXPECT_FLOAT_EQ(s.value, 1.0f);
    EXPECT_FLOAT_EQ(valueAt(t, 999, gbc, once), 1.0f);  // holds forever
}

TEST(TweenAtLoopNTimes, HoldsFinalAfterNPasses) {
    const Tween<float> t = yoyo();  // total 12, resting value = 0 (last seg.to)
    const auto n2 = PlaybackMode::loopNTimes(2);  // ends at 24
    EXPECT_FALSE(tweenAt(t, 23, gbc, n2).finished);
    const TweenSample<float> s = tweenAt(t, 24, gbc, n2);
    EXPECT_TRUE(s.finished);
    EXPECT_FLOAT_EQ(s.value, 0.0f);  // the final segment's `to`
    EXPECT_TRUE(tweenAt(t, 0, gbc, PlaybackMode::loopNTimes(0)).finished);  // zero passes → done
}

TEST(TweenAtPlayForDuration, HoldsCutoffValuePastDuration) {
    const Tween<float> t = ramp();  // total 6
    const std::uint64_t cut = gbc.ticksForDuration(50ms);  // 50ms → 3 ticks
    ASSERT_EQ(cut, 3u);
    const auto dur = PlaybackMode::playForDuration(50ms);
    EXPECT_FALSE(tweenAt(t, 2, gbc, dur).finished);
    const TweenSample<float> s = tweenAt(t, 3, gbc, dur);
    EXPECT_TRUE(s.finished);
    // Cutoff value = value shown at the last played tick (cut-1 == 2): linear 2/6 ≈ 0.3333.
    EXPECT_FLOAT_EQ(s.value, valueAt(t, 2, gbc, PlaybackMode::loopIndefinitely()));
    EXPECT_FLOAT_EQ(tweenAt(t, 500, gbc, dur).value, s.value);  // frozen past the cutoff
}

// ── tweenAt — degenerate cases ───────────────────────────────────────────────────────────────────────

TEST(TweenAtDegenerate, EmptyTweenHoldsFromAndIsFinished) {
    const Tween<float> empty{5.0f, {}};
    const TweenSample<float> s = tweenAt(empty, 0, gbc, PlaybackMode::loopIndefinitely());
    EXPECT_FLOAT_EQ(s.value, 5.0f);  // the `from` anchor
    EXPECT_TRUE(s.finished);
    EXPECT_EQ(totalTicks(empty, gbc), 0u);
}

TEST(TweenAtDegenerate, ZeroDurationSegmentSnapsAndNeverRestsThere) {
    // 8ms → 0 ticks. A zero-tick middle segment is an instantaneous snap: never the shown value, never a
    // stall, but it advances the chain so the NEXT segment starts from its `to`.
    ASSERT_EQ(gbc.ticksForDuration(8ms), 0u);
    const Tween<float> t =
        Tween<float>::of(0.0f, 1.0f, 100ms, Easing::Linear)  // 0→1 over 6
            .then(5.0f, 8ms, Easing::Linear)                  // snap to 5 (0 ticks — skipped)
            .then(9.0f, 100ms, Easing::Linear);               // 5→9 over 6
    EXPECT_EQ(totalTicks(t, gbc), 12u);
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_LT(valueAt(t, 5, gbc, loop), 1.0f);  // still climbing in seg0, hasn't reached the snap
    EXPECT_FLOAT_EQ(valueAt(t, 6,  gbc, loop), 5.0f);   // seg2 start: chains from the snapped 5, not 0
    EXPECT_FLOAT_EQ(valueAt(t, 9,  gbc, loop), 7.0f);   // seg2 midpoint: lerp(5, 9, 0.5)
    // Single holds the final segment's `to` (9), never the snapped intermediate.
    EXPECT_FLOAT_EQ(valueAt(t, 100, gbc, PlaybackMode::single()), 9.0f);
}

TEST(TweenAtDegenerate, AllInstantaneousHoldsRestingValue) {
    const Tween<float> t = Tween<float>::of(0.0f, 1.0f, 8ms).then(2.0f, 8ms);  // both 0 ticks
    EXPECT_EQ(totalTicks(t, gbc), 0u);
    EXPECT_FLOAT_EQ(valueAt(t, 0, gbc, PlaybackMode::loopIndefinitely()), 2.0f);  // resting value
    EXPECT_FALSE(tweenAt(t, 0, gbc, PlaybackMode::loopIndefinitely()).finished);  // loop just rests
    EXPECT_TRUE(tweenAt(t, 0, gbc, PlaybackMode::single()).finished);             // finite → done
}

// ── tweenAt — a Vec3 tween (the vector sink) ─────────────────────────────────────────────────────────

TEST(TweenAtVector, Vec3InterpolatesComponentWise) {
    const Tween<Vec3> t = Tween<Vec3>::of(Vec3{0, 0, 0}, Vec3{2, 4, 8}, 100ms, Easing::Linear);
    const Vec3 mid = valueAt(t, 3, gbc, PlaybackMode::single());  // localT 0.5
    EXPECT_FLOAT_EQ(mid.x, 1.0f);
    EXPECT_FLOAT_EQ(mid.y, 2.0f);
    EXPECT_FLOAT_EQ(mid.z, 4.0f);
}

// ── TweenPlayer ──────────────────────────────────────────────────────────────────────────────────────

TEST(TweenPlayer, BareAdvanceLoopsAndAccruesOnlyWhilePlaying) {
    const Tween<float> t = ramp();  // 6 ticks, linear 0→1
    TweenPlayer<float> p{.tween = &t};  // default profile = GBC
    p.advance(PlaybackMode::loopIndefinitely(), 3);  // tick 3 → 0.5
    EXPECT_FLOAT_EQ(p.value(), 0.5f);
    EXPECT_FALSE(p.finished());
    p.pause();
    p.advance(PlaybackMode::loopIndefinitely(), 100);  // frozen — elapsed does not grow
    EXPECT_FLOAT_EQ(p.value(), 0.5f);
    EXPECT_EQ(p.elapsedTicks, 3u);
    p.play();
    p.advance(PlaybackMode::loopIndefinitely(), 3);  // tick 6 → wraps to from (0)
    EXPECT_FLOAT_EQ(p.value(), 0.0f);
}

TEST(TweenPlayer, SingleHoldsFinalValueAndReportsFinished) {
    const Tween<float> t = ramp();
    TweenPlayer<float> p{.tween = &t};
    p.advance(PlaybackMode::single(), 6);
    EXPECT_TRUE(p.finished());
    EXPECT_FLOAT_EQ(p.value(), 1.0f);
}

TEST(TweenPlayer, StopRewindsToFromAndPauses) {
    const Tween<float> t = ramp();
    TweenPlayer<float> p{.tween = &t};
    p.advance(PlaybackMode::loopIndefinitely(), 3);
    p.stop();
    EXPECT_FLOAT_EQ(p.value(), 0.0f);  // rewound to the `from` anchor
    EXPECT_EQ(p.elapsedTicks, 0u);
    EXPECT_FALSE(p.playing);
    EXPECT_FALSE(p.finished());
    p.advance(PlaybackMode::loopIndefinitely(), 3);  // paused → no movement
    EXPECT_FLOAT_EQ(p.value(), 0.0f);
}

TEST(TweenPlayer, RestartRewindsAndPlays) {
    const Tween<float> t = ramp();
    TweenPlayer<float> p{.tween = &t};
    p.advance(PlaybackMode::single(), 6);
    ASSERT_TRUE(p.finished());
    p.restart();
    EXPECT_FLOAT_EQ(p.value(), 0.0f);
    EXPECT_EQ(p.elapsedTicks, 0u);
    EXPECT_TRUE(p.playing);
    EXPECT_FALSE(p.finished());
}

TEST(TweenPlayer, SeekLandsTheValueAtAWallTimeOffset) {
    const Tween<float> t = ramp();  // 100ms total, linear
    TweenPlayer<float> p{.tween = &t};
    p.seek(50ms);  // 50ms → 3 ticks → linear 0.5
    EXPECT_EQ(p.elapsedTicks, 3u);
    EXPECT_FLOAT_EQ(p.value(), 0.5f);
    // a subsequent advance applies the caller's real mode from there
    p.advance(PlaybackMode::single(), 3);  // tick 6 → final value, finished
    EXPECT_FLOAT_EQ(p.value(), 1.0f);
    EXPECT_TRUE(p.finished());
}

TEST(TweenPlayer, NullTweenIsInert) {
    TweenPlayer<float> p{};  // no tween
    p.advance();             // must not crash
    p.seek(100ms);
    p.stop();
    EXPECT_EQ(p.elapsedTicks, 0u);
    EXPECT_FLOAT_EQ(p.value(), 0.0f);
}

TEST(TweenPlayer, DefaultTimingIsConfigurableAndInherited) {
    const TimingProfile saved = TweenPlayer<float>::defaultTiming;
    TweenPlayer<float>::defaultTiming = TimingProfile{TickPeriodNs::Hz60};

    const Tween<float> t = ramp();
    TweenPlayer<float> p{.tween = &t};  // bare — inherits the configured default, not GBC
    EXPECT_EQ(p.profile, TweenPlayer<float>::defaultTiming);
    EXPECT_EQ(p.profile.tickPeriodNs, TickPeriodNs::Hz60);

    TweenPlayer<float> q{.tween = &t, .profile = TimingProfile::GameBoy};  // explicit override
    EXPECT_EQ(q.profile, TimingProfile::GameBoy);

    TweenPlayer<float>::defaultTiming = saved;  // restore (process-wide; no ASSERT above)
    TweenPlayer<float> r{.tween = &t};
    EXPECT_EQ(r.profile, saved);
}
