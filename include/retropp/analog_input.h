#pragma once

#include <cstdint>

#include "retropp/geometry.h"

namespace retropp {

// ── The analog / pointer input surface (ENG-2.A follow-on) ────────────────────────────────────────
//
// A single std-only value type carried BESIDE ButtonSet — never folded into it. The digital console
// model (Button / ButtonSet / InputProfile) is untouched; this rides parallel so an arcade or modern
// control idiom (mouse, spinner, paddle, twin-stick, analog triggers) is expressible while a faithful
// console port simply ignores it. The raw per-tick sample is the pair (ButtonSet, AnalogInput).
//
// Two kinds of quantity live here, with different per-tick semantics (see run_loop.h):
//   • ABSOLUTE — cursor position, stick values, the held-button mask: the latest value at the tick.
//   • RELATIVE — rawDelta (device motion) and wheel: ACCUMULATE across every event pumped between two
//     ticks and reset on sampleTick, so no motion is lost even when a host frame produces zero ticks
//     (a fast spinner flick still registers its full magnitude).

// A mouse button — the bit index into AnalogInput::mouseHeld.
enum class MouseButton : std::uint8_t { Left, Right, Middle };

// A gamepad analog stick. stick(Stick) reports its {x, y} in [-1, 1] (dead-zoned by the platform).
enum class Stick : std::uint8_t { Left, Right };

// A gamepad analog trigger. trigger(Trigger) reports its pull in [0, 1] (dead-zoned by the platform).
enum class Trigger : std::uint8_t { Left, Right };

struct AnalogInput {
    // ── Pointer (mouse) ──
    Vec2i cursor{};                // ABSOLUTE position in VIEWPORT pixels (the internal render space)
    bool  cursorOnScreen = false;  // the pointer is inside the drawn viewport (false off it / in a bar)
    float rawDeltaX = 0.0f;        // RELATIVE raw device motion since the last tick (what a spinner
    float rawDeltaY = 0.0f;        //   integrates — independent of output scale)
    float wheel     = 0.0f;        // RELATIVE wheel delta since the last tick (accumulated)
    std::uint8_t mouseHeld = 0;    // ABSOLUTE: bit per MouseButton currently held

    // ── Gamepad analog ──
    float leftX = 0.0f, leftY = 0.0f;    // left stick,  [-1, 1] per axis (dead-zoned)
    float rightX = 0.0f, rightY = 0.0f;  // right stick, [-1, 1] per axis (dead-zoned)
    float triggerL = 0.0f, triggerR = 0.0f;  // triggers, [0, 1] (dead-zoned)

    [[nodiscard]] constexpr bool mouseDown(MouseButton b) const noexcept {
        return (mouseHeld & static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(b))) != 0;
    }

    // Fold one host-FRAME sample into a per-TICK accumulator: relative quantities sum (so a 0-tick
    // host frame's motion is carried to the next tick rather than discarded); absolute quantities take
    // the frame's latest. The run loop calls this each setRawAnalog and clearRelatives() at each tick.
    constexpr void accumulateFrom(const AnalogInput& frame) noexcept {
        rawDeltaX += frame.rawDeltaX;
        rawDeltaY += frame.rawDeltaY;
        wheel     += frame.wheel;
        cursor         = frame.cursor;
        cursorOnScreen = frame.cursorOnScreen;
        mouseHeld      = frame.mouseHeld;
        leftX = frame.leftX;   leftY = frame.leftY;
        rightX = frame.rightX; rightY = frame.rightY;
        triggerL = frame.triggerL; triggerR = frame.triggerR;
    }

    // Zero the relative quantities, keeping the absolutes — called once per tick after the tick has
    // consumed the accumulated motion, so the next tick's window starts fresh.
    constexpr void clearRelatives() noexcept {
        rawDeltaX = 0.0f;
        rawDeltaY = 0.0f;
        wheel     = 0.0f;
    }

    [[nodiscard]] constexpr bool operator==(const AnalogInput&) const noexcept = default;
};

}  // namespace retropp
