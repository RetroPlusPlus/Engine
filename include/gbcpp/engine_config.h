#pragma once

#include <string>

#include "gbcpp/input_map.h"  // InputProfile (a value type; transitively includes SDL headers, which
                              // every build mode that compiles input_map already has)
#include "gbcpp/timing.h"     // TimingProfile
#include "gbcpp/viewport.h"   // ViewportResolution

namespace gbcpp {

// Window creation parameters — the host-OS window the platform opens. STARTUP-ONLY: the window is
// created once at platform construction. A user resize is a runtime OS event the renderer reacts to
// via drawableSize() (letterboxing), NOT a config change. The internal render viewport is a separate
// field (EngineConfig::viewport) — window size and viewport size are independent. Identity is fields.
struct WindowConfig {
    std::string title  = "GBCPP";
    int         width  = 160 * 4;  // initial window size only; the internal viewport is separate
    int         height = 144 * 4;
};

// Forward presentation-enhancement toggles — every user-opt-in enhancement, defaulted to the
// FAITHFUL baseline (OFF / identity) so a default config plays exactly like the original. These are
// FORWARD DECLARATIONS carried so the startup surface is stable across the enhancement phase; they
// are consumed by NOTHING yet. Output scaling + the native fullscreen toggle attach first; a world
// zoom factor, an audio-pack selection, and post-process filter selection are appended in their own
// later phases (additive — never a reshape). Identity is the named fields.
struct EnhancementToggles {
    bool fullscreen   = false;  // native OS fullscreen toggle
    int  integerScale = 0;      // 0 = auto fit-to-window with letterbox; N = force an N× integer scale
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
//   * STARTUP-ONLY (consumed once at construction): window, viewport, timing.
//   * RUNTIME-DYNAMIC: inputProfile + control bindings (setters on the platform), enhancements (a
//     plain mutable value today; the system that reacts to changes attaches with the enhancement phase).
struct EngineConfig {
    WindowConfig       window{};
    ViewportResolution viewport     = ViewportResolution::GameBoyColor;  // 160×144
    TimingProfile      timing       = TimingProfile::GameBoyColor;
    InputProfile       inputProfile = InputProfile::GameBoy;
    EnhancementToggles enhancements{};
};

}  // namespace gbcpp
