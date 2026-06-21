#include "retropp/engine_config.h"

#include <SDL3/SDL_filesystem.h>  // SDL_GetBasePath — the one place the executable dir is consulted

#include "retropp/animation.h"        // AnimationPlayer::defaultTiming
#include "retropp/asset_registry.h"   // setConfigDefaultAssetPolicy, setAssetRoot
#include "retropp/renderer.h"         // Renderer::defaultViewport
#include "retropp/routine_registry.h" // setConfigDefaultRoutinePolicy
#include "retropp/run_loop.h"         // RunLoop::defaultTiming

namespace retropp {

namespace {
// Resolve the configured asset root to an ABSOLUTE path. An absolute path is used as-is; a relative
// one (empty by default → the executable directory itself) is joined onto the executable directory
// (SDL_GetBasePath — cached by SDL3, not freed). This is the single place the executable/base dir is
// consulted: the loaders then resolve LoadFromPath assets against the fanned-out absolute root via
// assetPath().
std::filesystem::path resolveAssetRoot(const std::filesystem::path& configured) {
    if (configured.is_absolute()) return configured;
    const char* base = SDL_GetBasePath();
    if (base == nullptr) return configured;  // base dir unavailable → leave it relative to the cwd
    return std::filesystem::path(base) / configured;
}
}  // namespace

// The single point where the SDL/GPU-coupled config layer and the SDL-free core loop meet — in a .cpp,
// in the config layer, with the data flowing DOWNWARD only: this reaches into the core's own static
// defaults to store plain SDL-free values (TimingProfile, ViewportResolution), so the core headers
// never reach UP into engine_config.h (which transitively pulls SDL via input_map.h). The values
// crossing the boundary are the same ones `RunLoop loop{clock, config.timing}` / `Renderer{...,
// config.viewport}` take by argument — stored as defaults instead of threaded per call.
//
// This also seeds AnimationPlayer::defaultTiming, so one startup call covers timing, viewport, and
// animation cadence (a game need not set AnimationPlayer::defaultTiming separately).
void EngineConfig::setActive(const EngineConfig& config) {
    active                         = config;
    RunLoop::defaultTiming         = config.timing;
    Renderer::defaultViewport      = config.viewport;
    AnimationPlayer::defaultTiming = config.timing;
    // Asset embed policy: fan the default policy out to the runtime default the loaders read, and
    // resolve the asset root to an absolute path ONCE (here, the SDL-coupled meeting point) so
    // LoadFromPath assets resolve the same way everywhere via assetPath().
    detail::setConfigDefaultAssetPolicy(config.defaultAssetPolicy);
    setAssetRoot(resolveAssetRoot(config.assetRoot));
    // Routine / chiptune embed-policy default: fan the routine policy default out to the loaders'
    // runtime default. There is no separate routine root — LoadFromPath routines resolve against the
    // same assetRoot() set just above.
    detail::setConfigDefaultRoutinePolicy(config.defaultRoutinePolicy);
}

}  // namespace retropp
