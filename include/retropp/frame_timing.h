#pragma once

namespace retropp {

// The render-timing the run loop hands the renderer each iteration: the sub-tick blend factor and whether a
// simulation tick committed this iteration. The renderer interpolates each object between its previous and
// current tick state by `alpha`, and rotates that per-object history once per tick (on `tickAdvanced`).
struct FrameTiming {
    float alpha        = 0.0f;   // accumulator / tickPeriod, in [0, 1) — the sub-tick blend factor
    bool  tickAdvanced = false;  // a simulation tick committed this loop iteration
};

// The channel the run loop uses to reach the renderer without sharing a reference. The loop computes the
// blend factor and the tick signal but holds no renderer pointer (the game's render callback is the only
// link, and it stays renderFrame(frame) — no interpolation argument). The loop publishes here right before
// invoking the render callback; the renderer reads here at the top of renderFrame. SDL-free, so the run
// loop stays SDL-free.
//
// Scoped to the calling thread (the single-threaded loop publishes and renders on the same thread). A
// thread whose loop has not published yet reads the default (alpha 0, tickAdvanced false) — which the
// renderer composites as the submission verbatim, the correct first-frame behaviour.
void                      publishFrameTiming(FrameTiming timing) noexcept;
[[nodiscard]] FrameTiming frameTiming() noexcept;

}  // namespace retropp
