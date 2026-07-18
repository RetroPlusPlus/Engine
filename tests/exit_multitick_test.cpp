#include <gtest/gtest.h>

#include <string>

#include "retropp/run_loop.h"
#include "retropp/timing.h"

#include "manual_clock.h"

// The multi-tick close-out — the guard's reason to exist. An exit is requested, then a guard answers
// NotYet across several frame boundaries (a save-in-progress / fade-out), appending to a close-out
// artifact each boundary, and only Proceeds when it is done. The sim keeps advancing frame-by-frame
// while the guard closes out; the loop stops the boundary the guard Proceeds, and NO further boundary
// runs (the overshoot guard). Fixed bytes, not random — a unit test needs a stable expected value.

using retropp::ExitVerdict;
using retropp::RunLoop;
using retropp::TimingProfile;
using retropp::test::ManualClock;

namespace {

constexpr auto kTickPeriod = TimingProfile::GameBoyColor.tickPeriod();

}  // namespace

TEST(ExitMultiTick, MultiBoundaryCloseOutWritesInOrderThenStops) {
    ManualClock clock;
    RunLoop     loop{clock};
    loop.renderLoop([] {});

    std::string closeOut;  // the close-out artifact — a real save would be bytes on disk here
    int         boundary = 0;
    loop.exitAction([&]() -> ExitVerdict {
        ++boundary;
        if (boundary == 1) closeOut.clear();          // boundary 1 clears the artifact
        closeOut += std::to_string(boundary) + "\n";  // each boundary appends its number
        return boundary >= 3 ? ExitVerdict::Proceed : ExitVerdict::NotYet;
    });

    loop.advance();       // baseline — not pending yet, the guard is not consulted
    loop.exitRequest();

    int advances = 0;
    while (!loop.exitResolved()) {
        clock.advanceBy(kTickPeriod);  // one boundary per iteration
        loop.advance();
        ++advances;
        ASSERT_LE(advances, 10) << "the guard never Proceeded — runaway loop";
    }

    EXPECT_EQ(closeOut, "1\n2\n3\n");  // exact order + count
    EXPECT_EQ(boundary, 3);            // no 4th boundary ran — the overshoot guard
    EXPECT_EQ(advances, 3);            // three boundaries closed the exit out
    EXPECT_TRUE(loop.exitResolved());
}
