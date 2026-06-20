#pragma once

#include <cstdint>
#include <initializer_list>

#include "retropp/analog_input.h"
#include "retropp/geometry.h"

namespace retropp {

// The canonical 8-/16-bit-console input surface: the LOGICAL buttons a consumer port
// speaks, regardless of the physical device the platform layer maps from. The
// enumerator VALUE is the bit index in ButtonSet.
//
// The shipped set covers Game Boy / NES (Up..Select) and SNES (adds X, Y, L, R). New
// buttons (Genesis C/Z/Mode, …) are APPENDED at the end — appending is purely additive:
// ButtonSet's 32-bit storage already has room, so adding a button never reshapes a type
// or breaks ABI. Bump kButtonCount when you append. Console-specific labels (Master
// System's "1"/"2" for A/B, etc.) are a glyph-layer concern — the logical names are fixed.
enum class Button : std::uint8_t {
    Up, Down, Left, Right,   // 0..3  — d-pad
    A, B,                    // 4,5   — Game Boy / NES / SNES / Genesis / SMS (1,2)
    X, Y,                    // 6,7   — SNES (and Genesis 6-button)
    L, R,                    // 8,9   — SNES shoulders
    Start, Select,           // 10,11 — Game Boy / NES / SNES (SMS Pause / Reset)
    // ← append future buttons here (Genesis C/Z/Mode, …); bump kButtonCount
};
inline constexpr int kButtonCount = 12;  // shipped button count; bump when appending

// The canonical buttons currently held, packed one bit per button (bit index ==
// the Button enumerator value). A value type — cheap to copy, compare, and snapshot.
class ButtonSet {
public:
    constexpr ButtonSet() noexcept = default;

    constexpr void set(Button b, bool held) noexcept {
        const std::uint32_t mask = std::uint32_t{1} << static_cast<unsigned>(b);
        bits_ = held ? (bits_ | mask) : (bits_ & ~mask);
    }

    [[nodiscard]] constexpr bool held(Button b) const noexcept {
        const std::uint32_t mask = std::uint32_t{1} << static_cast<unsigned>(b);
        return (bits_ & mask) != 0;
    }

    [[nodiscard]] constexpr std::uint32_t bits() const noexcept { return bits_; }

    // Union: every button held in EITHER set. The run loop ORs each host frame's held state into a
    // per-tick accumulator (heldUnion), so a button that was down at ANY point between two ticks is
    // seen by the tick — a press is never dropped just because it was released before the tick sampled
    // it (see run_loop.h's per-tick sampling).
    constexpr ButtonSet& operator|=(ButtonSet other) noexcept {
        bits_ |= other.bits_;
        return *this;
    }
    [[nodiscard]] friend constexpr ButtonSet operator|(ButtonSet a, ButtonSet b) noexcept {
        return a |= b;
    }

    friend constexpr bool operator==(ButtonSet, ButtonSet) noexcept = default;

private:
    // One bit per Button. 32-bit storage gives 32 button slots, so appending buttons /
    // console profiles is forever additive — it never re-widens this type or breaks ABI.
    std::uint32_t bits_ = 0;
};

// Build a ButtonSet from a button list — the readable mask builder an InputProfile
// (input_map.h) declares its buttons with. constexpr so profiles are compile-time constants.
[[nodiscard]] constexpr ButtonSet makeButtonSet(std::initializer_list<Button> buttons) noexcept {
    ButtonSet set;
    for (Button b : buttons) set.set(b, true);
    return set;
}

// Per-tick input view: held state for the current tick plus edges relative to the previous tick, and
// the analog/pointer surface sampled at the same tick. Edges are sim-tick-keyed, so they are
// deterministic and frame-rate-independent — never sampled at render cadence.
class InputState {
public:
    // ── Digital buttons ──
    [[nodiscard]] bool isHeld(Button b) const noexcept;        // held this tick (honest level)
    [[nodiscard]] bool justPressed(Button b) const noexcept;   // pressed since the last tick
    [[nodiscard]] bool justReleased(Button b) const noexcept;  // held→released this tick

    // ── Pointer (mouse) ──
    [[nodiscard]] Vec2i cursor() const noexcept;          // absolute viewport-pixel position
    [[nodiscard]] bool  cursorOnScreen() const noexcept;  // pointer is over the drawn viewport
    [[nodiscard]] Vec2i cursorDelta() const noexcept;     // viewport-pixel change since the last tick
    [[nodiscard]] float rawDeltaX() const noexcept;       // accumulated raw device motion (spinner)
    [[nodiscard]] float rawDeltaY() const noexcept;
    [[nodiscard]] float wheel() const noexcept;           // accumulated wheel delta this tick
    [[nodiscard]] bool  mouseHeld(MouseButton b) const noexcept;
    [[nodiscard]] bool  mouseJustPressed(MouseButton b) const noexcept;   // mirrors the digital edges
    [[nodiscard]] bool  mouseJustReleased(MouseButton b) const noexcept;

    // ── Gamepad analog ──
    [[nodiscard]] Vec2  stick(Stick s) const noexcept;      // {x, y} in [-1, 1]
    [[nodiscard]] float trigger(Trigger t) const noexcept;  // [0, 1]

    // Engine-internal: advance one tick. `held` is the latest level this tick; `pressedSinceTick` is
    // the UNION of every host-frame held state observed since the previous tick (the run loop's
    // heldUnion). justPressed fires for any button in `pressedSinceTick` that was not held at the
    // previous tick — so a tap shorter than a tick (visible in a host-frame poll but already released
    // by tick time) still registers exactly one press, never silently dropped. held()/justReleased()
    // stay honest level edges off `held`, so a release never sticks. The analog sample is stored and
    // its viewport-space cursorDelta derives from (previous cursor, current cursor).
    //
    // On the first tick previous_ is the all-released default, so a button already held on tick 1
    // reads as justPressed — the correct edge for a press that landed between baseline and tick 1.
    void sampleTick(ButtonSet held, ButtonSet pressedSinceTick, const AnalogInput& analog) noexcept;

private:
    ButtonSet previous_;
    ButtonSet current_;
    ButtonSet pressed_;       // union of held states seen since the previous tick (press buffering)
    AnalogInput analogPrev_;  // previous tick's analog sample (for cursorDelta + mouse edges)
    AnalogInput analog_;      // this tick's analog sample
};

}  // namespace retropp
