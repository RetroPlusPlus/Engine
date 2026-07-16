#include "retropp/windowed_host.h"

#include <cstdint>

namespace retropp {

void WindowedHost::run() {
    // Anchor the first frame deadline to "now"; nextFrameDeadline accumulates it by the display refresh
    // period each iteration (with a resync clamp on a stall). See pacing.h for the arithmetic.
    std::chrono::nanoseconds deadline = platform_.nowMonotonic();
    while (!platform_.quitRequested()) {
        platform_.pumpEvents();
        loop_.setRawInput(platform_.input());  // per-slot actions + analog/pointer, one sample
        const std::uint64_t ticksBefore = loop_.tickCount();
        loop_.advance();  // the render callback presents inside advance() (vsync still on top)

        // Flush gamepad vibration once per host frame that committed ≥ 1 tick: the game declared its
        // motor state inside advance()'s tick callback(s), so reconcile it against the device now (diff
        // → emit only changes). A zero-tick host frame does NOT flush — the game had no tick to declare
        // in, so a held rumble must not be reset to silence (see Platform::flushVibration).
        if (loop_.tickCount() != ticksBefore) {
            platform_.flushVibration();
        }

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
