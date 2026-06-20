#pragma once

#include "retropp/pacing.h"
#include "retropp/platform.h"
#include "retropp/run_loop.h"

namespace retropp {

// The hosted-mode driver: the windowed replacement for ENG-1's headless
// RunLoop::run(). It owns nothing and depends only on the two abstractions — a
// RunLoop and a Platform — so the whole pump → push-input → advance interleave is
// unit-testable against a MockPlatform with no live window or GPU device.
//
// Each iteration: drain OS events, push the platform's current held-button state AND its
// analog/pointer sample into the loop, then advance the simulation once. The present happens inside advance()
// via the consumer's render callback (so the ENG-1 "render once per advance with
// alpha" contract is preserved unchanged); the host owns only the scheduling. The
// loop terminates when the platform reports a quit request.
//
// Frame pacing (PACE-INTERP sub-block 1): after each present the host sleeps to a monotonic frame
// deadline (display-refresh-period spaced) via the Platform pacing seam, so the loop runs at the
// monitor's cadence instead of free-spinning when the vsync present fails to block. The deadline
// arithmetic is pure (pacing.h); the OS time/refresh/sleep primitives are the Platform seam, so the
// whole driver — pacing included — stays unit-testable against MockPlatform with no live device.
class WindowedHost {
public:
    WindowedHost(RunLoop& loop, Platform& platform) noexcept
        : loop_(loop), platform_(platform) {}

    // Pump → push input → advance, until the platform requests quit.
    void run();

private:
    RunLoop&  loop_;
    Platform& platform_;
};

}  // namespace retropp
