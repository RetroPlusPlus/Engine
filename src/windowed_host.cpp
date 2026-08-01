#include "retropp/windowed_host.h"

#include <cstdint>

namespace retropp {

void WindowedHost::run() {
    // Anchor the first frame deadline to "now"; nextFrameDeadline advances it by the display refresh
    // period each iteration, re-anchoring to now whenever the iteration ran late so lateness is never
    // carried into the following frames. See pacing.h for the arithmetic.
    std::chrono::nanoseconds deadline = platform_.nowMonotonic();
    while (!loop_.exitResolved()) {  // stop when a guard resolves the exit to Proceed (OS-close or programmatic)
        platform_.pumpEvents();

        // Route the OS window-close through the same exit guard as a programmatic exit: union it into
        // the loop's pending state so the X button runs the close-out too. Idempotent while pending.
        if (platform_.quitRequested()) loop_.exitRequest();

        loop_.setRawInput(platform_.input());  // per-slot actions + analog/pointer, one sample
        const bool wasPending = loop_.exitPending();
        const std::uint64_t ticksBefore = loop_.tickCount();
        loop_.advance();  // the render callback presents inside advance() (vsync still on top); resolves the guard at the boundary

        // A VETO — the guard cleared the pending exit without resolving — must also clear the OS quit
        // latch, else quitRequested() stays true and re-raises the exit next iteration so the guard
        // could never be answered "No". Proceed needs no clear (we are stopping); a still-pending
        // NotYet keeps both the pending state and the latch for the next boundary.
        if (wasPending && !loop_.exitPending() && !loop_.exitResolved()) {
            platform_.clearQuitRequest();
        }

        // Flush gamepad vibration once per host frame that committed ≥ 1 tick: the game declared its
        // motor state inside advance()'s tick callback(s), so reconcile it against the device now (diff
        // → emit only changes). A zero-tick host frame does NOT flush — the game had no tick to declare
        // in, so a held rumble must not be reset to silence (see Platform::flushVibration).
        if (loop_.tickCount() != ticksBefore) {
            platform_.flushVibration();
        }

        // Drive the platform's window once per frame, unconditionally: the pointer's raw delta is a
        // per-pump quantity, so skipping a zero-tick frame would silently drop that frame's drag
        // motion. The frame period gives the automatic movement its per-second speed base.
        platform_.window().update(platform_.input(), platform_.displayRefreshPeriod());

        // Pace to the display: sleep out the remainder of this frame. When the vsync present already
        // blocked, now is at/past the deadline and sleepFor is ~0; when it didn't, sleepFor is ≈ the
        // full refresh period, so the loop holds the display cadence instead of spinning.
        const FrameDeadline fd =
            nextFrameDeadline(deadline, platform_.displayRefreshPeriod(), platform_.nowMonotonic());
        deadline = fd.nextDeadline;
        platform_.sleepPrecise(fd.sleepFor);
    }
}

}  // namespace retropp
