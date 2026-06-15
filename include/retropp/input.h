#pragma once

#include <cstdint>
#include <initializer_list>

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

// The 8 canonical buttons currently held, packed one bit per button (bit index ==
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

// Per-tick input view: held state for the current tick plus edges relative to the
// previous tick. Edges are sim-tick-keyed (ENG-1 PLAN Decision #10/#12), so they are
// deterministic and frame-rate-independent — never sampled at render cadence.
class InputState {
public:
    [[nodiscard]] bool isHeld(Button b) const noexcept;        // held this tick
    [[nodiscard]] bool justPressed(Button b) const noexcept;   // released→held this tick
    [[nodiscard]] bool justReleased(Button b) const noexcept;  // held→released this tick

    // Engine-internal: advance one tick. The current sample becomes previous; the
    // freshly-pushed raw sample becomes current. Edges derive from (previous, current).
    // On the first tick previous_ is the all-released default, so a button already held
    // on tick 1 reads as justPressed — the correct edge for a press that landed between
    // baseline and the first tick.
    void sampleTick(ButtonSet raw) noexcept;

private:
    ButtonSet previous_;
    ButtonSet current_;
};

}  // namespace retropp
