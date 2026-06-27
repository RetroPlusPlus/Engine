#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "retropp/asset_policy.h"  // AssetPolicy (the embed/load default below)
#include "retropp/input_map.h"  // InputProfile (a value type; transitively includes SDL headers, which
                              // every build mode that compiles input_map already has)
#include "retropp/output.h"     // SamplingMode
#include "retropp/timing.h"     // TimingProfile
#include "retropp/viewport.h"   // ViewportResolution

namespace retropp {

// Window creation parameters — the host-OS window the platform opens. The window SIZE is not set
// here: it is derived from the viewport and the presentation scale (EnhancementToggles::windowScale)
// so the window is always an integer multiple of the game's native resolution, clamped to the
// display — see EnhancementToggles below. This struct carries only the title.
struct WindowConfig {
    std::string title = "Retro++";
};

// Presentation-enhancement settings — every user-opt-in enhancement, defaulted so a default config
// plays exactly like the original (faithful colour/sampling, windowed) at a sensible default size.
// `fullscreen` + `sampling` default OFF/identity (faithful); `windowScale` is the one sizing field.
// A world zoom factor, an audio-pack selection, and post-process filter selection are appended
// later (additive — never a reshape).
struct EnhancementToggles {
    int          windowScale = 4;                       // window = viewport × this (LOGICAL points),
                                                        // clamped down to fit the display (fitWindowScale)
    bool         fullscreen  = false;                   // native OS fullscreen toggle
    SamplingMode sampling    = SamplingMode::Nearest;   // blit sampler: Nearest (faithful) / Bilinear
    // world zoom factor, audio-pack id, post-process filter selection: appended when their phase lands
};

// The single startup configuration the host hands the engine: window + internal viewport + render
// timing + active controller profile + forward enhancement toggles. Platform-agnostic VALUE types
// only (no live device handles): the platform layer reads `window` + `inputProfile`; the renderer
// reads `viewport`; the run loop reads `timing`. Every field defaults to the faithful Game Boy Color
// baseline, so a default-constructed EngineConfig reproduces the original behaviour.
//
// Dynamic vs startup:
//   * STARTUP-ONLY (consumed once at construction): window title, viewport, timing.
//   * RUNTIME-DYNAMIC: inputProfile + control bindings (setters on the platform); enhancements —
//     windowScale seeds the initial window size and is then re-applied live via Platform::setWindowSize,
//     fullscreen via setFullscreen, sampling seeded from config by setActive() (so the call site need not
//     apply it) and overridable at runtime via Renderer::setSamplingMode.
struct EngineConfig {
    WindowConfig       window{};
    ViewportResolution viewport     = ViewportResolution::GameBoyColor;  // 160×144
    TimingProfile      timing       = TimingProfile::GameBoyColor;
    InputProfile       inputProfile = InputProfile::GameBoy;
    EnhancementToggles enhancements{};

    // Asset embed policy (see assets-and-embedding.md). `defaultAssetPolicy` is the default for whether an
    // ingestible asset is baked into the binary (Embed) or read from disk at runtime (LoadFromPath);
    // nullopt (the default) = fall through to each loader's per-type default (loadAtlas → LoadFromPath,
    // loadMapPng → Embed). It rides the setActive fan-out into the free runtime default the loaders read.
    // `assetRoot` is the runtime base directory LoadFromPath assets resolve against (via assetPath());
    // setActive resolves it to an ABSOLUTE path once — against the executable directory — and fans it
    // out. Default empty = the executable directory itself, which is where the build copies LoadFromPath
    // assets (at their logical path), so the build and the runtime agree with no configuration. A
    // default-constructed EngineConfig forces no embedding (both fields nullopt / empty).
    std::optional<AssetPolicy> defaultAssetPolicy{};
    std::filesystem::path      assetRoot{};

    // VM-routine + chiptune embed-policy default (see vm-and-routines.md / audio.md). Separate from defaultAssetPolicy because the
    // per-type defaults differ — a routine / chiptune driver defaults to Embed (hundreds of bytes of
    // clean-room code), an atlas to LoadFromPath. nullopt = fall through to that per-type default. There
    // is NO separate routine root: a LoadFromPath routine path is a full project-root-relative literal
    // resolved against `assetRoot` above, exactly like an atlas. A default-constructed EngineConfig
    // leaves this unset (nullopt).
    std::optional<AssetPolicy> defaultRoutinePolicy{};

    // The set-once active config: the host assigns it once via setActive() (below), and bare engine
    // ctors then inherit from it instead of every field being threaded to every ctor — RunLoop and
    // Renderer read the per-type static defaults setActive() fans the config out into, and SdlPlatform
    // reads `active` directly (it takes the whole config — window + inputProfile). Per-ctor override
    // stays available. A default-constructed `active` is the faithful Game Boy Color baseline. Self-
    // typed static: declared here (the type is incomplete in-class), defined `inline` just below the
    // struct.
    static EngineConfig active;

    // Assign `active = config` AND fan the config out into the per-type SDL-free static defaults
    // (RunLoop::defaultTiming, Renderer::defaultViewport, Renderer::defaultSamplingMode,
    // AnimationPlayer::defaultTiming, TweenPlayer<T>::defaultTiming) so bare ctors inherit them. One call
    // at startup. Defined in src/engine_config.cpp — keeping the definition out of
    // this header avoids pulling renderer.h (SDL_gpu) / run_loop.h / animation.h in, so this header stays
    // light and SDL/GPU-free, and it quarantines the one point where the SDL-coupled config layer and the
    // locked SDL-free core loop meet (data flows downward only).
    static void setActive(const EngineConfig& config);
};

// Out-of-class definition: a complete type is required for a static data member definition, so this
// cannot live inside the struct body (where EngineConfig is still incomplete). `inline` makes it a
// single definition across translation units (C++17). Same idiom as TimingProfile::GameBoyColor.
inline EngineConfig EngineConfig::active{};

}  // namespace retropp
