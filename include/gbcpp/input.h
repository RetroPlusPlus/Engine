#pragma once

#include <cstdint>

namespace gbcpp {

// The canonical Game-Boy-family input surface: the 8 buttons every consumer port
// speaks, regardless of the physical device the (later, ENG-2) platform layer maps
// from. The enumerator value is the bit index in ButtonSet.
enum class Button : std::uint8_t { Up, Down, Left, Right, A, B, Start, Select };
inline constexpr int kButtonCount = 8;

// The 8 canonical buttons currently held, packed one bit per button (bit index ==
// the Button enumerator value). A value type — cheap to copy, compare, and snapshot.
class ButtonSet {
public:
    constexpr ButtonSet() noexcept = default;

    constexpr void set(Button b, bool held) noexcept {
        const auto mask = static_cast<std::uint8_t>(1u << static_cast<unsigned>(b));
        bits_ = held ? static_cast<std::uint8_t>(bits_ | mask)
                     : static_cast<std::uint8_t>(bits_ & static_cast<std::uint8_t>(~mask));
    }

    [[nodiscard]] constexpr bool held(Button b) const noexcept {
        const auto mask = static_cast<std::uint8_t>(1u << static_cast<unsigned>(b));
        return (bits_ & mask) != 0;
    }

    [[nodiscard]] constexpr std::uint8_t bits() const noexcept { return bits_; }

    friend constexpr bool operator==(ButtonSet, ButtonSet) noexcept = default;

private:
    std::uint8_t bits_ = 0;
};

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

}  // namespace gbcpp
