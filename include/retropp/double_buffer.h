#pragma once

namespace retropp {

// Opt-in interpolation-snapshot helper for the game side of the sim/render split.
// RunLoop supplies the interpolation factor alpha; the game holds its previous and
// current renderable snapshots here and blends them itself. The blend (lerp) is
// game-specific and lives in your code. Optional — render from current() alone for
// tick-quantized output.
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
