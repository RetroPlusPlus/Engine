#include "retropp/engine_config.h"

#include "retropp/animation.h"   // AnimationPlayer::defaultTiming
#include "retropp/renderer.h"    // Renderer::defaultViewport
#include "retropp/run_loop.h"    // RunLoop::defaultTiming

namespace retropp {

// The single point where the SDL/GPU-coupled config layer and the locked SDL-free core loop meet —
// in a .cpp, in the config layer, with the data flowing DOWNWARD only: this reaches into the core's
// own static defaults to store plain SDL-free values (TimingProfile, ViewportResolution), so the core
// headers never reach UP into engine_config.h (which transitively pulls SDL via input_map.h). The
// values crossing the boundary are identical to what `RunLoop loop{clock, config.timing}` /
// `Renderer{..., config.viewport}` pass today — just stored as defaults instead of threaded per call.
//
// Consolidation: this also seeds AnimationPlayer::defaultTiming, so a game no longer needs the separate
// `AnimationPlayer::defaultTiming = loop.timing();` line — one startup call covers timing, viewport, and
// animation cadence.
void EngineConfig::setActive(const EngineConfig& config) {
    active                         = config;
    RunLoop::defaultTiming         = config.timing;
    Renderer::defaultViewport      = config.viewport;
    AnimationPlayer::defaultTiming = config.timing;
}

}  // namespace retropp
