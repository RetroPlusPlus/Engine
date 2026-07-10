#include "retropp/input.h"

namespace retropp {

void InputState::sampleTick(const InputSample& sample,
                            const std::array<ActionSet, kMaxPlayers>& pressedSinceTick) noexcept {
    for (int i = 0; i < kMaxPlayers; ++i) {
        Slot&               slot = slots_[static_cast<std::size_t>(i)];
        const PlayerSample& in   = sample.players[static_cast<std::size_t>(i)];
        slot.previous   = slot.current;
        slot.current    = in.held;
        slot.pressed    = pressedSinceTick[static_cast<std::size_t>(i)];
        slot.values     = in.values;
        slot.analogPrev = slot.analog;
        slot.analog     = in.analog;
        slot.device     = in.device;
    }
}

}  // namespace retropp
