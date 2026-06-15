#pragma once

namespace retropp {

// Opt-in interpolation-snapshot helper for the game side of the sim/render split.
// The engine supplies the interpolation factor alpha (RunLoop); the game holds its
// prev/curr renderable snapshots here and blends them itself. The blend (lerp) is
// game-specific and lives in the consumer — see ENG-1 PLAN §6. Not required by the
// engine; shipped because the first consumer needs it immediately.
template <typename T>
class DoubleBuffer {
public:
    [[nodiscard]] T&       current()       noexcept { return current_; }
    [[nodiscard]] const T& current() const noexcept { return current_; }
    [[nodiscard]] const T& previous() const noexcept { return previous_; }

    // Call once per simulation tick, before mutating current(): the just-completed
    // state becomes "previous", ready to be the interpolation source next render.
    void advance() { previous_ = current_; }

private:
    T previous_{};
    T current_{};
};

}  // namespace retropp
