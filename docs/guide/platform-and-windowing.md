# Platform & windowing

The host-OS boundary: the window, the GPU device, the OS event pump, and the lifecycle. Everything
that touches the operating system lives behind one seam so the rest of the engine never depends on a
live device, and the startup configuration that drives it.

```cpp
#include "retropp/platform.h"        // Platform (the abstract seam)
#include "retropp/sdl_platform.h"    // SdlPlatform (the production implementation)
#include "retropp/windowed_host.h"   // WindowedHost (the hosted-mode driver)
#include "retropp/engine_config.h"   // EngineConfig, WindowConfig, EnhancementToggles
```

## The seam: `Platform`

```cpp
class Platform {
public:
    virtual void      pumpEvents()            = 0;  // drain OS events once per host iteration
    virtual bool      quitRequested() const   = 0;  // user asked to close the window
    virtual ButtonSet buttons()      const    = 0;  // held buttons as of the last pump
    virtual PixelSize drawableSize() const     = 0;  // window's physical pixel size, for letterboxing
    virtual void      setWindowSize(PixelSize) = 0;  // resize the window (LOGICAL points)
    virtual PixelSize usableDisplaySize() const= 0;  // display work area (logical), for scale clamping
    virtual void      setFullscreen(bool)      = 0;  // enter/leave OS-native fullscreen
    virtual bool      isFullscreen() const     = 0;
};
```

`Platform` is the host-OS abstraction — window + GPU present + input + lifecycle — expressed as an
interface so the engine's scheduling and input logic can run with **no live device**. It is the
platform analog of the run loop's injectable `Clock`: the same seam discipline applied to the platform
instead of to time. The production implementation is `SdlPlatform`; tests drive a `MockPlatform`,
which keeps the whole windowed-host interleave verifiable headlessly.

No hardware-register or scanline idioms cross this boundary. Input is sampled held-button state (a
`ButtonSet`); a frame is presented whole. There are no per-line, per-register, or mid-frame hooks.

## The production platform: `SdlPlatform`

```cpp
class SdlPlatform : public Platform {
public:
    explicit SdlPlatform(const EngineConfig& config = {});   // default = faithful GBC baseline

    SDL_GPUDevice* device() const noexcept;   // the live device the Renderer draws with
    SDL_Window*    window() const noexcept;

    void      setWindowSize(PixelSize size) override;   // resize to N× the viewport (logical points)
    PixelSize usableDisplaySize() const override;       // the window's display's usable area (logical)
    void      setFullscreen(bool enabled) override;     // native fullscreen (a real macOS Space)
    bool      isFullscreen() const override;

    const ControlBindings& bindings() const noexcept;
    void setBindings(const ControlBindings&) noexcept;        // wholesale rebind (marks customized)

    const InputProfile& activeProfile() const noexcept;       // masks the sampled input
    void setActiveProfile(const InputProfile&) noexcept;

    ControllerType controllerType() const noexcept;           // detected pad family (Unknown if none)
};
```

The constructor initialises SDL (video + gamepad), **creates the window sized to
`viewport × enhancements.windowScale` logical points, clamped down to fit the display**, acquires the
GPU device, and claims the window for it; the destructor releases them in reverse order. It is
single-threaded — every call runs on the platform thread.

**It owns the window, device, and input — not the drawing.** Drawing is the `Renderer`'s job: the
renderer takes `device()` / `window()` and submits frames against them (see [rendering.md](rendering.md)).
This open-internals split — the platform hands out the live device rather than hiding it behind a
`present()` method — is deliberate, so the renderer can own the pipeline and viewport independently of
the platform.
SDL types appear in this header by design (this is the SDL platform).

**Input masking.** The active `InputProfile` (seeded from `config.inputProfile`) masks the sampled
input: the platform only ever reports the buttons that profile exposes — a Game Boy profile never
reports X/Y/L/R even on a pad that has them. See [input.md](input.md).

**On-connect controller defaults.** When a pad connects, the platform detects its family
(`controllerType()`) and — *unless the host has called `setBindings()`* — applies that family's
default gamepad layout via `ControlBindings::defaultsForGamepad(family)`. For a Nintendo pad that
swaps A/B (and X/Y) to the Nintendo-labelled positions so a Switch player's labelled **A** confirms;
Xbox / PlayStation / Standard stay positional. A host or user rebind (`setBindings`) sets an internal
"customized" flag that suppresses this auto-apply, so plugging in a different pad never clobbers a
custom mapping; a disconnect reverts symmetrically.

**Window scale.** The presentation scale is *window size*, expressed as an integer multiple of the
viewport in **logical points**: the window opens at `viewport × enhancements.windowScale` (default
**4×**), and the renderer auto-fills it crisply. Logical points (not physical pixels) keep the
perceived size the same on any display density — "4×" reads the same on a Retina laptop and a 1080p
monitor. The size is always **clamped down to fit the usable display** (`fitWindowScale` in
[geometry.h](rendering.md), fed by `usableDisplaySize()`), so even a large viewport never opens a
window bigger than the screen — it steps down to the nearest ratio that fits (floor 1×). At runtime,
`setWindowSize(viewport × N)` resizes to a new scale; a settings menu computes the clamped `N` the
same way and calls it.

**Native fullscreen.** `setFullscreen(true)` puts the window into the host's *real* fullscreen
affordance — on macOS a fullscreen Space, elsewhere a borderless desktop fill — via
`SDL_SetWindowFullscreen` (the platform-native idiom, not a fake borderless window). It does **not**
make the window freely resizable; the renderer's fill blit absorbs the new target size with no
renderer change. `EngineConfig::enhancements.fullscreen` is applied once at construction (default
windowed), and the toggle is runtime-dynamic thereafter. `isFullscreen()` reports the current state.

**High-DPI.** The window is created with `SDL_WINDOW_HIGH_PIXEL_DENSITY`, so on a Retina/HiDPI display
its drawable is the true physical pixel resolution. `drawableSize()` reports physical pixels and the
renderer fills in physical pixels, so the art renders crisp at native resolution automatically (the
fill just picks a larger integer scale). See [rendering.md](rendering.md).

## The hosted-mode driver: `WindowedHost`

```cpp
class WindowedHost {
public:
    WindowedHost(RunLoop& loop, Platform& platform) noexcept;
    void run();   // pump → push input → advance, until the platform requests quit
};
```

`WindowedHost` is the windowed replacement for the run loop's headless `run()`. It owns nothing and
depends only on the two abstractions — a `RunLoop` and a `Platform` — so the whole pump → push-input
→ advance interleave is unit-testable against a `MockPlatform` with no live window or GPU device.

Each iteration: drain OS events, push the platform's current held-button state into the loop
(`setRawInput`), then `advance()` the simulation once. The present happens *inside* `advance()` via
the consumer's render callback, so the run loop's "render once per advance with `alpha`" contract is
preserved unchanged — the host owns only the scheduling. The loop stops when the platform reports a
quit request. A typical `main()` is just: construct config → clock → loop → platform → renderer, wire
the loop's tick/render callbacks, then `WindowedHost{loop, platform}.run();`.

## Startup configuration: `EngineConfig`

```cpp
struct WindowConfig {
    std::string title = "Retro++";   // title only — the window SIZE comes from windowScale × viewport
};

struct EnhancementToggles {        // faithful baseline at a sensible default size
    int          windowScale = 4;                      // window = viewport × this (LOGICAL points),
                                                       // clamped down to fit the display
    bool         fullscreen  = false;                  // native OS fullscreen toggle
    SamplingMode sampling    = SamplingMode::Nearest;  // blit sampler: Nearest (faithful) / Bilinear
    // world zoom factor, audio-pack id, post-process filter selection: appended in their own phases
};

struct EngineConfig {
    WindowConfig       window{};
    ViewportResolution viewport     = ViewportResolution::GameBoyColor;  // 160×144 internal
    TimingProfile      timing       = TimingProfile::GameBoyColor;
    InputProfile       inputProfile = InputProfile::GameBoy;
    EnhancementToggles enhancements{};
};
```

`EngineConfig` is the single value bundle a host hands the engine at startup — window + internal
viewport + render timing + active controller profile + forward enhancement toggles. It holds
**platform-agnostic value types only** (no live device handles): the platform reads `window` +
`inputProfile`, the renderer reads `viewport`, the run loop reads `timing`. It is a passive struct —
there is no facade — so a consumer threads its fields into the existing constructors
(`SdlPlatform{config}`, `Renderer{dev, win, config.viewport}`, `RunLoop{clock, config.timing}`).

Every field defaults to the faithful Game Boy Color baseline, so a default-constructed
`EngineConfig{}` reproduces the original behaviour — override only what you mean to change:

```cpp
const EngineConfig config{ .window = {.title = "My Game"} };  // everything else = faithful GBC
```

**Dynamic vs startup.** `viewport` and `timing` are consumed once at construction (startup-only).
`inputProfile` + control bindings are runtime-dynamic (setters on the platform). The `enhancements`
toggles are read at startup (`windowScale` sizes the initial window in the platform ctor; `sampling`
feeds the renderer's setter) and then driven live at runtime — `setWindowSize` / `setFullscreen` on
the platform, `setSamplingMode` on the renderer. A world-zoom factor / audio-pack id / post-process
filter are appended to `EnhancementToggles` in their own later phases (additive — never a reshape).

## Where to change things

- **Window title:** `EngineConfig::window.title`. The window *size* is `enhancements.windowScale ×
  viewport` (clamped to the display), not a `WindowConfig` field — change the scale, or the viewport
  (`EngineConfig::viewport`) to change what the game renders into.
- **Target console's button set:** `EngineConfig::inputProfile` (see [input.md](input.md)).
- **Presentation scale / fullscreen / sampling:** start from `EngineConfig::enhancements`
  (`windowScale` / `fullscreen` / `sampling`); toggle live via `SdlPlatform::setWindowSize` (size to
  `viewport × N`, clamp with `fitWindowScale` + `usableDisplaySize()`), `setFullscreen`, and the
  renderer's `setSamplingMode` (details in [rendering.md](rendering.md)).
- **Porting to a non-SDL platform:** implement the `Platform` interface (`pumpEvents`,
  `quitRequested`, `buttons`, `drawableSize`, `setWindowSize`, `usableDisplaySize`, `setFullscreen`,
  `isFullscreen`) and hand your device/window to the `Renderer`; everything above the seam is unchanged.
- **Headless / automated testing:** drive a `MockPlatform` (in the test tree) — the `WindowedHost`
  loop runs identically with no real window.
