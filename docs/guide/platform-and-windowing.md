# Platform & windowing

The host-OS boundary: the window, the GPU device, the OS event pump, and the lifecycle. Everything
that touches the operating system lives behind one seam so the rest of the engine never depends on a
live device, and the startup configuration that drives it.

```cpp
#include "gbcpp/platform.h"        // Platform (the abstract seam)
#include "gbcpp/sdl_platform.h"    // SdlPlatform (the production implementation)
#include "gbcpp/windowed_host.h"   // WindowedHost (the hosted-mode driver)
#include "gbcpp/engine_config.h"   // EngineConfig, WindowConfig, EnhancementToggles
```

## The seam: `Platform`

```cpp
class Platform {
public:
    virtual void      pumpEvents()            = 0;  // drain OS events once per host iteration
    virtual bool      quitRequested() const   = 0;  // user asked to close the window
    virtual ButtonSet buttons()      const    = 0;  // held buttons as of the last pump
    virtual PixelSize drawableSize() const     = 0;  // window's physical pixel size, for letterboxing
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

    const ControlBindings& bindings() const noexcept;
    void setBindings(const ControlBindings&) noexcept;        // wholesale rebind (marks customized)

    const InputProfile& activeProfile() const noexcept;       // masks the sampled input
    void setActiveProfile(const InputProfile&) noexcept;

    ControllerType controllerType() const noexcept;           // detected pad family (Unknown if none)
};
```

The constructor initialises SDL (video + gamepad), creates the window from `config.window`, acquires
the GPU device, and claims the window for it; the destructor releases them in reverse order. It is
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
    std::string title  = "GBCPP";
    int         width  = 160 * 4;   // initial WINDOW size only — independent of the viewport
    int         height = 144 * 4;
};

struct EnhancementToggles {        // forward seams — defaulted OFF (faithful), consumed by nothing yet
    bool fullscreen   = false;     // native OS fullscreen toggle (planned)
    int  integerScale = 0;         // 0 = auto fit-to-window + letterbox; N = force N× (planned)
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

**Dynamic vs startup.** `window`, `viewport`, and `timing` are consumed once at construction
(startup-only). `inputProfile` + control bindings are runtime-dynamic (setters on the platform).
The `enhancements` toggles are forward declarations — carried so the startup surface is stable across
the enhancement phase, but consumed by nothing yet; output scaling + the native fullscreen toggle are
the first to attach (planned), and a world-zoom factor / audio-pack id / post-process filter are
appended in their own later phases (additive — never a reshape).

## Where to change things

- **Window title / initial size:** `EngineConfig::window`. The window size is independent of the
  internal render resolution — change the viewport (`EngineConfig::viewport`) to change what the game
  renders into, the window size to change the OS window only.
- **Target console's button set:** `EngineConfig::inputProfile` (see [input.md](input.md)).
- **Porting to a non-SDL platform:** implement the four-method `Platform` interface and hand your
  device/window to the `Renderer`; everything above the seam is unchanged.
- **Headless / automated testing:** drive a `MockPlatform` (in the test tree) — the `WindowedHost`
  loop runs identically with no real window.
