#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "retropp/pacing.h"

namespace retropp {
namespace {

using ns = std::chrono::nanoseconds;
constexpr ns P{10'000'000};  // a clean 10 ms "refresh period" for arithmetic

// One iteration of the host loop's pacing: the frame's work advances the clock, the deadline decides
// the sleep, and the sleep advances the clock again. `work` is one entry per iteration; the returned
// rows carry what the loop would have observed each iteration.
struct PacedFrame {
    ns now;        // the clock when the deadline was queried — the capture's per-frame sample point
    ns dt;         // interval since the previous sample: the preceding sleep plus this frame's work
    ns sleepFor;   // what the deadline asked the loop to sleep
};

std::vector<PacedFrame> runPacing(const std::vector<ns>& work, ns period) {
    std::vector<PacedFrame> frames;
    frames.reserve(work.size());
    ns clock = ns::zero();
    ns deadline = clock;  // WindowedHost anchors the first deadline to "now" before entering the loop
    ns prevNow = clock;
    for (const ns w : work) {
        clock += w;
        const FrameDeadline fd = nextFrameDeadline(deadline, period, clock);
        frames.push_back({.now = clock, .dt = clock - prevNow, .sleepFor = fd.sleepFor});
        prevNow = clock;
        deadline = fd.nextDeadline;
        clock += fd.sleepFor;
    }
    return frames;
}

// Constexpr-usable (pure, literal FrameDeadline) — locks the "no clock, no sleep" property at compile time.
static_assert(nextFrameDeadline(P, P, P).sleepFor == ns::zero());
static_assert(nextFrameDeadline(P, P, P).nextDeadline == 2 * P);

// Early: the frame finished before its deadline → sleep out exactly the remainder; deadline accumulates.
TEST(Pacing, EarlySleepsTheRemainder) {
    const FrameDeadline fd = nextFrameDeadline(/*prevDeadline=*/P, /*period=*/P, /*now=*/ns{4'000'000});
    EXPECT_EQ(fd.sleepFor, ns{6'000'000});  // 10 ms deadline − 4 ms now
    EXPECT_EQ(fd.nextDeadline, 2 * P);
}

// Exactly on time → no sleep, accumulate from the deadline.
TEST(Pacing, OnTimeDoesNotSleep) {
    const FrameDeadline fd = nextFrameDeadline(P, P, /*now=*/P);
    EXPECT_EQ(fd.sleepFor, ns::zero());
    EXPECT_EQ(fd.nextDeadline, 2 * P);
}

// Slightly late → no sleep (never negative), and the deadline re-anchors to now rather than carrying
// the 2 ms forward: the next frame gets a full period, not a shortened one.
TEST(Pacing, SlightlyLateDoesNotSleepAndReAnchors) {
    const ns now = P + ns{2'000'000};
    const FrameDeadline fd = nextFrameDeadline(P, P, now);
    EXPECT_EQ(fd.sleepFor, ns::zero());
    EXPECT_EQ(fd.nextDeadline, now + P);
}

// Lateness is forgiven at every magnitude — one nanosecond behind and three periods behind re-anchor
// identically. There is no window inside which lateness accumulates.
TEST(Pacing, LateReAnchorsByAnyAmount) {
    const ns barelyLate = P + ns{1};
    const FrameDeadline barely = nextFrameDeadline(/*prevDeadline=*/P, P, barelyLate);
    EXPECT_EQ(barely.sleepFor, ns::zero());
    EXPECT_EQ(barely.nextDeadline, barelyLate + P);

    const ns deeplyLate = P + 3 * P;
    const FrameDeadline deeply = nextFrameDeadline(/*prevDeadline=*/P, P, deeplyLate);
    EXPECT_EQ(deeply.sleepFor, ns::zero());
    EXPECT_EQ(deeply.nextDeadline, deeplyLate + P);
}

// Far behind (a stall) → no sleep, and the deadline is one period from now: the backlog is dropped so
// recovery never fast-forwards.
TEST(Pacing, FarBehindResyncsToNow) {
    const ns now = P + 5 * P;  // 5 periods behind the deadline
    const FrameDeadline fd = nextFrameDeadline(/*prevDeadline=*/P, P, now);
    EXPECT_EQ(fd.sleepFor, ns::zero());
    EXPECT_EQ(fd.nextDeadline, now + P);
}

// The contract's central invariant, driven over a mixed sequence of short and long frames: a frame is
// asked to sleep nothing only when the interval it just closed was itself at least a full period, so a
// short frame is always paced. The first iteration is exempt — the loop anchors its deadline to "now"
// before running, so the anchor frame closes no interval.
TEST(Pacing, ZeroSleepImpliesFullPeriodFrame) {
    const std::vector<ns> work{
        ns{2'000'000},   // short
        ns{3'000'000},   // short
        ns{12'000'000},  // over a period
        ns{11'000'000},  // over a period
        ns{10'000'000},  // exactly a period
        ns{1'000'000},   // short again — the flip
        ns{500'000},     // short
        ns{4'000'000},   // short
        ns{25'000'000},  // a stall
        ns{1'000'000},   // short
        ns{1'000'000},   // short
    };
    const std::vector<PacedFrame> frames = runPacing(work, P);

    int zeroSleeps = 0;
    int positiveSleeps = 0;
    for (std::size_t i = 1; i < frames.size(); ++i) {
        SCOPED_TRACE(testing::Message() << "iteration " << i << ": dt=" << frames[i].dt.count()
                                        << "ns sleepFor=" << frames[i].sleepFor.count() << "ns");
        if (frames[i].sleepFor == ns::zero()) {
            ++zeroSleeps;
            EXPECT_GE(frames[i].dt, P);
        } else {
            ++positiveSleeps;
        }
    }
    // Both branches must occur, or the invariant above is vacuous.
    EXPECT_GT(zeroSleeps, 0);
    EXPECT_GT(positiveSleeps, 0);
}

// A render callback that blocks for about a refresh period per frame, then stops blocking: the frames
// after the flip sleep out their whole remainder rather than running unpaced. The sequence is a long
// frame entering the blocking regime, several period-length blocked frames, then short frames once the
// block lifts — the shape a GPU-backpressure stretch has when it ends.
TEST(Pacing, RegimeFlipDoesNotBurst) {
    constexpr ns kRegimeEntry{18'000'000};        // the frame that enters the blocking regime overruns
    constexpr ns kBlockedFrame = P + ns{6'200};   // one period plus a small per-frame clock drift
    constexpr ns kShortFrame{500'000};            // the block has lifted
    constexpr std::size_t kBlockedCount = 6;
    constexpr std::size_t kShortCount = 5;

    std::vector<ns> work{kRegimeEntry};
    work.insert(work.end(), kBlockedCount, kBlockedFrame);
    work.insert(work.end(), kShortCount, kShortFrame);
    const std::vector<PacedFrame> frames = runPacing(work, P);

    const std::size_t flip = 1 + kBlockedCount;  // the first short frame

    // The frame right after the flip sleeps its full remainder — the burst is one frame long at most,
    // and that frame is paced.
    EXPECT_EQ(frames[flip].sleepFor, P - kShortFrame);

    // Once frames run short, no two consecutive iterations run unpaced.
    for (std::size_t i = flip; i < frames.size(); ++i) {
        SCOPED_TRACE(testing::Message() << "iteration " << i << ": dt=" << frames[i].dt.count()
                                        << "ns sleepFor=" << frames[i].sleepFor.count() << "ns");
        EXPECT_GT(frames[i].sleepFor, ns::zero());
    }
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
