// ENG-2.H — animations: the pure playback resolver, programmatic frame access, duration/tick handling,
// and the game-owned AnimationPlayer. Entirely device-free — Animation is plain data and the resolver
// is a pure function of (frames, elapsed ticks, TimingProfile, PlaybackMode), so no window or GPU is
// created. Tick numbers are pinned against the GameBoyColor cadence: ticksForDuration(100ms) == 6 and
// a 3×100ms animation totals 18 ticks (the bridge is asserted exactly so a cadence regression is loud).

#include <chrono>

#include <gtest/gtest.h>

#include "retropp/animation.h"
#include "retropp/renderer.h"  // AtlasManifest::frame shorthand (no device created)
#include "retropp/timing.h"

using namespace retropp;
using namespace std::chrono_literals;

namespace {

constexpr AssetDimensions k8  = AssetDimensions::GameBoy8x8;
const TimingProfile        gbc = TimingProfile::GameBoyColor;

// 3 frames, 100ms each (6 ticks each → 18 total), distinct atlas/slot/palette/labels.
Animation makeUniform() {
    return Animation{{
        AnimationFrame{"a", static_cast<AtlasId>(1), AssetSlot{0, k8}, static_cast<PaletteId>(10), 100ms},
        AnimationFrame{"b", static_cast<AtlasId>(1), AssetSlot{1, k8}, static_cast<PaletteId>(11), 100ms},
        AnimationFrame{"c", static_cast<AtlasId>(1), AssetSlot{2, k8}, static_cast<PaletteId>(12), 100ms},
    }};
}

std::size_t idx(const Animation& a, std::uint64_t t, PlaybackMode m) {
    return playbackAt(a, t, gbc, m).frameIndex;
}
bool fin(const Animation& a, std::uint64_t t, PlaybackMode m) {
    return playbackAt(a, t, gbc, m).finished;
}

}  // namespace

// ── The pinned bridge: 100ms == 6 ticks, total == 18 ────────────────────────────────────────────────

TEST(Animation, FrameTicksAndTotalArePinnedToGbcCadence) {
    EXPECT_EQ(gbc.ticksForDuration(100ms), 6u);
    EXPECT_EQ(totalTicks(makeUniform(), gbc), 18u);
}

// ── playbackAt — LoopIndefinitely ──────────────────────────────────────────────────────────────────

TEST(PlaybackAtLoopIndefinitely, FrameWindows) {
    const Animation a = makeUniform();
    const auto loop = PlaybackMode::loopIndefinitely();
    // window 0 = [0,6), window 1 = [6,12), window 2 = [12,18)
    EXPECT_EQ(idx(a, 0,  loop), 0u);
    EXPECT_EQ(idx(a, 5,  loop), 0u);
    EXPECT_EQ(idx(a, 6,  loop), 1u);
    EXPECT_EQ(idx(a, 11, loop), 1u);
    EXPECT_EQ(idx(a, 12, loop), 2u);
    EXPECT_EQ(idx(a, 17, loop), 2u);
}

TEST(PlaybackAtLoopIndefinitely, WrapsModuloTotal) {
    const Animation a = makeUniform();
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(idx(a, 18, loop), 0u);  // exactly one full pass wraps to frame 0
    EXPECT_EQ(idx(a, 19, loop), 0u);  // 19 % 18 = 1 → still window 0
    EXPECT_EQ(idx(a, 24, loop), 1u);  // 24 % 18 = 6 → window 1
}

TEST(PlaybackAtLoopIndefinitely, LargeMultiplesWrap) {
    const Animation a = makeUniform();
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(idx(a, 100u * 18u + 7u, loop), 1u);  // +7 → window 1
    EXPECT_EQ(idx(a, 100u * 18u + 13u, loop), 2u);  // +13 → window 2
}

TEST(PlaybackAtLoopIndefinitely, NeverFinished) {
    const Animation a = makeUniform();
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_FALSE(fin(a, 0, loop));
    EXPECT_FALSE(fin(a, 18, loop));
    EXPECT_FALSE(fin(a, 100000, loop));
}

// ── playbackAt — Single ──────────────────────────────────────────────────────────────────────────

TEST(PlaybackAtSingle, PlaysThenClampsToLastFrame) {
    const Animation a = makeUniform();
    const auto once = PlaybackMode::single();
    EXPECT_EQ(idx(a, 0,  once), 0u);
    EXPECT_EQ(idx(a, 11, once), 1u);
    EXPECT_FALSE(fin(a, 11, once));
    EXPECT_EQ(idx(a, 17, once), 2u);
    EXPECT_FALSE(fin(a, 17, once));
    // at/after total: clamp to the last frame, finished.
    EXPECT_EQ(idx(a, 18, once), 2u);
    EXPECT_TRUE(fin(a, 18, once));
    EXPECT_EQ(idx(a, 999, once), 2u);
    EXPECT_TRUE(fin(a, 999, once));
}

// ── playbackAt — LoopNTimes ────────────────────────────────────────────────────────────────────────

TEST(PlaybackAtLoopNTimes, WrapsForNPassesThenHolds) {
    const Animation a = makeUniform();  // total 18
    const auto n3 = PlaybackMode::loopNTimes(3);  // ends at 54
    EXPECT_EQ(idx(a, 6,  n3), 1u);
    EXPECT_FALSE(fin(a, 6, n3));
    EXPECT_EQ(idx(a, 24, n3), 1u);   // pass 2, window 1
    EXPECT_FALSE(fin(a, 53, n3));     // one tick before the end
    EXPECT_TRUE(fin(a, 54, n3));      // exactly n·total → finished
    EXPECT_EQ(idx(a, 54, n3), 2u);    // holding the last frame
    EXPECT_TRUE(fin(a, 1000, n3));
}

TEST(PlaybackAtLoopNTimes, SinglePassBoundary) {
    const Animation a = makeUniform();
    const auto n1 = PlaybackMode::loopNTimes(1);  // ends at 18 — like Single
    EXPECT_FALSE(fin(a, 17, n1));
    EXPECT_TRUE(fin(a, 18, n1));
    EXPECT_EQ(idx(a, 18, n1), 2u);
}

TEST(PlaybackAtLoopNTimes, ZeroPassesIsImmediatelyFinished) {
    const Animation a = makeUniform();
    EXPECT_TRUE(fin(a, 0, PlaybackMode::loopNTimes(0)));
}

// ── playbackAt — PlayForDuration ─────────────────────────────────────────────────────────────────

TEST(PlaybackAtPlayForDuration, WrapsUntilCutoffThenHolds) {
    const Animation a = makeUniform();  // total 18
    // 500ms → 30 ticks (spans ~1.66 passes). Plays/wraps before the cutoff, holds after.
    const std::uint64_t cut = gbc.ticksForDuration(500ms);
    const auto dur = PlaybackMode::playForDuration(500ms);
    ASSERT_EQ(cut, 30u);
    EXPECT_EQ(idx(a, 6, dur), 1u);
    EXPECT_FALSE(fin(a, 6, dur));
    EXPECT_EQ(idx(a, 24, dur), 1u);   // 24 % 18 = 6 → window 1, still playing
    EXPECT_FALSE(fin(a, 29, dur));     // one tick before cutoff
    EXPECT_TRUE(fin(a, 30, dur));      // at the cutoff → finished
    EXPECT_TRUE(fin(a, 500, dur));
}

TEST(PlaybackAtPlayForDuration, ShorterThanOneFrame) {
    const Animation a = makeUniform();
    const std::uint64_t cut = gbc.ticksForDuration(50ms);  // 50ms → 3 ticks (< one 6-tick frame)
    ASSERT_EQ(cut, 3u);
    const auto dur = PlaybackMode::playForDuration(50ms);
    EXPECT_EQ(idx(a, 2, dur), 0u);
    EXPECT_FALSE(fin(a, 2, dur));
    EXPECT_TRUE(fin(a, 3, dur));
    EXPECT_EQ(idx(a, 3, dur), 0u);  // never left frame 0
}

// ── Duration unit handling ────────────────────────────────────────────────────────────────────────

TEST(AnimationDuration, MsLiteralAndExplicitMillisecondsAreIdentical) {
    Animation lit{{AnimationFrame{"", AtlasId{}, AssetSlot{0, k8}, PaletteId{}, 100ms}}};
    Animation exp{{AnimationFrame{"", AtlasId{}, AssetSlot{0, k8}, PaletteId{},
                                  std::chrono::milliseconds(100)}}};
    EXPECT_EQ(totalTicks(lit, gbc), totalTicks(exp, gbc));
    EXPECT_EQ(totalTicks(lit, gbc), 6u);
}

TEST(AnimationDuration, SecondsResolveAndMixedUnitsResolve) {
    EXPECT_EQ(gbc.ticksForDuration(2s), 119u);  // 2s / 16.742706ms ≈ 119.46 → 119
    Animation mixed{{
        AnimationFrame{"", AtlasId{}, AssetSlot{0, k8}, PaletteId{}, 100ms},  // 6
        AnimationFrame{"", AtlasId{}, AssetSlot{1, k8}, PaletteId{}, 2s},     // 119
    }};
    EXPECT_EQ(totalTicks(mixed, gbc), 125u);
}

// ── Tick quantization ─────────────────────────────────────────────────────────────────────────────

TEST(AnimationTickQuantization, RoundsToNearest) {
    EXPECT_EQ(gbc.ticksForDuration(30ms), 2u);  // 30/16.74 ≈ 1.79 → rounds UP to 2
    EXPECT_EQ(gbc.ticksForDuration(25ms), 1u);  // 25/16.74 ≈ 1.49 → rounds DOWN to 1
}

TEST(AnimationTickQuantization, ZeroTickFrameIsSkippedNeverRestingNeverStalls) {
    // 8ms → 0 ticks. The middle frame is instantaneous: it must never be shown and must not stall.
    ASSERT_EQ(gbc.ticksForDuration(8ms), 0u);
    Animation a{{
        AnimationFrame{"x", AtlasId{}, AssetSlot{0, k8}, PaletteId{}, 100ms},  // 6
        AnimationFrame{"gone", AtlasId{}, AssetSlot{1, k8}, PaletteId{}, 8ms}, // 0 — skipped
        AnimationFrame{"y", AtlasId{}, AssetSlot{2, k8}, PaletteId{}, 100ms},  // 6
    }};
    EXPECT_EQ(totalTicks(a, gbc), 12u);
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(idx(a, 0,  loop), 0u);
    EXPECT_EQ(idx(a, 5,  loop), 0u);
    EXPECT_EQ(idx(a, 6,  loop), 2u);   // skips straight from frame 0 to frame 2 — never frame 1
    EXPECT_EQ(idx(a, 11, loop), 2u);
    EXPECT_EQ(idx(a, 12, loop), 0u);   // wraps without ever resting on the zero-tick frame
    // Single clamps to the last positive frame (2), not the zero-tick frame.
    EXPECT_EQ(idx(a, 100, PlaybackMode::single()), 2u);
}

// ── Programmatic access ───────────────────────────────────────────────────────────────────────────

TEST(AnimationAccess, IndexAndCount) {
    const Animation a = makeUniform();
    EXPECT_EQ(a.count(), 3u);
    EXPECT_EQ(a[0].label, "a");
    EXPECT_EQ(a[2].slot.tile, 2);
}

TEST(AnimationAccess, IndexOfAndFind) {
    const Animation a = makeUniform();
    ASSERT_TRUE(a.indexOf("b").has_value());
    EXPECT_EQ(*a.indexOf("b"), 1u);
    EXPECT_EQ(a.find("c"), &a.frames[2]);
    EXPECT_EQ(a.indexOf("missing"), std::nullopt);
    EXPECT_EQ(a.find("missing"), nullptr);
}

TEST(AnimationAccess, FirstMatchOnDuplicateLabels) {
    Animation a{{
        AnimationFrame{"dup", AtlasId{}, AssetSlot{0, k8}, PaletteId{}, 100ms},
        AnimationFrame{"dup", AtlasId{}, AssetSlot{9, k8}, PaletteId{}, 100ms},
    }};
    ASSERT_TRUE(a.indexOf("dup").has_value());
    EXPECT_EQ(*a.indexOf("dup"), 0u);            // first match
    EXPECT_EQ(a.find("dup")->slot.tile, 0);
}

// ── Multi-atlas frames (the arbitrary-source requirement) ───────────────────────────────────────────

TEST(AnimationMultiAtlas, FramesCarryDistinctAtlasIds) {
    Animation a{{
        AnimationFrame{"fromSheet", static_cast<AtlasId>(1), AssetSlot{3, k8}, PaletteId{}, 100ms},
        AnimationFrame{"fromOneOff", static_cast<AtlasId>(2), AssetSlot{0, k8}, PaletteId{}, 100ms},
    }};
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(frameAt(a, 0, gbc, loop).atlas, static_cast<AtlasId>(1));
    EXPECT_EQ(frameAt(a, 6, gbc, loop).atlas, static_cast<AtlasId>(2));
}

// ── Palette-cycling (the same unit, art constant) ──────────────────────────────────────────────────

TEST(AnimationPaletteCycle, SameSlotDifferentPalettePerWindow) {
    Animation a{{
        AnimationFrame{"", AtlasId{}, AssetSlot{0, k8}, static_cast<PaletteId>(10), 100ms},
        AnimationFrame{"", AtlasId{}, AssetSlot{0, k8}, static_cast<PaletteId>(11), 100ms},
        AnimationFrame{"", AtlasId{}, AssetSlot{0, k8}, static_cast<PaletteId>(12), 100ms},
    }};
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(frameAt(a, 0,  gbc, loop).slot.tile, 0);       // art constant
    EXPECT_EQ(frameAt(a, 0,  gbc, loop).palette, static_cast<PaletteId>(10));
    EXPECT_EQ(frameAt(a, 6,  gbc, loop).palette, static_cast<PaletteId>(11));
    EXPECT_EQ(frameAt(a, 12, gbc, loop).palette, static_cast<PaletteId>(12));
}

// ── Degenerate guards ─────────────────────────────────────────────────────────────────────────────

TEST(AnimationDegenerate, EmptyAnimation) {
    Animation empty{};
    EXPECT_EQ(empty.count(), 0u);
    EXPECT_EQ(totalTicks(empty, gbc), 0u);
    const PlaybackState s = playbackAt(empty, 0, gbc, PlaybackMode::loopIndefinitely());
    EXPECT_EQ(s.frameIndex, 0u);
    EXPECT_TRUE(s.finished);
}

TEST(AnimationDegenerate, SingleFrameLoopStaysOnFrameZero) {
    Animation a{{AnimationFrame{"only", AtlasId{}, AssetSlot{0, k8}, PaletteId{}, 100ms}}};
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(idx(a, 0, loop), 0u);
    EXPECT_EQ(idx(a, 5, loop), 0u);
    EXPECT_EQ(idx(a, 6, loop), 0u);  // wraps to itself
    EXPECT_EQ(idx(a, 999, loop), 0u);
}

// ── AnimationPlayer ─────────────────────────────────────────────────────────────────────────────

TEST(AnimationPlayer, BareAdvanceLoopsByDefault) {
    const Animation a = makeUniform();
    AnimationPlayer p{.animation = &a};  // default profile = GBC
    p.advance();  // +1 tick
    EXPECT_EQ(p.currentIndex(), 0u);
    EXPECT_FALSE(p.finished());
    p.advance(PlaybackMode::loopIndefinitely(), 6);  // now at tick 7
    EXPECT_EQ(p.currentIndex(), 1u);
    p.advance(PlaybackMode::loopIndefinitely(), 12);  // tick 19 → window 0
    EXPECT_EQ(p.currentIndex(), 0u);
    EXPECT_FALSE(p.finished());
}

TEST(AnimationPlayer, SingleHoldsFinalFrameAndReportsFinished) {
    const Animation a = makeUniform();
    AnimationPlayer p{.animation = &a};
    p.advance(PlaybackMode::single(), 18);
    EXPECT_EQ(p.currentIndex(), 2u);
    EXPECT_TRUE(p.finished());
    EXPECT_EQ(p.current().label, "c");
}

TEST(AnimationPlayer, LoopNTimesFinishesAfterNPasses) {
    const Animation a = makeUniform();
    AnimationPlayer p{.animation = &a};
    p.advance(PlaybackMode::loopNTimes(2), 35);
    EXPECT_FALSE(p.finished());
    p.advance(PlaybackMode::loopNTimes(2), 1);  // tick 36 = 2·18
    EXPECT_TRUE(p.finished());
    EXPECT_EQ(p.currentIndex(), 2u);
}

TEST(AnimationPlayer, PlayForDurationFinishesPastDuration) {
    const Animation a = makeUniform();
    AnimationPlayer p{.animation = &a};
    const auto dur = PlaybackMode::playForDuration(200ms);  // 200ms → 12 ticks
    ASSERT_EQ(gbc.ticksForDuration(200ms), 12u);
    p.advance(dur, 11);
    EXPECT_FALSE(p.finished());
    p.advance(dur, 1);  // tick 12 = cutoff
    EXPECT_TRUE(p.finished());
}

TEST(AnimationPlayer, AccruesOnlyWhilePlaying) {
    const Animation a = makeUniform();
    AnimationPlayer p{.animation = &a};
    p.advance(PlaybackMode::loopIndefinitely(), 6);  // tick 6
    EXPECT_EQ(p.currentIndex(), 1u);
    p.pause();
    p.advance(PlaybackMode::loopIndefinitely(), 100);  // frozen — elapsed does not grow
    EXPECT_EQ(p.currentIndex(), 1u);
    EXPECT_EQ(p.elapsedTicks, 6u);
    p.play();
    p.advance(PlaybackMode::loopIndefinitely(), 6);  // tick 12
    EXPECT_EQ(p.currentIndex(), 2u);
}

TEST(AnimationPlayer, StopRewindsAndPauses) {
    const Animation a = makeUniform();
    AnimationPlayer p{.animation = &a};
    p.advance(PlaybackMode::loopIndefinitely(), 12);
    p.stop();
    EXPECT_EQ(p.currentIndex(), 0u);
    EXPECT_EQ(p.elapsedTicks, 0u);
    EXPECT_FALSE(p.playing);
    p.advance(PlaybackMode::loopIndefinitely(), 6);  // paused → no movement
    EXPECT_EQ(p.currentIndex(), 0u);
}

TEST(AnimationPlayer, RestartRewindsAndPlays) {
    const Animation a = makeUniform();
    AnimationPlayer p{.animation = &a};
    p.advance(PlaybackMode::single(), 18);  // finished, on frame 2
    ASSERT_TRUE(p.finished());
    p.restart();
    EXPECT_EQ(p.currentIndex(), 0u);
    EXPECT_EQ(p.elapsedTicks, 0u);
    EXPECT_TRUE(p.playing);
    EXPECT_FALSE(p.finished());
}

TEST(AnimationPlayer, SeekByIndexLandsAtWindowStartAcrossUnequalDurations) {
    // 100ms (6) / 200ms (12) / 100ms (6): window starts 0, 6, 18.
    Animation a{{
        AnimationFrame{"f0", AtlasId{}, AssetSlot{0, k8}, PaletteId{}, 100ms},
        AnimationFrame{"f1", AtlasId{}, AssetSlot{1, k8}, PaletteId{}, 200ms},
        AnimationFrame{"f2", AtlasId{}, AssetSlot{2, k8}, PaletteId{}, 100ms},
    }};
    AnimationPlayer p{.animation = &a};
    p.seek(1);
    EXPECT_EQ(p.elapsedTicks, 6u);
    EXPECT_EQ(p.currentIndex(), 1u);
    p.seek(2);
    EXPECT_EQ(p.elapsedTicks, 18u);
    EXPECT_EQ(p.currentIndex(), 2u);
    // re-resolving from the seeked position keeps the seeked frame (it sits at the window start).
    p.advance(PlaybackMode::loopIndefinitely(), 1);  // tick 19 → still window 2 [18,24)
    EXPECT_EQ(p.currentIndex(), 2u);
}

TEST(AnimationPlayer, SeekByLabelAndAbsentLabelIsNoOp) {
    const Animation a = makeUniform();
    AnimationPlayer p{.animation = &a};
    p.seek("c");
    EXPECT_EQ(p.currentIndex(), 2u);
    EXPECT_EQ(p.elapsedTicks, 12u);  // window 2 start
    p.seek("nope");                  // no-op
    EXPECT_EQ(p.currentIndex(), 2u);
    EXPECT_EQ(p.elapsedTicks, 12u);
}

TEST(AnimationPlayer, DefaultProfileResolvesAtGbcCadence) {
    const Animation a = makeUniform();
    AnimationPlayer p{.animation = &a};  // no profile set → inherits defaultTiming (GBC baseline)
    p.advance(PlaybackMode::loopIndefinitely(), 6);
    EXPECT_EQ(p.currentIndex(), 1u);  // matches the 6-tick GBC frame window
}

// The settable static default: set it once and bare players inherit it — the "set the default once"
// config pattern. Save/restore so this never leaks into other tests (the default is process-wide).
TEST(AnimationPlayer, DefaultProfileIsConfigurableAndInherited) {
    const TimingProfile saved = AnimationPlayer::defaultTiming;
    AnimationPlayer::defaultTiming = TimingProfile{TickPeriodNs::Hz60};  // a non-GBC cadence

    const Animation a = makeUniform();
    AnimationPlayer p{.animation = &a};  // bare — must inherit the configured default, not GBC
    EXPECT_EQ(p.profile, AnimationPlayer::defaultTiming);
    EXPECT_EQ(p.profile.tickPeriodNs, TickPeriodNs::Hz60);

    // an explicit profile still overrides the default
    AnimationPlayer q{.animation = &a, .profile = TimingProfile::GameBoy};
    EXPECT_EQ(q.profile, TimingProfile::GameBoy);

    AnimationPlayer::defaultTiming = saved;  // restore (no ASSERT above, so this always runs)
    AnimationPlayer r{.animation = &a};
    EXPECT_EQ(r.profile, saved);
}

// ── AtlasManifest::frame shorthand — fills in atlas + slot from a cell index ─────────────────────────

TEST(AtlasManifestFrame, BuildsFrameFromSheetCell) {
    // A hand-built manifest (no device): slots carry tiles 2 and 5.
    const AtlasManifest sheet{static_cast<AtlasId>(4), {AssetSlot{2, k8}, AssetSlot{5, k8}}};

    const AnimationFrame f = sheet.frame(1, static_cast<PaletteId>(9), 120ms, "step1");
    EXPECT_EQ(f.label, "step1");
    EXPECT_EQ(f.atlas, static_cast<AtlasId>(4));        // the manifest's atlas
    EXPECT_EQ(f.slot, (AssetSlot{5, k8}));              // slots[1]
    EXPECT_EQ(f.palette, static_cast<PaletteId>(9));
    EXPECT_EQ(f.duration, std::chrono::nanoseconds(120ms));

    // label is optional — defaults to empty.
    EXPECT_TRUE(sheet.frame(0, PaletteId{}, 100ms).label.empty());
    EXPECT_EQ(sheet.frame(0, PaletteId{}, 100ms).slot, (AssetSlot{2, k8}));
}
