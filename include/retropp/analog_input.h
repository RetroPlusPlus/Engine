#pragma once

#include <cstdint>

#include "retropp/geometry.h"

namespace retropp {

// ── The analog / pointer input surface ──────────────────────────────────────────────────────────────
//
// A single std-only value type carried BESIDE the digital action state — never folded into it. It
// rides inside each player slot's sample (input.h PlayerSample) so an arcade or modern control
// idiom (mouse, spinner, paddle, twin-stick, analog triggers) is expressible while a game that only
// reads actions simply ignores it.
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

    // ── Gamepad analog (processed by the configured AnalogResponse — dead-zone + gate) ──
    float leftX = 0.0f, leftY = 0.0f;    // left stick,  [-1, 1] per axis (processed)
    float rightX = 0.0f, rightY = 0.0f;  // right stick, [-1, 1] per axis (processed)
    float triggerL = 0.0f, triggerR = 0.0f;  // triggers, [0, 1] (processed)

    // The same inputs BEFORE processing — the untouched hardware readings, so a game can read raw and
    // processed at once (stickRaw()/triggerRaw()). ABSOLUTE, latest-at-tick like the processed axes.
    float rawLeftX = 0.0f, rawLeftY = 0.0f;
    float rawRightX = 0.0f, rawRightY = 0.0f;
    float rawTriggerL = 0.0f, rawTriggerR = 0.0f;

    [[nodiscard]] constexpr bool mouseDown(MouseButton b) const noexcept {
        return (mouseHeld & static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(b))) != 0;
    }

    // Fold one host-FRAME sample into a per-TICK accumulator: relative quantities sum (so a 0-tick
    // host frame's motion is carried to the next tick rather than discarded); absolute quantities take
    // the frame's latest. The run loop calls this each setRawInput and clearRelatives() at each tick.
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
        rawLeftX = frame.rawLeftX;   rawLeftY = frame.rawLeftY;
        rawRightX = frame.rawRightX; rawRightY = frame.rawRightY;
        rawTriggerL = frame.rawTriggerL; rawTriggerR = frame.rawTriggerR;
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
