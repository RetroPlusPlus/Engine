#pragma once

// The frame timing a test hands the interpolator, spelled at the call site so each one reads as the
// point in the tick it names.

#include <cstdint>

#include "retropp/frame_timing.h"

namespace retropp {

// A steady single-tick iteration at sub-tick fraction `subTick`. Under the default cadence a slot that
// changed on this commit eases at exactly that fraction, so `alpha` and the factor coincide.
[[nodiscard]] inline FrameTiming tickAt(float subTick) {
    return FrameTiming{.alpha = subTick, .tickAdvanced = true, .subTick = subTick, .commitSpan = 1};
}

// An iteration whose commit ran `span` ticks. `alpha` is the fraction mapped across that span, which is
// what the run loop publishes for a catch-up frame.
[[nodiscard]] inline FrameTiming tickAt(float subTick, std::uint32_t span) {
    return FrameTiming{.alpha = (static_cast<float>(span - 1) + subTick) / static_cast<float>(span),
                       .tickAdvanced = true,
                       .subTick      = subTick,
                       .commitSpan   = span};
}

// An iteration that committed no tick: the mirror is untouched and the fraction keeps growing.
[[nodiscard]] inline FrameTiming betweenTicks(float subTick, std::uint32_t span) {
    FrameTiming t   = tickAt(subTick, span);
    t.tickAdvanced  = false;
    return t;
}

}  // namespace retropp
