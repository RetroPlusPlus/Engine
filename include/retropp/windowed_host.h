#pragma once

#include "retropp/pacing.h"
#include "retropp/platform.h"
#include "retropp/run_loop.h"

namespace retropp {

// The hosted-mode driver: the windowed replacement for the run loop's headless
// RunLoop::run(). It owns nothing and depends only on the two abstractions — a
// RunLoop and a Platform — so the whole pump → push-input → advance interleave is
// unit-testable against a MockPlatform with no live window or GPU device.
//
// Each iteration: drain OS events, push the platform's current held-button state AND its
// analog/pointer sample into the loop, then advance the simulation once. The present happens
// inside advance() via the consumer's render callback (the run loop renders once per advance with
// alpha); the host owns only the scheduling. The loop terminates when the platform reports a quit
// request.
//
// Frame pacing: after each present the host sleeps to a monotonic frame deadline (spaced by the
// display refresh period) via the Platform pacing seam, so the loop runs at the monitor's cadence
// rather than relying on the vsync present to throttle it. The deadline arithmetic is pure
// (pacing.h); the OS time/refresh/sleep primitives are the Platform seam, so the whole driver —
// pacing included — stays unit-testable against MockPlatform with no live device.
class WindowedHost {
public:
    WindowedHost(RunLoop& loop, Platform& platform) noexcept
        : loop_(loop), platform_(platform) {}

    // Pump → push input → advance, until the platform requests quit. The host also drives the
    // platform's window once per frame (automatic window movement), beside the sim and the exit
    // guard.
    void run();

private:
    RunLoop&  loop_;
    Platform& platform_;
};

}  // namespace retropp
