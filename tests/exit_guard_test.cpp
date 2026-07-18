#include <gtest/gtest.h>

#include <string>

#include "retropp/run_loop.h"
#include "retropp/timing.h"
#include "retropp/windowed_host.h"

#include "manual_clock.h"
#include "mock_platform.h"

// The one-shot exit-guard paths: a guard that Proceeds at the boundary (with its close-out work done),
// a guard that Vetoes (the loop keeps running, the pending state is cleared), the no-guard default
// (a pending exit Proceeds immediately), and the OS window-close routing through the SAME guard (a
// Veto clears the platform quit latch so the loop can keep running). Device-free — the guard logic
// drives RunLoop::advance() directly; the OS-close path goes through WindowedHost + MockPlatform.

using retropp::ExitVerdict;
using retropp::RunLoop;
using retropp::TimingProfile;
using retropp::WindowedHost;
using retropp::test::ManualClock;
using retropp::test::MockPlatform;

namespace {

// The default-profile tick period (GBC) the cases advance the clock by, read from the profile.
constexpr auto kTickPeriod = TimingProfile::GameBoyColor.tickPeriod();

}  // namespace

TEST(ExitGuard, GuardProceedStopsAtBoundaryAndRunsCloseOut) {
    ManualClock clock;
    RunLoop     loop{clock};
    loop.renderLoop([] {});

    int         guardCalls = 0;
    std::string closeOut;
    loop.exitAction([&]() -> ExitVerdict {
        ++guardCalls;
        closeOut = "saved";  // the close-out work (a save stands in as a string write)
        return ExitVerdict::Proceed;
    });

    loop.advance();                // baseline (started_ == false) — no tick, the guard is not consulted
    loop.exitRequest();
    clock.advanceBy(kTickPeriod);  // one frame boundary elapses
    loop.advance();                // the tick batch runs, then the guard resolves at the boundary

    EXPECT_EQ(guardCalls, 1);
    EXPECT_TRUE(loop.exitResolved());
    EXPECT_FALSE(loop.exitPending());
    EXPECT_EQ(closeOut, "saved");
    EXPECT_EQ(loop.tickCount(), 1u);  // exactly one boundary's tick elapsed before the resolve
}

TEST(ExitGuard, GuardVetoKeepsRunningAndClearsPending) {
    ManualClock clock;
    RunLoop     loop{clock};
    loop.renderLoop([] {});

    int guardCalls = 0;
    loop.exitAction([&] { ++guardCalls; return ExitVerdict::Veto; });

    loop.advance();                // baseline
    loop.exitRequest();
    clock.advanceBy(kTickPeriod);
    loop.advance();                // the guard vetoes

    EXPECT_EQ(guardCalls, 1);
    EXPECT_FALSE(loop.exitPending());   // the request was abandoned
    EXPECT_FALSE(loop.exitResolved());  // and no teardown

    // The loop keeps running: a further advance does NOT consult the guard again (nothing is pending).
    clock.advanceBy(kTickPeriod);
    loop.advance();
    EXPECT_EQ(guardCalls, 1);
    EXPECT_FALSE(loop.exitResolved());
}

TEST(ExitGuard, NoGuardProceedsImmediately) {
    ManualClock clock;
    RunLoop     loop{clock};
    loop.renderLoop([] {});

    loop.advance();                // baseline
    loop.exitRequest();
    clock.advanceBy(kTickPeriod);
    loop.advance();                // no guard registered → Proceed

    EXPECT_TRUE(loop.exitResolved());
}

TEST(ExitGuard, RunStopsWhenGuardProceeds) {
    // run() terminates via the guard: a manual clock never advances on its own, so each advance() runs
    // zero ticks — but the guard is consulted every frame boundary while pending, so it Proceeds and
    // clears running_. Proves run() is not an infinite loop once a guard resolves the exit.
    ManualClock clock;
    RunLoop     loop{clock};
    loop.renderLoop([] {});
    loop.exitAction([] { return ExitVerdict::Proceed; });

    loop.advance();      // baseline so run()'s first advance is past the started_ latch
    loop.exitRequest();
    loop.run();          // advance() → guard Proceeds → running_ = false → run() returns

    EXPECT_TRUE(loop.exitResolved());
}

TEST(ExitGuard, OsCloseRoutesThroughGuardAndVetoClearsLatch) {
    ManualClock  clock;
    RunLoop      loop{clock};
    MockPlatform platform{1000};  // never auto-quit; the two OS-close events are driven explicitly
    loop.renderLoop([](float) {});

    int guardCalls = 0;
    loop.exitAction([&] { return ++guardCalls == 1 ? ExitVerdict::Veto : ExitVerdict::Proceed; });

    // Advance the clock one tick per pump so a boundary fires each iteration; latch an OS close at
    // pumps 2 and 4 — the first is vetoed (and the latch must clear so the loop survives), the second
    // proceeds.
    platform.setOnPump([&] {
        clock.advanceBy(kTickPeriod);
        const int n = platform.pumpCount();
        if (n == 2 || n == 4) platform.requestQuit();
    });

    WindowedHost host{loop, platform};
    host.run();

    EXPECT_EQ(guardCalls, 2);          // the window-close button ran the guard both times
    EXPECT_TRUE(loop.exitResolved());  // the second close proceeded and stopped the loop
    EXPECT_EQ(platform.pumpCount(), 4);
}
