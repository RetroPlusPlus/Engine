#pragma once

#include <chrono>

namespace retropp {

// ── Host-loop frame pacing (PACE-INTERP sub-block 1) ─────────────────────────────
// The pure arithmetic behind the windowed host's frame deadline. WindowedHost owns the
// deadline and feeds this function the OS-coupled inputs (current monotonic time + the
// display refresh period, both via the Platform seam); the SDL realization is just the
// SDL_GetTicksNS / SDL_DelayPrecise plumbing around it. Keeping the decision logic here —
// chrono-only, no clock, no sleep, no SDL — is the engine's standard "pure CPU mirror +
// device-free test" pattern (cf. fitWindowScale, frameColorTransform, displaceSourceUv).
//
// Why a deadline at all: the host loop's only throttle used to be the vsync present block,
// which macOS does not reliably honor while the window is idle — so the loop free-spun, wasting
// CPU and desyncing audio. A monotonic per-frame deadline paces the loop to the display refresh
// regardless of whether the present blocked; when the present DID block, now is already at the
// deadline and the computed sleep is ~0, so the two compose rather than fight.

struct FrameDeadline {
    std::chrono::nanoseconds nextDeadline;  // carry into the next iteration
    std::chrono::nanoseconds sleepFor;      // remainder to sleep now (never negative)
};

// Monotonic-accumulate pacing with a resync clamp:
//   sleepFor     = max(0, prevDeadline - now)              — wait out the rest of this frame
//   nextDeadline = prevDeadline + period                   — accumulate (drift-free cadence)
// EXCEPT when the loop has fallen behind by more than `maxLagPeriods` periods
// (now - prevDeadline > maxLagPeriods * period): then nextDeadline snaps to now + period, so a
// stall (debugger break, GC pause) never banks a sleep debt that fast-forwards on recovery. This is
// orthogonal to RunLoop's kMaxFrameTime sim-spiral clamp — that bounds ticks per advance(); this
// bounds presentation backlog. A non-positive period (a degenerate refresh query) yields no sleep
// and nextDeadline = now, so the loop simply free-runs under vsync rather than dividing by it.
[[nodiscard]] constexpr FrameDeadline nextFrameDeadline(std::chrono::nanoseconds prevDeadline,
                                                        std::chrono::nanoseconds period,
                                                        std::chrono::nanoseconds now,
                                                        int maxLagPeriods = 4) noexcept {
    using ns = std::chrono::nanoseconds;
    if (period <= ns::zero()) {
        return {now, ns::zero()};
    }
    const ns behind = now - prevDeadline;            // > 0 when we are late
    if (behind > period * maxLagPeriods) {
        return {now + period, ns::zero()};           // too far behind → resync, drop the backlog
    }
    const ns sleepFor = behind < ns::zero() ? -behind : ns::zero();  // = max(0, prevDeadline - now)
    return {prevDeadline + period, sleepFor};
}

}  // namespace retropp
