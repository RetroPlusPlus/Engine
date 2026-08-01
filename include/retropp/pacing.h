#pragma once

#include <chrono>

namespace retropp {

// ── Host-loop frame pacing ───────────────────────────────────────────────────────
// The pure arithmetic behind the windowed host's frame deadline. WindowedHost owns the deadline
// and feeds this function the OS-coupled inputs (current monotonic time + the display refresh
// period, both via the Platform seam); the SDL realization is the SDL_GetTicksNS / SDL_DelayPrecise
// plumbing around it. The decision logic stays here — chrono-only, no clock, no sleep, no SDL — so
// it is unit-testable without a device.
//
// A monotonic per-frame deadline paces the loop to the display refresh independent of the vsync
// present block (which not every platform reliably honors while the window is idle). When the
// present does block, now is already at the deadline and the computed sleep is ~0, so the deadline
// and vsync compose rather than fight.

struct FrameDeadline {
    std::chrono::nanoseconds nextDeadline;  // carry into the next iteration
    std::chrono::nanoseconds sleepFor;      // remainder to sleep now (never negative)
};

// Deadline pacing — the deadline never banks presentation debt:
//   sleepFor     = max(0, prevDeadline - now)              — wait out the rest of this frame
//   nextDeadline = max(prevDeadline, now) + period         — accumulate when on time, re-anchor when late
// On time or early, the deadline accumulates from itself, so the cadence is drift-free. Late by any
// amount, it re-anchors to now: the lateness is forgiven rather than carried, and the next iteration
// sleeps a full period minus its work. A frame therefore computes sleepFor == 0 only when the frame
// before it consumed at least a full period, so a run of slow frames cannot accrue a sleep debt that
// discharges as a burst of unpaced ones on recovery. This bounds presentation backlog and is
// orthogonal to RunLoop's kMaxFrameTime sim-spiral clamp, which bounds ticks per advance(). A
// non-positive period (a degenerate refresh query) yields no sleep and nextDeadline = now, so the
// loop simply free-runs under vsync rather than dividing by it.
[[nodiscard]] constexpr FrameDeadline nextFrameDeadline(std::chrono::nanoseconds prevDeadline,
                                                        std::chrono::nanoseconds period,
                                                        std::chrono::nanoseconds now) noexcept {
    using ns = std::chrono::nanoseconds;
    if (period <= ns::zero()) {
        return {now, ns::zero()};
    }
    const ns sleepFor = prevDeadline > now ? prevDeadline - now : ns::zero();
    const ns anchor   = prevDeadline > now ? prevDeadline : now;  // late ⇒ re-anchor, never bank
    return {anchor + period, sleepFor};
}

}  // namespace retropp
