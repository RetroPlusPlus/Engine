#pragma once

// The pure auto-close decision, factored out so it is unit-testable without a VM or a device.
//
// SFX are fire-and-forget — the engine closes a one-shot when its output has gone silent (the AudioSystem
// stops stepping the VM). Music is a resource the game opens and closes on demand and is NEVER
// auto-closed. So a finished one-shot SFX is exactly: type == Sfx AND the output has been exact-zero for
// at least `thresholdFrames` consecutive frames.

#include <cstddef>

#include "retropp/audio_library.h"  // AudioType

namespace retropp::detail {

[[nodiscard]] constexpr bool shouldAutoStop(std::size_t silenceRunFrames, std::size_t thresholdFrames,
                                            AudioType type) noexcept {
    return type == AudioType::Sfx && silenceRunFrames >= thresholdFrames;
}

}  // namespace retropp::detail
