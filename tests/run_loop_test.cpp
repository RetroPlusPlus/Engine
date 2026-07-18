#include <gtest/gtest.h>

#include <chrono>

#include "retropp/input.h"
#include "retropp/run_loop.h"
#include "retropp/timing.h"
#include "manual_clock.h"

using retropp::InputSample;
using retropp::InputState;
using retropp::actionId;
using retropp::RunLoop;
using retropp::TickPeriodNs;
using retropp::TimingProfile;
using retropp::kMaxFrameTime;
using retropp::test::ManualClock;

namespace {

// The default-profile tick period (GBC, 16'742'706 ns) — the cadence these cases assert on,
// read from the profile. The default RunLoop uses this.
constexpr auto kTickPeriod = TimingProfile::GameBoyColor.tickPeriod();

// Common fixture: a manual clock + loop + counters the tests assert against.
struct LoopHarness {
    ManualClock clock;
    RunLoop loop{clock};
    int ticks = 0;
    int renders = 0;
    float lastAlpha = -1.0f;

    LoopHarness() {
        loop.simTick([this](const InputState&) { ++ticks; });
        loop.renderLoop([this](float a) { ++renders; lastAlpha = a; });
    }

    // Settle the baseline (first advance runs zero ticks) so subsequent advances
    // measure elapsed from a known zero.
    void settle() { loop.advance(); }
};

}  // namespace

TEST(RunLoop, FirstAdvanceEstablishesBaselineAndRunsZeroTicks) {
    LoopHarness h;
    h.clock.set(std::chrono::nanoseconds{123'456});  // arbitrary nonzero baseline
    h.loop.advance();
    EXPECT_EQ(h.ticks, 0);
    EXPECT_EQ(h.loop.tickCount(), 0u);
    EXPECT_EQ(h.renders, 1);  // render still fires (alpha 0) on the baseline frame
    EXPECT_FLOAT_EQ(h.lastAlpha, 0.0f);
}

TEST(RunLoop, ExactMultipleOfPeriodRunsExactlyThatManyTicks) {
    LoopHarness h;
    h.settle();
    h.clock.advanceBy(kTickPeriod * 5);
    h.loop.advance();
    EXPECT_EQ(h.ticks, 5);
    EXPECT_EQ(h.loop.tickCount(), 5u);
    EXPECT_FLOAT_EQ(h.lastAlpha, 0.0f);  // landed exactly on a tick boundary
}

TEST(RunLoop, SteadyStateRunsOneTickPerOneFrameAdvance) {
    // One GB-frame of elapsed per advance runs exactly one tick per advance — the
    // steady-state path a real host drives. Count is rate-agnostic (100 frames → 100
    // ticks); a single multi-frame advance would instead hit the per-advance
    // spiral-of-death clamp, which is per-advance by design.
    LoopHarness h;
    h.settle();
    constexpr int kFrames = 100;
    for (int i = 0; i < kFrames; ++i) {
        h.clock.advanceBy(kTickPeriod);  // one real GB frame (59.7275 Hz), not 1/60 s
        h.loop.advance();
    }
    EXPECT_EQ(h.ticks, kFrames);
    EXPECT_EQ(h.loop.tickCount(), static_cast<std::uint64_t>(kFrames));
}

TEST(RunLoop, AlphaStaysInUnitIntervalAcrossSubPeriodAdvances) {
    LoopHarness h;
    h.settle();
    // Advance by 1.5 periods: one tick fires, half a period of residual → alpha ≈ 0.5.
    h.clock.advanceBy(kTickPeriod + kTickPeriod / 2);
    h.loop.advance();
    EXPECT_EQ(h.ticks, 1);
    EXPECT_GE(h.lastAlpha, 0.0f);
    EXPECT_LT(h.lastAlpha, 1.0f);
    EXPECT_NEAR(h.lastAlpha, 0.5f, 0.01f);
}

TEST(RunLoop, AccumulatesTickCountAcrossAdvances) {
    LoopHarness h;
    h.settle();
    h.clock.advanceBy(kTickPeriod * 3);
    h.loop.advance();
    h.clock.advanceBy(kTickPeriod * 4);
    h.loop.advance();
    EXPECT_EQ(h.loop.tickCount(), 7u);
}

TEST(RunLoop, SpiralOfDeathClampCapsTicksForOverlongFrame) {
    LoopHarness h;
    h.settle();
    h.clock.advanceBy(std::chrono::seconds{10});  // far over the clamp
    h.loop.advance();
    // Capped at ⌊kMaxFrameTime / kTickPeriod⌋ — derived, not hardcoded.
    const auto cap = static_cast<int>(kMaxFrameTime / kTickPeriod);
    EXPECT_EQ(h.ticks, cap);
}

TEST(RunLoop, BackwardsClockRunsZeroTicksAndDoesNotUnderflow) {
    LoopHarness h;
    h.clock.set(std::chrono::seconds{5});
    h.settle();
    h.clock.set(std::chrono::seconds{1});  // time went backwards
    h.loop.advance();
    EXPECT_EQ(h.ticks, 0);
    EXPECT_FLOAT_EQ(h.lastAlpha, 0.0f);  // accumulator unchanged, still on a boundary
}

TEST(RunLoop, RenderFiresExactlyOncePerAdvance) {
    LoopHarness h;
    h.settle();  // render #1
    h.clock.advanceBy(kTickPeriod * 3);
    h.loop.advance();  // render #2 — one render despite three ticks
    EXPECT_EQ(h.renders, 2);
}

TEST(RunLoop, CatchUpBatchSharesOneRawSampleSoEdgeFiresOnFirstTickOnly) {
    enum class Act : std::uint8_t { Fire };
    LoopHarness h;
    int pressedCount = 0;
    h.loop.simTick([&](const InputState& in) {
        if (in.justPressed(Act::Fire)) ++pressedCount;
    });
    h.settle();
    InputSample down;
    down.players[0].held.set(actionId(Act::Fire), true);
    h.loop.setRawInput(down);
    h.clock.advanceBy(kTickPeriod * 4);  // 4 ticks in one advance, same raw the whole batch
    h.loop.advance();
    EXPECT_EQ(pressedCount, 1);  // pressed edge only on the first tick of the batch
}

TEST(RunLoop, RunStopsWhenCallbackRequestsStop) {
    LoopHarness h;
    int iterations = 0;
    // Stop from inside the render callback after a few iterations; run() must return.
    h.loop.renderLoop([&](float) {
        if (++iterations >= 3) h.loop.stop();
    });
    h.loop.run();
    EXPECT_GE(iterations, 3);
}

TEST(RunLoop, NonDefaultProfileTicksOnItsOwnPeriod) {
    // A loop built with a non-default profile schedules on THAT period, proving the profile's
    // tick period actually threads through advance() (not the GBC default).
    ManualClock clock;
    RunLoop loop{clock, TimingProfile{TickPeriodNs::Hz60}};
    EXPECT_EQ(loop.tickPeriod(), std::chrono::nanoseconds{16'666'667});

    int ticks = 0;
    loop.simTick([&](const InputState&) { ++ticks; });
    loop.advance();                              // settle baseline
    clock.advanceBy(loop.tickPeriod() * 3);      // exactly three Hz60 periods
    loop.advance();
    EXPECT_EQ(ticks, 3);
    EXPECT_EQ(loop.tickCount(), 3u);
}

TEST(RunLoop, DefaultProfileIsGameBoyColorCadence) {
    ManualClock clock;
    RunLoop loop{clock};
    EXPECT_EQ(loop.tickPeriod(), TimingProfile::GameBoyColor.tickPeriod());
    EXPECT_EQ(loop.timing().tickPeriodNs, TickPeriodNs::GameBoyColor);
}
