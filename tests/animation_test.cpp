// Animations: the pure playback resolver, programmatic frame access, duration/tick handling, the
// game-owned AnimationPlayer, and frame art resolution through the sheet. Two layers of coverage:
//   * Headless — the resolver is a pure function of (frames, elapsed ticks, TimingProfile,
//     PlaybackMode) and a frame's sheet/tileIndex/palette are plain values, so everything up to art
//     resolution runs with no window or GPU (hand-built AtlasManifests supply sheet ids).
//   * Device-backed — tile()/size() resolve a slot arithmetically from the slice geometry the engine
//     renderer records at loadAtlas (Renderer::atlasSlot via Renderer::instance()), so those reads are
//     exercised on a real GPU device (a software rasterizer in CI) against a loadAtlas'd sheet.
// Tick numbers are pinned against the GameBoyColor cadence: ticksForDuration(100ms) == 6 and a 3×100ms
// animation totals 18 ticks (the bridge is asserted exactly so a cadence regression is loud).

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/animation.h"
#include "retropp/atlas_manifest.h"  // AtlasManifest — a frame's sheet (hand-built here, no device)
#include "retropp/renderer.h"        // Renderer — the device-backed tile()/size() resolution below
#include "retropp/timing.h"

using namespace retropp;
using namespace std::chrono_literals;

namespace {

constexpr AssetDimensions k8  = AssetDimensions::GameBoy8x8;
const TimingProfile        gbc = TimingProfile::GameBoyColor;

// Device-free sheets: hand-built manifests whose ids the frames keep (`.sheet = kSheet.atlasId` — the
// explicit projection; a frame retains nothing and points at nothing).
const AtlasManifest kSheet = [] {
    AtlasManifest m{.atlasId = static_cast<AtlasId>(1), .kind = ContentKind::SingleAnimation};
    for (std::uint16_t i = 0; i < 10; ++i) m.slots.push_back(AssetSlot{i, k8});
    return m;
}();
const AtlasManifest kSheet2{.atlasId = static_cast<AtlasId>(2),
                            .slots   = {AssetSlot{0, k8}, AssetSlot{1, k8}},
                            .kind    = ContentKind::SingleAnimation};

// 3 frames, 100ms each (6 ticks each → 18 total), distinct slots/palettes/labels.
Animation makeUniform() {
    return Animation{{
        {.label = "a", .sheet = kSheet.atlasId, .tileIndex = 0, .palette = static_cast<PaletteId>(10), .duration = 100ms},
        {.label = "b", .sheet = kSheet.atlasId, .tileIndex = 1, .palette = static_cast<PaletteId>(11), .duration = 100ms},
        {.label = "c", .sheet = kSheet.atlasId, .tileIndex = 2, .palette = static_cast<PaletteId>(12), .duration = 100ms},
    }};
}

std::size_t idx(const Animation& a, std::uint64_t t, PlaybackMode m) {
    return sampleAnimation(a, t, gbc, m).frameIndex;
}
bool fin(const Animation& a, std::uint64_t t, PlaybackMode m) {
    return sampleAnimation(a, t, gbc, m).finished;
}

}  // namespace

// ── The pinned bridge: 100ms == 6 ticks, total == 18 ────────────────────────────────────────────────

TEST(Animation, FrameTicksAndTotalArePinnedToGbcCadence) {
    EXPECT_EQ(gbc.ticksForDuration(100ms), 6u);
    EXPECT_EQ(totalTicks(makeUniform(), gbc), 18u);
}

// ── sampleAnimation — LoopIndefinitely ──────────────────────────────────────────────────────────────────

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

// ── sampleAnimation — Single ──────────────────────────────────────────────────────────────────────────

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

// ── sampleAnimation — LoopNTimes ────────────────────────────────────────────────────────────────────────

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

// ── sampleAnimation — PlayForDuration ─────────────────────────────────────────────────────────────────

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
    Animation lit{{{.sheet = kSheet.atlasId, .tileIndex = 0, .duration = 100ms}}};
    Animation exp{{{.sheet = kSheet.atlasId, .tileIndex = 0, .duration = std::chrono::milliseconds(100)}}};
    EXPECT_EQ(totalTicks(lit, gbc), totalTicks(exp, gbc));
    EXPECT_EQ(totalTicks(lit, gbc), 6u);
}

TEST(AnimationDuration, SecondsResolveAndMixedUnitsResolve) {
    EXPECT_EQ(gbc.ticksForDuration(2s), 119u);  // 2s / 16.742706ms ≈ 119.46 → 119
    Animation mixed{{
        {.sheet = kSheet.atlasId, .tileIndex = 0, .duration = 100ms},  // 6
        {.sheet = kSheet.atlasId, .tileIndex = 1, .duration = 2s},     // 119
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
        {.label = "x", .sheet = kSheet.atlasId, .tileIndex = 0, .duration = 100ms},   // 6
        {.label = "gone", .sheet = kSheet.atlasId, .tileIndex = 1, .duration = 8ms},  // 0 — skipped
        {.label = "y", .sheet = kSheet.atlasId, .tileIndex = 2, .duration = 100ms},   // 6
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
// (tile()/size() — the slot → cell/size resolution — are device-backed below: they resolve through the
// engine renderer's recorded slice geometry, so they need a loadAtlas'd sheet.)

TEST(AnimationArt, AtlasAndHasArtAreDirectReads) {
    const AtlasManifest sheet{.atlasId = static_cast<AtlasId>(4),
                              .slots   = {AssetSlot{7, AssetDimensions::GameBoy8x16}, AssetSlot{3, k8}},
                              .kind    = ContentKind::SpriteSeries};
    const AnimationFrame f0{.sheet = sheet.atlasId, .tileIndex = 0, .palette = static_cast<PaletteId>(9), .duration = 120ms};
    EXPECT_TRUE(f0.hasArt());
    EXPECT_EQ(f0.atlas(), static_cast<AtlasId>(4));  // the sheet's id — the manifest converted, nothing retained
    EXPECT_EQ(f0.sheet, static_cast<AtlasId>(4));
    EXPECT_EQ(*f0.tileIndex, 0u);
}

TEST(AnimationArt, PaletteOnlyFrameCarriesNoArt) {
    const AnimationFrame art{.sheet = kSheet.atlasId, .tileIndex = 2, .palette = static_cast<PaletteId>(5), .duration = 100ms};
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
    EXPECT_EQ(*a[2].tileIndex, 2u);  // frame 2 names kSheet's slot 2
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
        {.label = "dup", .sheet = kSheet.atlasId, .tileIndex = 0, .duration = 100ms},
        {.label = "dup", .sheet = kSheet.atlasId, .tileIndex = 9, .duration = 100ms},
    }};
    ASSERT_TRUE(a.indexOf("dup").has_value());
    EXPECT_EQ(*a.indexOf("dup"), 0u);        // first match
    EXPECT_EQ(*a.find("dup")->tileIndex, 0u);
}

// ── Multi-sheet frames (frames compose across different sheets) ──────────────────────────────────────

TEST(AnimationMultiSheet, FramesNameDistinctSheets) {
    Animation a{{
        {.label = "fromSheet",  .sheet = kSheet.atlasId,  .tileIndex = 3, .duration = 100ms},
        {.label = "fromOneOff", .sheet = kSheet2.atlasId, .tileIndex = 0, .duration = 100ms},
    }};
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(sampleAnimationFrame(a, 0, gbc, loop).atlas(), static_cast<AtlasId>(1));
    EXPECT_EQ(sampleAnimationFrame(a, 6, gbc, loop).atlas(), static_cast<AtlasId>(2));
}

// ── Palette-cycling (the same unit, art constant) ──────────────────────────────────────────────────

TEST(AnimationPaletteCycle, SameSlotDifferentPalettePerWindow) {
    Animation a{{
        {.sheet = kSheet.atlasId, .tileIndex = 0, .palette = static_cast<PaletteId>(10), .duration = 100ms},
        {.sheet = kSheet.atlasId, .tileIndex = 0, .palette = static_cast<PaletteId>(11), .duration = 100ms},
        {.sheet = kSheet.atlasId, .tileIndex = 0, .palette = static_cast<PaletteId>(12), .duration = 100ms},
    }};
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(*sampleAnimationFrame(a, 0,  gbc, loop).tileIndex, 0u);  // art constant
    EXPECT_EQ(sampleAnimationFrame(a, 0,  gbc, loop).palette, static_cast<PaletteId>(10));
    EXPECT_EQ(sampleAnimationFrame(a, 6,  gbc, loop).palette, static_cast<PaletteId>(11));
    EXPECT_EQ(sampleAnimationFrame(a, 12, gbc, loop).palette, static_cast<PaletteId>(12));
}

// ── Degenerate guards ─────────────────────────────────────────────────────────────────────────────

TEST(AnimationDegenerate, EmptyAnimation) {
    Animation empty{};
    EXPECT_EQ(empty.count(), 0u);
    EXPECT_EQ(totalTicks(empty, gbc), 0u);
    const PlaybackState s = sampleAnimation(empty, 0, gbc, PlaybackMode::loopIndefinitely());
    EXPECT_EQ(s.frameIndex, 0u);
    EXPECT_TRUE(s.finished);
}

TEST(AnimationDegenerate, SingleFrameLoopStaysOnFrameZero) {
    Animation a{{{.label = "only", .sheet = kSheet.atlasId, .tileIndex = 0, .duration = 100ms}}};
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
        {.label = "f0", .sheet = kSheet.atlasId, .tileIndex = 0, .duration = 100ms},
        {.label = "f1", .sheet = kSheet.atlasId, .tileIndex = 1, .duration = 200ms},
        {.label = "f2", .sheet = kSheet.atlasId, .tileIndex = 2, .duration = 100ms},
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

// ── Device-backed: tile()/size() resolve through the engine renderer's slice geometry ───────────────

namespace {

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << "could not open fixture: " << path;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string fixture(const char* name) { return std::string{RETROPP_FIXTURES_DIR} + "/" + name; }

// Windows on ARM is a courtesy runner with no production-representative GPU backend in CI; its production
// path (D3D12 + DXIL) is covered by the Windows x64 job, so a missing device there is an out-of-scope skip.
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class AnimationArtDevice : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    initError_;

    static void SetUpTestSuite() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            initError_ = std::string("SDL_Init(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
            return;
        }
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
            /*debug_mode=*/false, /*name=*/nullptr);
        if (!device_) initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
    }

    static void TearDownTestSuite() {
        if (device_) {
            SDL_DestroyGPUDevice(device_);
            device_ = nullptr;
        }
        SDL_Quit();
    }

    void SetUp() override {
        if (!device_) {
            if (kDeviceOptional) {
                GTEST_SKIP() << "Windows on ARM is a courtesy runner with no production-representative GPU "
                                "backend in CI; the frame fields and resolver are covered device-free above. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". An AnimationFrame's tile()/size() resolve through the engine renderer's recorded "
                      "slice geometry and so need a GPU device on every production-representative platform "
                      "(a software rasterizer suffices; on a headless runner set SDL_VIDEODRIVER=offscreen).";
        }
    }
};

// sheet16x32.png (gen_fixtures.py): a 2×2 grid of 8×16 assets. Sliced as a SpriteSeries its carve is
// cells {0, 1, 4, 5} — slot index ≠ atlas cell from slot 2 on, so a correct tile() must resolve
// THROUGH the sheet's geometry, never echo the index. The reads agree with the manifest slot for
// slot: the arithmetic IS the carve.
TEST_F(AnimationArtDevice, TileAndSizeResolveThroughTheRecordedSliceGeometry) {
    Renderer r{device_, nullptr};  // compose-only (no window) — the one engine renderer
    const AtlasManifest sheet = r.loadAtlasFromMemory(readFile(fixture("sheet16x32.png")),
                                                      AssetDimensions{8, 16}, ContentKind::SpriteSeries);
    ASSERT_EQ(sheet.tileCount(), 4u);
    EXPECT_EQ(sheet.kind, ContentKind::SpriteSeries);  // the load's declared kind lands on the manifest

    const AnimationFrame f{.sheet = sheet.atlasId, .tileIndex = 2, .duration = 100ms};
    EXPECT_EQ(f.atlas(), sheet.atlasId);
    EXPECT_EQ(f.tile(), 4);                          // the resolved cell, not the index (2)
    EXPECT_EQ(f.size(), (AssetDimensions{8, 16}));

    for (std::size_t i = 0; i < sheet.tileCount(); ++i) {
        const AnimationFrame fi{.sheet = sheet.atlasId, .tileIndex = i, .duration = 100ms};
        EXPECT_EQ(fi.tile(), sheet[i].tile);
        EXPECT_EQ(fi.size(), sheet[i].dimensions);
    }
}

TEST_F(AnimationArtDevice, SingleSheetResolvesTheWholeImage) {
    Renderer r{device_, nullptr};
    const AtlasManifest sheet = r.loadAtlasFromMemory(readFile(fixture("sheet16x32.png")),
                                                      AssetDimensions{8, 16}, ContentKind::Single);
    ASSERT_EQ(sheet.tileCount(), 1u);
    EXPECT_EQ(sheet.kind, ContentKind::Single);
    const AnimationFrame f{.sheet = sheet.atlasId, .tileIndex = 0, .duration = 100ms};
    EXPECT_EQ(f.tile(), 0);
    EXPECT_EQ(f.size(), (AssetDimensions{16, 32}));  // Single: slot 0 spans the image
}

// A sheet whose pixels were built in code: the upload declares the carve and records the same slice
// geometry loadAtlas would, so its frames resolve identically. The three-argument upload carves
// Single — one whole-image asset at slot 0.
TEST_F(AnimationArtDevice, UploadCarveResolvesLikeALoadedSheet) {
    Renderer r{device_, nullptr};
    std::array<std::uint8_t, 16 * 8> idx{};  // a 16×8 strip — two 8×8 cells
    const AtlasManifest sheet = r.uploadAtlas(idx.data(), 16, 8, AssetDimensions::GameBoy8x8,
                                              ContentKind::SpriteSeries);
    ASSERT_EQ(sheet.tileCount(), 2u);
    EXPECT_EQ(sheet.kind, ContentKind::SpriteSeries);
    const AnimationFrame f{.sheet = sheet.atlasId, .tileIndex = 1, .duration = 100ms};
    EXPECT_EQ(f.tile(), 1);
    EXPECT_EQ(f.size(), AssetDimensions::GameBoy8x8);

    const AtlasManifest bare = r.uploadAtlas(idx.data(), 16, 8);  // Single: one whole-image asset
    ASSERT_EQ(bare.tileCount(), 1u);
    EXPECT_EQ(bare.kind, ContentKind::Single);  // the bare 3-arg upload declares Single by contract
    const AnimationFrame whole{.sheet = bare.atlasId, .tileIndex = 0, .duration = 100ms};
    EXPECT_EQ(whole.tile(), 0);
    EXPECT_EQ(whole.size(), (AssetDimensions{16, 8}));
}

// A slot index at/past the carved count resolves to AssetSlot{} (logged) — graceful, never UB.
TEST_F(AnimationArtDevice, OutOfRangeSlotResolvesEmpty) {
    Renderer r{device_, nullptr};
    const AtlasManifest sheet = r.loadAtlasFromMemory(readFile(fixture("sheet16x32.png")),
                                                      AssetDimensions{8, 16}, ContentKind::SpriteSeries);
    const AnimationFrame f{.sheet = sheet.atlasId, .tileIndex = 99, .duration = 100ms};
    EXPECT_EQ(f.tile(), 0);
    EXPECT_EQ(f.size(), AssetDimensions{});  // AssetSlot{}'s default dimensions — not slot data
}

}  // namespace
