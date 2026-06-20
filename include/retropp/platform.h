#pragma once

#include <chrono>

#include "retropp/analog_input.h"
#include "retropp/geometry.h"
#include "retropp/input.h"

namespace retropp {

// The host-OS boundary: window + GPU present + input + lifecycle, expressed as an
// abstract seam so the engine's scheduling and input-translation logic never depends
// on a live device. The production implementation is SdlPlatform; tests drive a
// MockPlatform, keeping the windowed-host driver verifiable headlessly. It is to the
// platform what the run loop's injectable Clock is to time — the same seam discipline.
//
// No hardware-register or scanline idioms cross this boundary: input is sampled
// held-button state (a ButtonSet), and a frame is presented whole. There are no
// per-line, per-register, or mid-frame hooks.
class Platform {
public:
    virtual ~Platform() = default;

    // Drain the OS event queue: refresh the held-button state and latch a quit
    // request (window-close button or OS quit). Called once per host iteration.
    virtual void pumpEvents() = 0;

    // True once the user has asked to close the window. The windowed host stops when
    // this becomes true.
    [[nodiscard]] virtual bool quitRequested() const = 0;

    // The 8 canonical buttons currently held, as of the most recent pumpEvents().
    [[nodiscard]] virtual ButtonSet buttons() const = 0;

    // The analog / pointer surface as of the most recent pumpEvents(). Cursor is in VIEWPORT pixels
    // (the platform inverts its own letterbox/integer-scale blit so the coordinate matches what is
    // drawn); the relative quantities (rawDelta, wheel) are this FRAME's accumulated motion, which the
    // run loop sums between ticks. Rides parallel to buttons() — the digital path is unchanged.
    [[nodiscard]] virtual AnalogInput analog() const = 0;

    // Enter / leave relative-pointer (capture) mode: the OS cursor is hidden and confined, and motion
    // arrives as unbounded relative deltas (rawDeltaX/Y) — the authentic rotary-spinner / mouse-look
    // feel. While captured there is no meaningful absolute cursor. A game toggles this per context (on
    // for a spinner level, off for a menu). Host-OS-agnostic; a backend without it no-ops.
    virtual void setPointerCaptured(bool captured) = 0;

    // Whether the pointer is currently captured (relative mode).
    [[nodiscard]] virtual bool pointerCaptured() const = 0;

    // Show or hide the host-OS hardware cursor — INDEPENDENTLY of pointer capture. A game that draws
    // its own cursor (a reticle, a paddle the mouse drives) hides the OS arrow while keeping absolute
    // cursor tracking live: analog().cursor and cursorOnScreen still update. This is distinct from
    // setPointerCaptured, which hides AND confines the cursor and switches motion to relative-only —
    // the two are orthogonal knobs (a game may hide the cursor without capturing, or capture without
    // caring about visibility). The OS cursor starts visible. Host-OS-agnostic; a backend without a
    // cursor no-ops.
    virtual void setCursorVisible(bool visible) = 0;

    // Whether the host-OS cursor is currently shown (the explicit setCursorVisible state, independent
    // of capture).
    [[nodiscard]] virtual bool cursorVisible() const = 0;

    // The window's current drawable size in physical pixels. The renderer reads this
    // each frame to letterbox the internal viewport into the window; it tracks window
    // resizes (the swapchain is kept sized to the window by the platform). On-screen
    // output itself is the renderer's job — the platform owns the window/device/input,
    // not the drawing.
    [[nodiscard]] virtual PixelSize drawableSize() const = 0;

    // Resize the window to `size` LOGICAL points — sizes the window to the chosen
    // presentation scale (viewport × windowScale), so the content fills the window snugly. Logical
    // points (not physical pixels) so the perceived size is the same on any display density; the
    // drawable the renderer fills is this × the display's pixel density. The OS may clamp to its
    // min/max window size; drawableSize() reports the realized physical size after the fact.
    // Host-OS-agnostic — a backend with no resizable window no-ops.
    virtual void setWindowSize(PixelSize size) = 0;

    // The usable area of the display the window is on, in LOGICAL points (the desktop work area minus
    // OS chrome — menu bar / dock / taskbar). Same units as setWindowSize, so the scaling logic can
    // pick the largest window scale that fits the screen and never overflow it (see fitWindowScale in
    // geometry.h). A backend without a queryable display returns a safe fallback.
    [[nodiscard]] virtual PixelSize usableDisplaySize() const = 0;

    // Enter or leave OS-native fullscreen. The production platform uses the host's real
    // fullscreen affordance (on macOS a fullscreen Space, not a fake borderless window).
    // Fullscreen does NOT make the window freely resizable; the existing letterbox /
    // integer-scale blit handles the new target size. Host-OS-agnostic — a future
    // touch/mobile backend implements it per its OS or no-ops.
    virtual void setFullscreen(bool enabled) = 0;

    // Whether the platform is currently in fullscreen.
    [[nodiscard]] virtual bool isFullscreen() const = 0;

    // ── Frame pacing ────────────────────────────────────────────────────────────
    // The OS-coupled primitives the windowed host uses to pace each iteration to a monotonic frame
    // deadline (the deadline arithmetic itself is pure — see pacing.h). They let the host enforce the
    // display cadence directly, independent of whether the vsync present blocks (which not every
    // platform reliably does while the window is idle).

    // Current monotonic time. Distinct from RunLoop's injected Clock (private to the loop, drives sim
    // ticks) — the host needs its own read, in the SAME clock domain as sleepPrecise(), to compute the
    // remainder to the next frame deadline. A backend with no clock returns a monotonically increasing
    // value of its choosing (tests inject a controllable one).
    [[nodiscard]] virtual std::chrono::nanoseconds nowMonotonic() const = 0;

    // The refresh period (1 / refresh_rate, in ns) of the display the window is on, or a safe 60 Hz
    // fallback when the rate is unavailable. The host paces to this so the loop runs at the monitor's
    // cadence. Queried live each iteration (one trivial call per frame) so a window dragged to a
    // different-refresh display re-paces with no event handling.
    [[nodiscard]] virtual std::chrono::nanoseconds displayRefreshPeriod() const = 0;

    // Precise-sleep the calling thread for `duration` (no-op if <= 0). Called once per host iteration,
    // after the frame is presented, for the host's computed remainder-to-deadline. The production
    // platform uses a high-resolution sleep; a test platform records the request and does not wait, so
    // the host suite stays instant and deterministic.
    virtual void sleepPrecise(std::chrono::nanoseconds duration) = 0;
};

}  // namespace retropp
