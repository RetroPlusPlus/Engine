#pragma once

#include "gbcpp/geometry.h"
#include "gbcpp/input.h"

namespace gbcpp {

// The host-OS boundary: window + GPU present + input + lifecycle, expressed as an
// abstract seam so the engine's scheduling and input-translation logic never depends
// on a live device. The production implementation is SdlPlatform; tests drive a
// MockPlatform, keeping the windowed-host driver verifiable headlessly. This is the
// ENG-2 analog of ENG-1's injectable Clock — the same seam discipline applied to the
// platform instead of time.
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

    // The window's current drawable size in physical pixels. The renderer reads this
    // each frame to letterbox the internal viewport into the window; it tracks window
    // resizes (the swapchain is kept sized to the window by the platform). On-screen
    // output itself is the renderer's job — the platform owns the window/device/input,
    // not the drawing.
    [[nodiscard]] virtual PixelSize drawableSize() const = 0;

    // Resize the window to `size` LOGICAL points — used to size the window to the chosen
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
};

}  // namespace gbcpp
