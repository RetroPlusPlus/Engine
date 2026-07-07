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
```

## Contents

- [The model](#the-model)
- [The internal viewport: `ViewportResolution`](#the-internal-viewport-viewportresolution)
- [Filling the window: `integerScaleToFitRect`](#filling-the-window-integerscaletofitrect)
- [Sampling: `SamplingMode`](#sampling-samplingmode)
- [Evaluation grid: `EvaluationGrid`](#evaluation-grid-evaluationgrid)
- [Per-frame submission: `renderFrame`](#per-frame-submission-renderframe)
- [Offscreen capture: `captureViewport`](#offscreen-capture-captureviewport)
- [Post-process effects: `postEffects`](#post-process-effects-posteffects)
  - [Built-in effect library](#built-in-effect-library)
- [Custom shader stages: register a shader by path](#custom-shader-stages-register-a-shader-by-path)
- [Amortized resources: `uploadAtlas` / `uploadPalette`](#amortized-resources-uploadatlas--uploadpalette)
- [Layer-key collision policy](#layer-key-collision-policy)
- [How shaders reach the GPU](#how-shaders-reach-the-gpu)
- [Where to change things](#where-to-change-things)

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
    static inline ViewportResolution defaultViewport;  // 160×144; seeded by EngineConfig::setActive()

    Renderer(SDL_GPUDevice* device, SDL_Window* window,
             ViewportResolution viewport = defaultViewport);

    AtlasId   uploadAtlas(const std::uint8_t* indices, int width, int height,
                          int transparentIndex = -1);   // also uint16_t / uint32_t overloads
    PaletteId uploadPalette(std::span<const Rgba8> colors);

    PostProcessStageId registerPostProcessStage(LiteralPath shaderPath);  // custom shader, by .hlsl path (string literal)

    void renderFrame(const FrameDrawState& frame);   // composite + present; auto-interpolates (default)

    std::vector<Rgba8> captureViewport(const FrameDrawState& frame);     // compose offscreen, download pixels

    void setLayerCollisionPolicy(LayerKeyCollisionPolicy) noexcept;
    LayerKeyCollisionPolicy layerCollisionPolicy() const noexcept;

    void setInterpolation(bool enabled) noexcept;          // automatic per-object easing; default on
    bool interpolationEnabled() const noexcept;

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

The internal render resolution defaults to the original Game Boy 160×144 (the faithful default) and
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
faithful crisp-pixel output value-for-value; a consumer reads `EngineConfig::enhancements.sampling` and calls
the setter, then toggles it live from a settings menu.

**Native fullscreen** is a `Platform`-seam concern, not a renderer one — see
[platform-and-windowing.md](platform-and-windowing.md). The fill blit absorbs the fullscreen target
size with no renderer change. **High-DPI** is automatic: the window opts into
`SDL_WINDOW_HIGH_PIXEL_DENSITY`, `drawableSize()` reports physical pixels, and the fill picks the
larger integer scale so the art renders crisp at native resolution.

## Evaluation grid: `EvaluationGrid`

```cpp
enum class EvaluationGrid { Viewport, Output };                       // output.h
void           Renderer::setEvaluationGrid(EvaluationGrid) noexcept;  // runtime-dynamic
EvaluationGrid Renderer::evaluationGrid() const noexcept;
```

`Viewport` (the default) evaluates every analytic render path on the viewport grid — transformed tile
layers, the effect regions (select / stencil, in polygon / analytic-curve / baked-mask form), the
sampling effects (`RowDisplacement` / `Ripple`), **geometrically-transformed sprites** (`Sprite::transform`
/ `DrawLayer::transform`), and **custom shader stages** (automatically — see
[Custom shader stages](#custom-shader-stages-register-a-shader-by-path)). The image is then
pixel-identical to the viewport-resolution rasterization,
nearest-upscaled: crisp, square pixels — even when placement composites onto a finer grid for steady
motion. `Output` evaluates each path per output pixel instead, so edges and displacement resolve smoothly
at the higher resolution (softer under upscale). The setting is a mathematical no-op when the compositor
runs at viewport resolution — it only bites once compositing runs above it.

A transformed sprite resolves its coverage per viewport cell: the rotated / scaled / foreshortened
silhouette AND the internal texel stairs land on the viewport grid, so a spun sprite reads as chunky
blocks that upscale cleanly rather than resampled edges. An untransformed sprite is unaffected either way
— its texels are already solid blocks and its sub-pixel placement is the steady-motion mechanism.

Evaluation granularity is independent of **placement** granularity: object placement stays sub-pixel on
either setting (the steady-motion mechanism), so `EvaluationGrid` chooses only where the geometry is
*sampled*, not where whole objects *land*. Seed it from `EngineConfig::evaluationGrid` (which `setActive()`
fans into `Renderer::defaultEvaluationGrid`), then flip it live from a settings menu.

## Per-frame submission: `renderFrame`

```cpp
void renderFrame(const FrameDrawState& frame);
```

Call this once per render callback. The game hands a whole `FrameDrawState` (the Z-sorted layer
stack — see [draw-state.md](draw-state.md)); the renderer composites the layers back-to-front into
the offscreen viewport — applying any **per-layer screen-space effects** (`DrawLayer::effect`,
`Layer` / `Below` scope) as it goes — then runs the **frame-level post-process chain** (`postEffects`,
below), applies the frame-level colour transform, and blits the viewport integer-scaled + letterboxed
onto the swapchain and presents. There is **no mid-frame state-change API** — a frame is computed whole
and submitted whole, every frame. A frame with no per-layer effects composites in a single pass — the
per-layer path is paid for only where used.

**Automatic interpolation.** There is no interpolation argument. With interpolation on (the default),
the renderer eases each layer and sprite between its previous and current simulation-tick state by the
run loop's sub-tick factor, matching each object to its prior tick by its `key` (see
[draw-state.md](draw-state.md)); motion stays smooth when the display refreshes faster than the
simulation ticks. The game submits its latest state each frame and the engine blends.
`setInterpolation(false)` (or `EngineConfig::interpolation = false`) turns it off, and each submission
composites verbatim — the game then owns any blending itself. The full model, including the
developer-owned path, is in [run-loop-and-timing.md](run-loop-and-timing.md#interpolation).

## Offscreen capture: `captureViewport`

```cpp
std::vector<Rgba8> captureViewport(const FrameDrawState& frame);
```

`captureViewport` composites `frame` exactly as `renderFrame` does — the copy pass, the layer
composite, the post-process chain — but **downloads** the finished viewport as packed `Rgba8` instead
of blitting it to the window. The result is `viewport.width × viewport.height` pixels, row-major, top
row first; the bytes are what `renderFrame` would have shown. Use it for a thumbnail, a screenshot, a
server-side render, or an image-diff test.

It works on any renderer, **including a windowless one**. A `Renderer` built with `nullptr` for the
window is *compose-only*: it builds the device-side compose resources but no blit pipeline and never
acquires a swapchain, so it composites and captures but cannot present.

```cpp
Renderer offscreen{device, /*window=*/nullptr, ViewportResolution::GameBoyColor};
std::vector<Rgba8> pixels = offscreen.captureViewport(frame);   // composes on the GPU, no window
```

A compose-only renderer needs a GPU **device** but no display, so it runs on a headless machine. The
call **blocks** until the download lands (a fence wait); it is a capture, not part of the per-frame
loop, so keep it off the hot path.

## Post-process effects: `postEffects`

After compositing, the renderer runs `FrameDrawState::postEffects` — a list of screen-space effects
applied to the **whole composited viewport** before the blit. Each effect is one full-viewport pass;
the renderer ping-pongs two internal scratch targets so any number of effects chain in submission
order. An **empty list is the faithful path** — the blit samples the composited viewport
directly. To **confine** an effect to a shape, put it in a `Region` (which owns the shape and the
effect) on `FrameDrawState::regions` or `DrawLayer::regions` — the renderer gates it with an extra
select pass that leaves the rest of the image untouched; see
[draw-state.md](draw-state.md#confining-an-effect-to-a-shape-region).

By default these effects and their region gates evaluate on the **viewport grid** (crisp — the result is
pixel-identical to the viewport-resolution rasterization, nearest-upscaled); switch to per-output-pixel
(smooth) evaluation with `EvaluationGrid::Output` — see [Evaluation grid](#evaluation-grid-evaluationgrid).

### Built-in effect library

The engine ships a growing set of **built-in effects** behind `ScreenSpaceEffectKind` — you name the
kind and set parameters; the engine owns the shader (no registration, no shader authoring). Shipped:

- **`RowDisplacement`** — wavy water / heat haze / per-line scroll (axis-aligned), with a developer-
  selectable frame-edge (`Blank` default / `Stretch`).
- **`Ripple`** — a radial concentric ripple (a water droplet): rings expand outward from a `center`,
  faded with radius by `decay`. Aspect-corrected so the rings stay circular.
- **`ColorFill`** — paint a colour (`Rgba8 fill`) into the effect's region: `out.rgb = fill`. A filled
  region is a solid shape, a stroked region a drawn colored line/path; a `Region`'s `alpha` makes its fill
  translucent. (Writes over existing pixels — see
  [draw-state.md](draw-state.md#painting-a-colour-into-a-region-colorfill) for the opaque-source scopes.)

You build one with plain designated-init — set `.kind` and the fields that kind consults; every field is
settable inline, nothing is hidden:

```cpp
frame.postEffects.push_back(ScreenSpaceEffect{
    .kind = ScreenSpaceEffectKind::Ripple, .amplitude = 6.0f, .frequency = 6.0f,
    .phase = t * 0.012f, .center = {80, 72}, .decay = 2.5f});
frame.postEffects.push_back(ScreenSpaceEffect{
    .kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 3.0f, .frequency = 3.0f,
    .phase = t * 0.01f, .axis = Axis::Horizontal});
```

`center` is in **viewport pixels** (the engine normalizes to UV); advance `phase` slowly off your frame
counter to animate (slow expansion — no strobing). The effect type, its scopes (frame-level here vs.
per-layer), the edge choice, and which fields each kind consults are documented in
[draw-state.md](draw-state.md#screen-space-effects); the candidate built-ins still to come are listed in
[effect-library-roadmap.md](../effect-library-roadmap.md). These are *content* effects declared on the
draw state — distinct from output-side display filters (CRT/scanlines), a separate planned stage (below).

## Custom shader stages: register a shader by path

When the built-in effect vocabulary stops, a game writes its **own fragment shader** and uses it as a
first-class screen-space effect. Registration is just the shader's **path**:

```cpp
auto stage = renderer.registerPostProcessStage("game/shaders/my_effect.frag.hlsl");
```

That is the whole thing — **no `ShaderVariants`, no uniform type, no generated-header include, no CMake
rule.** A build-time source scan sees that `.hlsl` path referenced in your code, compiles it to this
platform's GPU bytecode, embeds it in the executable, and registers it under the path string; the call
resolves the path against the embedded registry at load time and builds the pipeline pair.

> **The path must be a string literal.** Because the scan reads it out of your source verbatim, a path
> passed as a variable, a `std::string`, or a computed/concatenated value is invisible to it. The
> parameter type enforces this: a non-literal is a **compile error**, not a runtime surprise. Write the
> literal directly at the call — `registerPostProcessStage("game/shaders/my_effect.frag.hlsl")`.

Your shader declares its **own parameters** in a cbuffer — its own names — and writes only `main()`; the
engine injects the plumbing (the source texture + sampler):

```hlsl
// game/shaders/my_effect.frag.hlsl — you write only the cbuffer + main()
cbuffer Params : register(b1, space3) {
    float2 center;
    float  strength;
};
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv + (center - uv) * strength);  // sampleSource() honours the effect's edge policy
}
```

The build **reflects that cbuffer** and surfaces its fields on `ScreenSpaceEffect` by name, so you set them
inline — exactly like a built-in's named params, with **no uniform struct, no `as_bytes`, no size**:

```cpp
frame.postEffects.push_back(ScreenSpaceEffect{
    .kind = ScreenSpaceEffectKind::Custom, .customShader = stage,
    .center = {0.5f, 0.5f}, .strength = 0.2f});   // your shader's OWN params, mutated live per frame
```

It composes with the built-ins in submission order, at either attachment point (`postEffects` or
`DrawLayer::effect`), and is region-gateable — wherever a built-in works, a custom shader does too.

**The fragment contract.** The game supplies a *fragment only* — the engine's shared fullscreen-triangle
`postprocess.vert` is the vertex stage. The engine injects the plumbing: **`sampleSource(uv)`** (the
composited image, or the prior chain pass) plus an engine cbuffer (`b0`, `space3`). Your shader adds its own
parameter cbuffer at **`b1`, `space3`**, which the build reflects into the effect's inline fields and fills
per frame from them. **Always sample through `sampleSource()`** — it obeys the effect's **edge policy**
(`ScreenSpaceEffect::edge`): `Blank` (the default) returns transparent outside `[0,1]` so an effect that
displaces past the frame edge reveals the backdrop / layers below; `Stretch` clamps (smears) the border.
The edge behaviour is the **layer/effect's** choice, not the shader's — sampling `SourceTexture` directly
opts out and always clamps. **No runtime shader compiler** — the bytecode is built and embedded; nothing is
loaded from disk at run time. Handles live until the renderer is destroyed.

**Crisp automatically.** A custom shader written to this contract — spatial math on the `uv` your
`main()` receives, sampling through `sampleSource()` — is crisp on the default
[`EvaluationGrid::Viewport`](#evaluation-grid-evaluationgrid) with **no shader-side work**. The engine's
generated entry point hands your `main()` the centre of the viewport cell its fragment falls in, so your
math evaluates once per viewport pixel; `sampleSource()` quantizes the displacement you request to whole
viewport pixels; `paramRowAtUv()` lands on the row the viewport-resolution rasterization reads. The
result upscales as solid square pixels even when the compositor runs at output resolution for steady
motion. `EvaluationGrid::Output` hands `main()` the raw output-resolution uv and samples exactly what you
request — smooth evaluation, wholesale.

**Build wiring.** Engine examples get the source scan automatically. A standalone game applies it **once
per target** — `retropp_autocompile_shaders(<target>)` after defining the target — and then never touches
CMake again, however many shaders it adds; re-run CMake after adding a *new* path reference (the scan is a
configure-time read of your sources). The custom path is for the long tail the built-in library doesn't
cover — effects that *do* have a use case are built-ins that need none of this. The `custom_stage_test`
exercises the reflection + packing device-free.

**Fast path for many additive regions — one declaration line.** When a custom shader is used across *many*
region-confined effects on one frame (e.g. a glow per pickup, a light per particle), the naive cost is two
full-frame passes per region — a serialized cliff as the count grows. If your shader's output is its source
**plus a term that does not depend on the source's contents** — `out = sampleSource(uv) + D(uv)` (a bloom, a
light, an additive tint) — declare it additive with a single comment line near the top of the `.hlsl`:

```hlsl
// retropp: additive
```

That is the **entire** opt-in — no API call, no `Region` field, no build rule. The engine compiles a second
variant and collapses all eligible same-shader regions into **one** instanced additive pass (pass count
becomes independent of region count). It engages automatically for a region on `Normal` blend at full
`alpha` whose shape is a circle or capsule; any other region silently keeps the per-region path with
identical output, so you never structure your scene around it. The declaration is a *promise*: a shader that
multiplies the source, samples its neighbours, or otherwise depends on the source contents will render
incorrectly on the fast path — the engine cannot verify additivity, which is exactly why the safe (per-
region) path is the default and the fast path is the thing you vouch for. Built-in additive effects need no
declaration (the engine already knows their math). `salvage_glow.frag.hlsl` in the Kessler example is the
worked case.

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
- **Analytic-path evaluation (crisp vs smooth edges/displacement):** `Renderer::setEvaluationGrid`
  (`Viewport` / `Output`), or seed it from `EngineConfig::evaluationGrid` — see "Evaluation grid" above.
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
- **Capture a frame offscreen (thumbnail, screenshot, headless render):** `Renderer::captureViewport`
  on a windowless (`nullptr`-window) renderer — composes on the GPU and downloads pixels, no display
  needed; see "Offscreen capture" above.
- **Your own shader effect (the built-ins don't cover it):** `registerPostProcessStage` + a
  `Custom`-kind effect — see "Custom shader stages" above.
- **Post-process display filters (CRT, scanlines):** author them as a `Custom` stage today (a
  full-frame fragment over the composited image), or wait for a planned engine-provided stage on the
  same chain machinery; the fill + sampling above is the faithful default it builds on.
