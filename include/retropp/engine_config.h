#pragma once

#include <string>

#include "retropp/input_map.h"  // InputProfile (a value type; transitively includes SDL headers, which
                              // every build mode that compiles input_map already has)
#include "retropp/output.h"     // SamplingMode
#include "retropp/timing.h"     // TimingProfile
#include "retropp/viewport.h"   // ViewportResolution

namespace retropp {

// Window creation parameters — the host-OS window the platform opens. The window SIZE is not set
// here: it is derived from the viewport and the presentation scale (EnhancementToggles::windowScale)
// so the window is always an integer multiple of the game's native resolution, clamped to the
// display — see EnhancementToggles below. This struct carries only the title. Identity is fields.
struct WindowConfig {
    std::string title = "Retro++";
};

// Presentation-enhancement settings — every user-opt-in enhancement, defaulted so a default config
// plays exactly like the original (faithful colour/sampling, windowed) at a sensible default size.
// `fullscreen` + `sampling` default OFF/identity (faithful); `windowScale` is the one sizing field.
// A world zoom factor, an audio-pack selection, and post-process filter selection are appended in
// their own later phases (additive — never a reshape). Identity is the named fields.
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
// baseline, so a default-constructed EngineConfig reproduces the original behaviour. Identity is the
// named fields.
//
// Dynamic vs startup:
//   * STARTUP-ONLY (consumed once at construction): window title, viewport, timing.
//   * RUNTIME-DYNAMIC: inputProfile + control bindings (setters on the platform); enhancements —
//     windowScale seeds the initial window size and is then re-applied live via Platform::setWindowSize,
//     fullscreen via setFullscreen, sampling via Renderer::setSamplingMode.
struct EngineConfig {
    WindowConfig       window{};
    ViewportResolution viewport     = ViewportResolution::GameBoyColor;  // 160×144
    TimingProfile      timing       = TimingProfile::GameBoyColor;
    InputProfile       inputProfile = InputProfile::GameBoy;
    EnhancementToggles enhancements{};
};

}  // namespace retropp
