#pragma once

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

    // Present one frame: acquire the swapchain, clear it to the bring-up colour,
    // submit, and present. This is ENG-2.A's entire on-screen output; ENG-2.B
    // replaces it with the real draw-state submission path.
    virtual void presentClearFrame() = 0;
};

}  // namespace gbcpp
