# Rendering

The `Renderer` object: the internal viewport it draws into, how that viewport is scaled and
letterboxed onto the window, the once-per-frame submission entry point, the amortized atlas/palette
uploads, and how shaders reach the GPU. The *content* model a frame carries (layers, tiles, sprites,
colour) is [draw-state.md](draw-state.md) + [tiles-and-colour.md](tiles-and-colour.md); this page is
the object and the output path.

```cpp
#include "retropp/renderer.h"       // Renderer
#include "retropp/viewport.h"       // ViewportResolution
#include "retropp/geometry.h"       // PixelSize, IntRect, integerScaleToFitRect, fitWindowScale
#include "retropp/output.h"         // SamplingMode
#include "retropp/shader_format.h"  // ShaderVariants, selectShader (internal plumbing)
```

## The model

The renderer draws every frame into an **offscreen internal viewport** at a fixed retro resolution
(160×144 by default), then **blits that viewport onto the window** at the largest integer scale that
fits, centred, with letterbox/pillarbox bars filling any leftover. Game content is authored once at
the small native resolution; the renderer always fills whatever window it's given, crisply. This
two-stage path (render small → fill window) is what keeps pixels square at any window size. The
presentation *size* — how big that window is — is the window-scale concern owned by the platform
(`EngineConfig::enhancements.windowScale` + `Platform::setWindowSize`, see
[platform-and-windowing.md](platform-and-windowing.md)); the renderer's only output knob is
**sampling** (nearest/bilinear, below). Post-process display filters (CRT and friends) are a later
stage (planned — see the Coverage table in the [guide index](README.md)).

The renderer is constructed from a live device + window (handed out by `SdlPlatform`) — drawing is
the renderer's job, the platform owns the window/device/input.

```cpp
class Renderer {
public:
    Renderer(SDL_GPUDevice* device, SDL_Window* window,
             ViewportResolution viewport = {});   // default 160×144

    AtlasId   uploadAtlas(const std::uint8_t* indices, int width, int height,
                          int transparentIndex = -1);
    PaletteId uploadPalette(std::span<const Rgba8> colors);

    PostProcessStageId registerPostProcessStage(const ShaderVariants& fragment,
                                                std::uint32_t uniformSize);  // custom shader hook

    void renderFrame(const FrameDrawState& frame, float alpha);

    void setLayerCollisionPolicy(LayerKeyCollisionPolicy) noexcept;
    LayerKeyCollisionPolicy layerCollisionPolicy() const noexcept;

    void         setSamplingMode(SamplingMode) noexcept;   // blit sampler: Nearest / Bilinear
    SamplingMode samplingMode() const noexcept;
};
```

## The internal viewport: `ViewportResolution`

```cpp
struct ViewportResolution {
    int width = 160, height = 144;
    static const ViewportResolution GameBoy, GameBoyColor, GameBoyAdvance,
                                    Nes, Snes, Genesis, MasterSystem;
};
```

The internal render resolution defaults to the original Game Boy 160×144 (the faithful baseline) and
is configurable so a game can request a larger internal viewport — e.g. a wider visible world for a
zoom-out feature — without the engine assuming a fixed size. Common-platform resolutions are named
presets (the self-type-constant idiom shared with `PaletteSize` / `TimingProfile`); a resolution **is**
a `{width, height}` tuple, so a preset and a raw value are interchangeable:

```cpp
Renderer{dev, win, ViewportResolution::Nes};   // a preset (256×240)
Renderer{dev, win, {256, 224}};                // or any raw {width, height}
```

It is not an exhaustive registry — add platforms as needed. The engine generalizes beyond the Game
Boy, so a fixed resolution baked into the type would be the hardcoded-dimensions mistake the project
avoids elsewhere.

## Filling the window: `integerScaleToFitRect`

```cpp
IntRect integerScaleToFitRect(PixelSize drawable, PixelSize viewport) noexcept;  // geometry.h
```

Each frame the renderer reads the window's `drawableSize()` and fills it at the **largest integer
multiple of the viewport that fits**, centred, with the leftover split into letterbox/pillarbox bars
(a pure, GPU-independent, unit-tested helper). It tracks resizes, so the content follows the window
live. This is *fill*, not *size* — the renderer crisply fills whatever window it's handed. Choosing
how big that window is (the presentation scale) is `windowScale` on the platform side; see
[platform-and-windowing.md](platform-and-windowing.md). The two compose: the window is sized to an
integer multiple of the viewport, and the renderer then fills it exactly with no bars.

## Sampling: `SamplingMode`

```cpp
enum class SamplingMode { Nearest, Bilinear };                     // output.h
void         Renderer::setSamplingMode(SamplingMode) noexcept;     // runtime-dynamic
SamplingMode Renderer::samplingMode() const noexcept;
```

`Nearest` (the default) is point sampling — crisp, square pixels, the faithful look. `Bilinear`
smooths the upscale. This is a blit **sampler** swap, not a shader change: the renderer builds both
samplers up front and binds the one the current mode selects. The tile/atlas path always samples
nearest; only the final viewport→window blit honours the mode. The default (`Nearest`) reproduces the
faithful baseline value-for-value; a consumer reads `EngineConfig::enhancements.sampling` and calls
the setter, then toggles it live from a settings menu.

**Native fullscreen** is a `Platform`-seam concern, not a renderer one — see
[platform-and-windowing.md](platform-and-windowing.md). The fill blit absorbs the fullscreen target
size with no renderer change. **High-DPI** is automatic: the window opts into
`SDL_WINDOW_HIGH_PIXEL_DENSITY`, `drawableSize()` reports physical pixels, and the fill picks the
larger integer scale so the art renders crisp at native resolution.

## Per-frame submission: `renderFrame`

```cpp
void renderFrame(const FrameDrawState& frame, float alpha);
```

Call this once per render callback. The game hands a whole `FrameDrawState` (the Z-sorted layer
stack — see [draw-state.md](draw-state.md)); the renderer composites the layers back-to-front into
the offscreen viewport — applying any **per-layer screen-space effects** (`DrawLayer::effect`,
`Layer` / `Below` scope) as it goes — then runs the **frame-level post-process chain** (`postEffects`,
below), applies the frame-level colour transform, and blits the viewport integer-scaled + letterboxed
onto the swapchain and presents. `alpha` is the run loop's interpolation factor. There is **no
mid-frame state-change API** — a frame is computed whole and submitted whole, every frame. A frame with
no per-layer effects composites exactly as before — the per-layer path is paid for only where used.

## Post-process effects: `postEffects`

After compositing, the renderer runs `FrameDrawState::postEffects` — a list of screen-space effects
applied to the **whole composited viewport** before the blit. Each effect is one full-viewport pass;
the renderer ping-pongs two internal scratch targets so any number of effects chain in submission
order. An **empty list is the faithful baseline** — the blit samples the composited viewport
directly, byte-identical to no chain. Any effect (here or per-layer, built-in or custom) can be
**confined to a shape** via `ScreenSpaceEffect::region` — the renderer gates it with an extra select
pass that leaves the rest of the image untouched; see
[draw-state.md](draw-state.md#confining-an-effect-to-a-shape-region).

The first engine effect is **row displacement** (wavy water / heat haze / per-line scroll) with a
developer-selectable frame-edge (`Blank` default / `Stretch`). The effect type, its scopes
(frame-level here vs. per-layer), and the edge choice are documented in
[draw-state.md](draw-state.md#screen-space-effects). This is a *content* effect declared on the draw
state — distinct from output-side display filters (CRT/scanlines), which are a separate planned stage
(below).

## Custom shader stages: `registerPostProcessStage`

```cpp
PostProcessStageId registerPostProcessStage(const ShaderVariants& fragment, std::uint32_t uniformSize);
```

When the built-in effect vocabulary stops, a game registers its **own fragment shader** and uses it as
a first-class screen-space effect — at either attachment point (`postEffects` or `DrawLayer::effect`),
composing with the built-ins in submission order. Register once at load time (builds the pipeline pair),
then per frame attach a `ScreenSpaceEffect{ .kind = Custom, .customShader = <handle>, .uniform = … }`;
the per-frame submission shape is in [draw-state.md](draw-state.md#screen-space-effects).

**The fragment contract.** The game supplies a *fragment only* — the engine's shared fullscreen-triangle
`postprocess.vert` is the vertex stage. The fragment's resources mirror the built-in displacement stage
exactly: one sampled **source texture + sampler** (`t0`/`s0`, `space2` — the composited image, or the
prior chain pass) and, when `uniformSize > 0`, one **uniform cbuffer** (`b0`, `space3`) the game fills
each frame via `ScreenSpaceEffect::uniform`. `uniformSize` is that cbuffer's byte size — `0`, or a
multiple of 16 (SDL_GPU register packing); other values throw. No extra textures/storage inputs in this
version. Handles live until the renderer is destroyed (no unregister yet).

**Authoring the shader.** A game writes HLSL and compiles it to this platform's bytecode through the
**same build-time generator the engine uses for its own shaders** — there is no runtime shader
compiler. The generator is exposed as a CMake function:

```cmake
retropp_generate_shader(STEM my_effect.frag
                      SRC  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/my_effect.frag.hlsl"
                      OUT  "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders/shaders/generated/my_effect_frag.h")
```

The generated header exposes the same symbol set the engine consumes (`kSpirv`/`kDxil`/`kMsl` +
entrypoints); wrap it in a `ShaderVariants` and pass it to `registerPostProcessStage` — identical to how
the engine builds its own stages. The `custom_shader_demo` example is a worked end-to-end instance: a
consumer-authored radial **ripple** (a water-droplet effect the axis-aligned built-in can't express),
generated through this hook, registered, and stacked with the built-in wave.

## Amortized resources: `uploadAtlas` / `uploadPalette`

Pixel art and colour are uploaded **once** (at load time / on change), not per frame; the draw state
then references them by handle each frame. See [tiles-and-colour.md](tiles-and-colour.md) for the
colour model and [images-and-transparency.md](images-and-transparency.md) for loading art from PNG.

- `uploadAtlas(indices, w, h, transparentIndex = -1)` → `AtlasId`. An **indexed** atlas: one palette
  index per pixel (not RGBA). The optional `transparentIndex` is that source's index-hole transparency
  (default −1 = fully opaque).
- `uploadPalette(colors)` → `PaletteId`. One palette's colours, written to a row of the renderer-owned
  palette store.

Handles stay valid until the renderer is destroyed (there is no per-handle eviction yet).

## Layer-key collision policy

A frame's layers must have unique `z` and unique `id` (see [draw-state.md](draw-state.md)). The
renderer validates this each frame and reacts per a policy you can override:

```cpp
renderer.setLayerCollisionPolicy(LayerKeyCollisionPolicy::Throw);          // fail fast (dev default)
renderer.setLayerCollisionPolicy(LayerKeyCollisionPolicy::WarnAndResolve); // keep a shipped game up
```

The default is `Throw` in debug builds (a collision surfaces the instant its frame runs) and
`WarnAndResolve` in release (a deterministic order is still produced, the game stays up).

## How shaders reach the GPU

The engine authors its shaders once in HLSL and **compiles them to the running platform's native
format at build time** — MSL on macOS (Metal), SPIR-V on Linux (Vulkan), DXIL on Windows (D3D12) —
using that platform's standard tools (`glslang` / `spirv-cross` / `dxc`). Nothing is cross-compiled
and no bytecode is committed; a clone builds its own shaders. At runtime the renderer picks the
variant the live device accepts:

```cpp
std::optional<std::pair<ShaderBytecode, SDL_GPUShaderFormat>>
selectShader(SDL_GPUShaderFormat supported, const ShaderVariants& variants);
```

A device never reports a format its backend can't run, and a variant not generated on this platform
is a null entry `selectShader` treats as absent — so the renderer code is identical on every
platform. This is internal plumbing; a consumer never calls it. The shader source and the build-time
generator live under `shaders/` (see `shaders/README.md`); the build-time tools are dependencies of
*building the engine*, never of the shipped binary.

## Where to change things

- **Internal render resolution:** the `Renderer` constructor's `ViewportResolution` (or
  `EngineConfig::viewport`).
- **A new shader / shader edit:** edit the HLSL under `shaders/src/` — the next build regenerates the
  affected per-platform header automatically (`shaders/README.md`).
- **Sampling (crisp vs smoothed):** `Renderer::setSamplingMode` (`Nearest` / `Bilinear`), or seed it
  from `EngineConfig::enhancements.sampling`.
- **Window size / presentation scale:** that's `windowScale` + `Platform::setWindowSize` /
  native fullscreen, on the platform side ([platform-and-windowing.md](platform-and-windowing.md)) —
  the renderer always fills whatever window it's given.
- **Screen-space content effects (wavy water, heat haze):** `FrameDrawState::postEffects` for the
  whole frame, or `DrawLayer::effect` (`Layer` / `Below` scope) for a single layer / everything below a
  layer — see `postEffects` above and [draw-state.md](draw-state.md#screen-space-effects).
- **Scale / rotate / skew / perspective a layer (Mode-7-style floors):** `DrawLayer::transform` (a
  `Transform`) + `DrawLayer::transformEdge` — see [draw-state.md](draw-state.md#transforms).
- **Transform a single sprite (spin/scale/foreshorten about its own pivot):** `Sprite::transform`,
  composing with the layer transform — see [draw-state.md](draw-state.md#per-sprite-transforms).
- **Your own shader effect (the built-ins don't cover it):** `registerPostProcessStage` + a
  `Custom`-kind effect — see "Custom shader stages" above.
- **Post-process display filters (CRT, scanlines):** author them as a `Custom` stage today (a
  full-frame fragment over the composited image), or wait for a planned engine-provided stage on the
  same chain machinery; the fill + sampling above is the faithful baseline it builds on.
