#pragma once

#include "gbcpp/platform.h"
#include "gbcpp/run_loop.h"

namespace gbcpp {

// The hosted-mode driver: the windowed replacement for ENG-1's headless
// RunLoop::run(). It owns nothing and depends only on the two abstractions — a
// RunLoop and a Platform — so the whole pump → push-input → advance interleave is
// unit-testable against a MockPlatform with no live window or GPU device.
//
// Each iteration: drain OS events, push the platform's current held-button state into
// the loop, then advance the simulation once. The present happens inside advance()
// via the consumer's render callback (so the ENG-1 "render once per advance with
// alpha" contract is preserved unchanged); the host owns only the scheduling. The
// loop terminates when the platform reports a quit request.
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

}  // namespace gbcpp
