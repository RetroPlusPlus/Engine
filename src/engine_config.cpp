#include "retropp/engine_config.h"

#include <SDL3/SDL_filesystem.h>  // SDL_GetBasePath — the one place the executable dir is consulted

#include "retropp/animation.h"       // AnimationPlayer::defaultTiming
#include "retropp/asset_registry.h"  // setConfigDefaultAssetPolicy, setAssetRoot
#include "retropp/renderer.h"        // Renderer::defaultViewport
#include "retropp/run_loop.h"        // RunLoop::defaultTiming

namespace retropp {

namespace {
// Resolve the configured asset root to an ABSOLUTE path. An absolute path is used as-is; a relative
// one (the default "assets") is joined onto the executable directory (SDL_GetBasePath — cached by SDL3,
// not freed). This is the single place the executable/base dir is consulted, per the PLAN: the loaders
// then resolve LoadFromPath assets against the fanned-out absolute root via assetPath().
std::filesystem::path resolveAssetRoot(const std::filesystem::path& configured) {
    if (configured.is_absolute()) return configured;
    const char* base = SDL_GetBasePath();
    if (base == nullptr) return configured;  // base dir unavailable → leave it relative to the cwd
    return std::filesystem::path(base) / configured;
}
}  // namespace

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
    // Asset embed policy (ENG-2.M.b): fan the engine-wide default policy out to the free runtime
    // default the loaders read, and resolve the asset root to an absolute path ONCE (here, the
    // SDL-coupled meeting point) so LoadFromPath assets resolve the same way everywhere via assetPath().
    detail::setConfigDefaultAssetPolicy(config.defaultAssetPolicy);
    setAssetRoot(resolveAssetRoot(config.assetRoot));
}

}  // namespace retropp
