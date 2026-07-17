// Animations: the pure playback resolver, programmatic frame access, duration/tick handling, the
// game-owned AnimationPlayer, and frame art resolution through its sheet. Device-free — Animation is
// plain data and the resolver is a pure function of (frames, elapsed ticks, TimingProfile, PlaybackMode);
// the sheets are hand-built AtlasManifests, so no window or GPU. Tick numbers are pinned against the
// GameBoyColor cadence: ticksForDuration(100ms) == 6 and a 3×100ms animation totals 18 ticks (the bridge
// is asserted exactly so a cadence regression is loud).

#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include "retropp/animation.h"
#include "retropp/atlas_manifest.h"  // AtlasManifest — a frame's sheet (hand-built here, no device)
#include "retropp/timing.h"

using namespace retropp;
using namespace std::chrono_literals;

namespace {

constexpr AssetDimensions k8  = AssetDimensions::GameBoy8x8;
const TimingProfile        gbc = TimingProfile::GameBoyColor;

// A device-free sheet: a manifest whose slot i carries atlas cell i (so tileIndex i resolves to cell i,
// size k8). Ten slots — enough for every test below. Static storage keeps its address valid for every
// Animation whose frames name it (the span-style lifetime a frame's SheetRef requires).
const AtlasManifest kSheet = [] {
    AtlasManifest m{static_cast<AtlasId>(1), {}};
    for (std::uint16_t i = 0; i < 10; ++i) m.slots.push_back(AssetSlot{i, k8});
    return m;
}();
const AtlasManifest kSheet2{static_cast<AtlasId>(2), {AssetSlot{0, k8}, AssetSlot{1, k8}}};

// 3 frames, 100ms each (6 ticks each → 18 total), distinct slots/palettes/labels.
Animation makeUniform() {
    return Animation{{
        {.label = "a", .sheet = kSheet, .tileIndex = 0, .palette = static_cast<PaletteId>(10), .duration = 100ms},
        {.label = "b", .sheet = kSheet, .tileIndex = 1, .palette = static_cast<PaletteId>(11), .duration = 100ms},
        {.label = "c", .sheet = kSheet, .tileIndex = 2, .palette = static_cast<PaletteId>(12), .duration = 100ms},
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
    Animation lit{{{.sheet = kSheet, .tileIndex = 0, .duration = 100ms}}};
    Animation exp{{{.sheet = kSheet, .tileIndex = 0, .duration = std::chrono::milliseconds(100)}}};
    EXPECT_EQ(totalTicks(lit, gbc), totalTicks(exp, gbc));
    EXPECT_EQ(totalTicks(lit, gbc), 6u);
}

TEST(AnimationDuration, SecondsResolveAndMixedUnitsResolve) {
    EXPECT_EQ(gbc.ticksForDuration(2s), 119u);  // 2s / 16.742706ms ≈ 119.46 → 119
    Animation mixed{{
        {.sheet = kSheet, .tileIndex = 0, .duration = 100ms},  // 6
        {.sheet = kSheet, .tileIndex = 1, .duration = 2s},     // 119
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
        {.label = "x", .sheet = kSheet, .tileIndex = 0, .duration = 100ms},   // 6
        {.label = "gone", .sheet = kSheet, .tileIndex = 1, .duration = 8ms},  // 0 — skipped
        {.label = "y", .sheet = kSheet, .tileIndex = 2, .duration = 100ms},   // 6
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

// ── Frame art resolution through the sheet ──────────────────────────────────────────────────────────

TEST(AnimationArt, ResolvesAtlasCellAndSizeThroughTheSheet) {
    // slot 0 → atlas cell 7 (8×16), slot 1 → cell 3 (8×8). Index != cell, so a correct read must go
    // THROUGH the sheet — tile() returns the resolved cell, not the tileIndex.
    const AtlasManifest sheet{static_cast<AtlasId>(4),
                              {AssetSlot{7, AssetDimensions::GameBoy8x16}, AssetSlot{3, k8}}};
    const AnimationFrame f0{.sheet = sheet, .tileIndex = 0, .palette = static_cast<PaletteId>(9), .duration = 120ms};
    const AnimationFrame f1{.sheet = sheet, .tileIndex = 1, .duration = 100ms};
    EXPECT_TRUE(f0.hasArt());
    EXPECT_EQ(f0.atlas(), static_cast<AtlasId>(4));           // the sheet's atlas
    EXPECT_EQ(f0.tile(), 7);                                  // the resolved cell, not the index (0)
    EXPECT_EQ(f0.size(), AssetDimensions::GameBoy8x16);
    EXPECT_EQ(f1.tile(), 3);
    EXPECT_EQ(f1.size(), k8);
}

TEST(AnimationArt, PaletteOnlyFrameCarriesNoArt) {
    const AnimationFrame art{.sheet = kSheet, .tileIndex = 2, .palette = static_cast<PaletteId>(5), .duration = 100ms};
    const AnimationFrame paletteOnly{.palette = static_cast<PaletteId>(6), .duration = 100ms};  // no .tileIndex
    EXPECT_TRUE(art.hasArt());
    EXPECT_FALSE(paletteOnly.hasArt());                       // omitted tileIndex → palette-only
    EXPECT_EQ(paletteOnly.palette, static_cast<PaletteId>(6));  // the palette is still carried
    // A palette-only FIRST frame is legal: it carries no art, so a consumer keeps whatever art the sprite
    // already holds. The resolver treats it like any other frame (it reads duration, never art).
    Animation a{{paletteOnly, art}};
    EXPECT_FALSE(a[0].hasArt());
    EXPECT_TRUE(a[1].hasArt());
    EXPECT_EQ(totalTicks(a, gbc), 12u);
}

// ── Programmatic access ───────────────────────────────────────────────────────────────────────────

TEST(AnimationAccess, IndexAndCount) {
    const Animation a = makeUniform();
    EXPECT_EQ(a.count(), 3u);
    EXPECT_EQ(a[0].label, "a");
    EXPECT_EQ(a[2].tile(), 2);  // frame 2 → kSheet[2] → cell 2
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
        {.label = "dup", .sheet = kSheet, .tileIndex = 0, .duration = 100ms},
        {.label = "dup", .sheet = kSheet, .tileIndex = 9, .duration = 100ms},
    }};
    ASSERT_TRUE(a.indexOf("dup").has_value());
    EXPECT_EQ(*a.indexOf("dup"), 0u);        // first match
    EXPECT_EQ(a.find("dup")->tile(), 0);
}

// ── Multi-sheet frames (frames compose across different sheets) ──────────────────────────────────────

TEST(AnimationMultiSheet, FramesNameDistinctSheets) {
    Animation a{{
        {.label = "fromSheet",  .sheet = kSheet,  .tileIndex = 3, .duration = 100ms},
        {.label = "fromOneOff", .sheet = kSheet2, .tileIndex = 0, .duration = 100ms},
    }};
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(frameAt(a, 0, gbc, loop).atlas(), static_cast<AtlasId>(1));
    EXPECT_EQ(frameAt(a, 6, gbc, loop).atlas(), static_cast<AtlasId>(2));
}

// ── Palette-cycling (the same unit, art constant) ──────────────────────────────────────────────────

TEST(AnimationPaletteCycle, SameSlotDifferentPalettePerWindow) {
    Animation a{{
        {.sheet = kSheet, .tileIndex = 0, .palette = static_cast<PaletteId>(10), .duration = 100ms},
        {.sheet = kSheet, .tileIndex = 0, .palette = static_cast<PaletteId>(11), .duration = 100ms},
        {.sheet = kSheet, .tileIndex = 0, .palette = static_cast<PaletteId>(12), .duration = 100ms},
    }};
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(frameAt(a, 0,  gbc, loop).tile(), 0);       // art constant
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
    Animation a{{{.label = "only", .sheet = kSheet, .tileIndex = 0, .duration = 100ms}}};
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
        {.label = "f0", .sheet = kSheet, .tileIndex = 0, .duration = 100ms},
        {.label = "f1", .sheet = kSheet, .tileIndex = 1, .duration = 200ms},
        {.label = "f2", .sheet = kSheet, .tileIndex = 2, .duration = 100ms},
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
