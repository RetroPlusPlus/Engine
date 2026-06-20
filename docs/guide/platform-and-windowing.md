# Platform & windowing

The host-OS boundary: the window, the GPU device, the OS event pump, frame pacing, and the lifecycle.
Everything that touches the operating system lives behind one seam so the rest of the engine never
depends on a live device — plus the startup configuration that drives it all.

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
    // Lifecycle + input, once per host iteration.
    virtual void        pumpEvents()           = 0;  // drain OS events
    virtual bool        quitRequested() const  = 0;  // user asked to close the window
    virtual ButtonSet   buttons()      const   = 0;  // held buttons as of the last pump
    virtual AnalogInput analog()       const   = 0;  // pointer/analog sample (cursor in viewport pixels)

    // Pointer modes.
    virtual void setPointerCaptured(bool) = 0;       // relative (spinner / mouse-look) capture
    virtual bool pointerCaptured() const  = 0;
    virtual void setCursorVisible(bool)   = 0;       // show/hide the OS cursor (independent of capture)
    virtual bool cursorVisible() const    = 0;

    // Window / display.
    virtual PixelSize drawableSize()      const = 0; // window's physical pixel size, for letterboxing
    virtual void      setWindowSize(PixelSize)  = 0; // resize the window (LOGICAL points)
    virtual PixelSize usableDisplaySize() const = 0; // display work area (logical), for scale clamping
    virtual void      setFullscreen(bool)       = 0; // enter/leave OS-native fullscreen
    virtual bool      isFullscreen() const      = 0;

    // Frame pacing (used by WindowedHost — see "Frame pacing" below).
    virtual std::chrono::nanoseconds nowMonotonic()         const = 0;
    virtual std::chrono::nanoseconds displayRefreshPeriod() const = 0;
    virtual void                     sleepPrecise(std::chrono::nanoseconds) = 0;
};
```

`Platform` is the host-OS abstraction — window + GPU present + input + pacing + lifecycle — expressed as
an interface so the engine's scheduling and input logic can run with **no live device**. It is to the
platform what the run loop's injectable `Clock` is to time: the same seam discipline. The production
implementation is `SdlPlatform`; tests drive a `MockPlatform`, which keeps the whole windowed-host
interleave (input, pacing included) verifiable headlessly.

No hardware-register or scanline idioms cross this boundary. Input is sampled held-button state (a
`ButtonSet`) plus an analog/pointer sample; a frame is presented whole. There are no per-line,
per-register, or mid-frame hooks.

**Input.** `buttons()` is the digital held state; `analog()` is the pointer/analog sample (cursor
mapped into **viewport pixels**, plus relative motion, wheel, mouse buttons, and gamepad sticks /
triggers). The two ride in parallel. `setPointerCaptured(true)` switches the pointer to relative
(spinner / mouse-look) mode — the OS cursor is hidden and confined and motion arrives as unbounded
deltas; there is no meaningful absolute cursor while captured. `setCursorVisible(false)` hides the OS
cursor **without** capturing (for a game that draws its own reticle while keeping absolute tracking
live) — an orthogonal knob. Full analog surface in [input.md](input.md).

## The production platform: `SdlPlatform`

```cpp
class SdlPlatform : public Platform {
public:
    explicit SdlPlatform(const EngineConfig& config = EngineConfig::active);

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

The constructor initialises SDL (video + gamepad + audio), **creates the window sized to
`viewport × enhancements.windowScale` logical points, clamped down to fit the display**, acquires the
GPU device, claims the window for it, and sets vsync present. The destructor releases them in reverse
order. It is single-threaded — every call runs on the platform thread.

The default constructor argument is `EngineConfig::active` (the set-once active config — see
[`EngineConfig`](#startup-configuration-engineconfig) below), so a bare `SdlPlatform platform;` opens
the window the host configured. With no `setActive()` call, `active` is the faithful Game Boy Color
baseline.

**It owns the window, device, and input — not the drawing.** Drawing is the `Renderer`'s job: the
renderer takes `device()` / `window()` and submits frames against them (see [rendering.md](rendering.md)).
The platform hands out the live device rather than hiding it behind a `present()` method, so the
renderer owns the pipeline and viewport independently. SDL types appear in this header by design (this
is the SDL platform).

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
    void run();   // pump → push input → advance → pace, until the platform requests quit
};
```

`WindowedHost` is the windowed replacement for the run loop's headless `run()`. It owns nothing and
depends only on the two abstractions — a `RunLoop` and a `Platform` — so the whole interleave is
unit-testable against a `MockPlatform` with no live window or GPU device.

Each iteration:

1. `pumpEvents()` — drain OS events.
2. push the platform's current input into the loop: `setRawInput(platform.buttons())` and
   `setRawAnalog(platform.analog())`.
3. `advance()` the simulation once. The present happens *inside* `advance()` via the consumer's render
   callback, so the run loop's "render once per advance with `alpha`" contract is preserved unchanged
   — the host owns only the scheduling.
4. pace to the next frame deadline (below).

The loop stops when the platform reports a quit request. A typical `main()` is just: construct config →
clock → loop → platform → renderer, wire the loop's tick/render callbacks, then
`WindowedHost{loop, platform}.run();`.

## Frame pacing

After each present, `WindowedHost` sleeps to a monotonic frame deadline spaced by the display's
current refresh period, so the loop runs at the monitor's cadence rather than relying on the vsync
present block to throttle it (not every platform reliably blocks on present while the window is idle).
When the present *does* block, the deadline is already reached and the computed sleep is ~0, so the two
compose rather than fight.

```cpp
struct FrameDeadline {
    std::chrono::nanoseconds nextDeadline;   // carry to the next iteration
    std::chrono::nanoseconds sleepFor;       // time to sleep now (never negative)
};

constexpr FrameDeadline nextFrameDeadline(std::chrono::nanoseconds prevDeadline,
                                          std::chrono::nanoseconds period,
                                          std::chrono::nanoseconds now,
                                          int maxLagPeriods = 4) noexcept;
```

`nextFrameDeadline` (in [pacing.h](run-loop-and-timing.md)) is **pure** — no clock, no sleep, no SDL —
so the pacing decision is unit-testable. The OS-coupled half is three `Platform` methods:
`nowMonotonic()` (current monotonic time, same clock domain as the sleep), `displayRefreshPeriod()`
(queried live each iteration, so dragging the window to a different-refresh monitor re-paces with no
event handling; 60 Hz fallback), and `sleepPrecise()` (high-resolution sleep, no-op when ≤ 0). If the
loop falls more than `maxLagPeriods` behind, the deadline resyncs to now and the backlog is dropped, so
recovery from a stall doesn't fast-forward. This is presentation pacing only; the sim's own catch-up
clamp is `kMaxFrameTime` (see [run-loop-and-timing.md](run-loop-and-timing.md)).

## Startup configuration: `EngineConfig`

```cpp
struct WindowConfig {
    std::string title = "Retro++";   // title only — the window SIZE comes from windowScale × viewport
};

struct EnhancementToggles {        // faithful defaults at a sensible window size
    int          windowScale = 4;                      // window = viewport × this (LOGICAL points),
                                                       // clamped down to fit the display
    bool         fullscreen  = false;                  // native OS fullscreen toggle
    SamplingMode sampling    = SamplingMode::Nearest;  // blit sampler: Nearest (faithful) / Bilinear
};

struct EngineConfig {
    WindowConfig       window{};
    ViewportResolution viewport     = ViewportResolution::GameBoyColor;  // 160×144 internal
    TimingProfile      timing       = TimingProfile::GameBoyColor;
    InputProfile       inputProfile = InputProfile::GameBoy;
    EnhancementToggles enhancements{};

    static EngineConfig active;                          // the set-once active config
    static void setActive(const EngineConfig& config);   // store it + seed engine defaults
};
```

`EngineConfig` is the single value bundle a host hands the engine at startup — window + internal
viewport + render timing + active controller profile + enhancement toggles. It holds
**platform-agnostic value types only** (no live device handles): the platform reads `window` +
`inputProfile`, the renderer reads `viewport`, the run loop reads `timing`.

Every field defaults to the faithful Game Boy Color baseline, so a default-constructed `EngineConfig{}`
reproduces the original behaviour — override only what you mean to change:

```cpp
EngineConfig config{ .window = {.title = "My Game"} };  // everything else = faithful GBC
```

**`setActive` — the one-call startup.** `EngineConfig::setActive(config)` stores the config as
`EngineConfig::active` **and** seeds the engine's per-type defaults from it, so the bare engine
constructors inherit the configuration without you threading fields to each one:

```cpp
EngineConfig::setActive(config);   // do this once at startup
SdlPlatform platform;              // reads EngineConfig::active (window + inputProfile)
RunLoop     loop{clock};           // uses the timing setActive seeded
Renderer    renderer{platform.device(), platform.window()};  // uses the viewport setActive seeded
```

Threading the fields explicitly still works and overrides the defaults per object —
`SdlPlatform{config}`, `RunLoop{clock, config.timing}`, `Renderer{dev, win, config.viewport}` — so a
program that needs two differently-configured objects can still construct them directly.

**Dynamic vs startup.** `viewport` and `timing` are consumed once at construction (startup-only).
`inputProfile` + control bindings are runtime-dynamic (setters on the platform). The `enhancements`
toggles are read at startup (`windowScale` sizes the initial window in the platform ctor; `sampling`
feeds the renderer's setter) and then driven live at runtime — `setWindowSize` / `setFullscreen` on
the platform, `setSamplingMode` on the renderer.

## Where to change things

- **One-call startup:** build an `EngineConfig` and call `EngineConfig::setActive(config)` before
  constructing the engine objects; bare ctors inherit it.
- **Window title:** `EngineConfig::window.title`. The window *size* is `enhancements.windowScale ×
  viewport` (clamped to the display), not a `WindowConfig` field — change the scale, or the viewport
  (`EngineConfig::viewport`) to change what the game renders into.
- **Target console's button set:** `EngineConfig::inputProfile` (see [input.md](input.md)).
- **Presentation scale / fullscreen / sampling:** start from `EngineConfig::enhancements`
  (`windowScale` / `fullscreen` / `sampling`); toggle live via `SdlPlatform::setWindowSize` (size to
  `viewport × N`, clamp with `fitWindowScale` + `usableDisplaySize()`), `setFullscreen`, and the
  renderer's `setSamplingMode` (details in [rendering.md](rendering.md)).
- **Pointer / cursor:** `setPointerCaptured` for relative (spinner / mouse-look) mode,
  `setCursorVisible` to hide the OS cursor while still tracking it; read the pointer via `analog()`
  ([input.md](input.md)).
- **Porting to a non-SDL platform:** implement the `Platform` interface (the full virtual surface
  above — input, window/display, and the three pacing methods) and hand your device/window to the
  `Renderer`; everything above the seam is unchanged.
- **Headless / automated testing:** drive a `MockPlatform` (in the test tree) — the `WindowedHost`
  loop, pacing included, runs identically with no real window.
