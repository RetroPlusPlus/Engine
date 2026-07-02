#include "retropp/engine_config.h"

#include <SDL3/SDL_filesystem.h>  // SDL_GetBasePath — the one place the executable dir is consulted

#include "retropp/animation.h"        // AnimationPlayer::defaultTiming
#include "retropp/asset_registry.h"   // setAssetRoot
#include "retropp/renderer.h"         // Renderer::defaultViewport
#include "retropp/run_loop.h"         // RunLoop::defaultTiming
#include "retropp/tween.h"            // TweenPlayer<T>::defaultTiming (the interpolable T's)

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
// This also seeds the playback-cadence defaults — AnimationPlayer::defaultTiming and every interpolable
// TweenPlayer<T>::defaultTiming — and the blit sampling default, so one startup call covers timing,
// viewport, sampling, animation cadence, and tween cadence (a game need not set any of them separately).
void EngineConfig::setActive(const EngineConfig& config) {
    active                         = config;
    RunLoop::defaultTiming         = config.timing;
    Renderer::defaultViewport      = config.viewport;
    Renderer::defaultSamplingMode  = config.enhancements.sampling;
    Renderer::defaultInterpolation = config.interpolation;
    Renderer::defaultEvaluationGrid = config.evaluationGrid;
    AnimationPlayer::defaultTiming = config.timing;
    // TweenPlayer<T>::defaultTiming is a per-T template static, so the fan-out seeds each of the engine's
    // interpolable tween types (the T's that lerp() supports). A game using only some of them still gets
    // every one seeded — harmless, and it means a bare TweenPlayer<Vec3> inherits the cadence too.
    TweenPlayer<float>::defaultTiming = config.timing;
    TweenPlayer<Vec2>::defaultTiming  = config.timing;
    TweenPlayer<Vec3>::defaultTiming  = config.timing;
    TweenPlayer<Vec4>::defaultTiming  = config.timing;
    // Asset root: resolve to an absolute path ONCE (here, the SDL-coupled meeting point) so LoadFromPath
    // assets resolve the same way everywhere via assetPath(). There is no separate routine root —
    // LoadFromPath routines resolve against this same assetRoot(). Embed-vs-load carries no global
    // default: it is the per-call AssetPolicy argument > the loader's per-type default.
    setAssetRoot(resolveAssetRoot(config.assetRoot));
}

}  // namespace retropp
