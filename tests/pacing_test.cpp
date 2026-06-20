#include <gtest/gtest.h>

#include <chrono>

#include "retropp/pacing.h"

namespace retropp {
namespace {

using ns = std::chrono::nanoseconds;
constexpr ns P{10'000'000};  // a clean 10 ms "refresh period" for arithmetic

// Constexpr-usable (pure, literal FrameDeadline) — locks the "no clock, no sleep" property at compile time.
static_assert(nextFrameDeadline(P, P, P).sleepFor == ns::zero());
static_assert(nextFrameDeadline(P, P, P).nextDeadline == 2 * P);

// Early: the frame finished before its deadline → sleep out exactly the remainder; deadline accumulates.
TEST(Pacing, EarlySleepsTheRemainder) {
    const FrameDeadline fd = nextFrameDeadline(/*prevDeadline=*/P, /*period=*/P, /*now=*/ns{4'000'000});
    EXPECT_EQ(fd.sleepFor, ns{6'000'000});  // 10 ms deadline − 4 ms now
    EXPECT_EQ(fd.nextDeadline, 2 * P);
}

// Exactly on time → no sleep, accumulate.
TEST(Pacing, OnTimeDoesNotSleep) {
    const FrameDeadline fd = nextFrameDeadline(P, P, /*now=*/P);
    EXPECT_EQ(fd.sleepFor, ns::zero());
    EXPECT_EQ(fd.nextDeadline, 2 * P);
}

// Slightly late (within the lag tolerance) → no sleep (never negative), still accumulate by one period.
TEST(Pacing, SlightlyLateDoesNotSleepAndAccumulates) {
    const FrameDeadline fd = nextFrameDeadline(P, P, /*now=*/P + ns{2'000'000});
    EXPECT_EQ(fd.sleepFor, ns::zero());
    EXPECT_EQ(fd.nextDeadline, 2 * P);  // accumulate, do NOT resync — we are only 2 ms behind
}

// Exactly maxLag periods behind is the inclusive boundary → still accumulate, NOT resync.
TEST(Pacing, MaxLagBoundaryAccumulates) {
    // behind = now - prevDeadline = 4·P, and maxLagPeriods defaults to 4 → 4P is not > 4P.
    const FrameDeadline fd = nextFrameDeadline(/*prevDeadline=*/P, P, /*now=*/P + 4 * P);
    EXPECT_EQ(fd.sleepFor, ns::zero());
    EXPECT_EQ(fd.nextDeadline, 2 * P);  // accumulate from prevDeadline, no snap
}

// More than maxLag periods behind (a stall) → resync to now + period so no sleep debt fast-forwards later.
TEST(Pacing, FarBehindResyncsToNow) {
    const ns now = P + 5 * P;  // 5 periods behind the deadline
    const FrameDeadline fd = nextFrameDeadline(/*prevDeadline=*/P, P, now);
    EXPECT_EQ(fd.sleepFor, ns::zero());
    EXPECT_EQ(fd.nextDeadline, now + P);  // snap to now, drop the backlog
}

// A degenerate (non-positive) period — a failed refresh query — must not sleep or divide by it: free-run.
TEST(Pacing, NonPositivePeriodFreeRuns) {
    const FrameDeadline zero = nextFrameDeadline(P, ns::zero(), /*now=*/ns{3'000'000});
    EXPECT_EQ(zero.sleepFor, ns::zero());
    EXPECT_EQ(zero.nextDeadline, ns{3'000'000});  // nextDeadline = now

    const FrameDeadline neg = nextFrameDeadline(P, ns{-1}, /*now=*/ns{3'000'000});
    EXPECT_EQ(neg.sleepFor, ns::zero());
    EXPECT_EQ(neg.nextDeadline, ns{3'000'000});
}

}  // namespace
}  // namespace retropp
