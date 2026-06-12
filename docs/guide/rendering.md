# Rendering

The `Renderer` object: the internal viewport it draws into, how that viewport is scaled and
letterboxed onto the window, the once-per-frame submission entry point, the amortized atlas/palette
uploads, and how shaders reach the GPU. The *content* model a frame carries (layers, tiles, sprites,
colour) is [draw-state.md](draw-state.md) + [tiles-and-colour.md](tiles-and-colour.md); this page is
the object and the output path.

```cpp
#include "gbcpp/renderer.h"       // Renderer
#include "gbcpp/viewport.h"       // ViewportResolution
#include "gbcpp/geometry.h"       // PixelSize, IntRect, integerScaleToFitRect
#include "gbcpp/shader_format.h"  // ShaderVariants, selectShader (internal plumbing)
```

## The model

The renderer draws every frame into an **offscreen internal viewport** at a fixed retro resolution
(160×144 by default), then **blits that viewport onto the window** integer-scaled and centred, with
letterbox/pillarbox bars filling the leftover. Game content is authored once at the small native
resolution; the window can be any size. This two-stage path (render small → scale to window) is what
keeps pixels crisp and square at any window size, and it is where output-scaling enhancements (CRT
filters, N× modes, fullscreen) attach later (planned — see the Coverage table in the [guide index](README.md)).

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

    void renderFrame(const FrameDrawState& frame, float alpha);

    void setLayerCollisionPolicy(LayerKeyCollisionPolicy) noexcept;
    LayerKeyCollisionPolicy layerCollisionPolicy() const noexcept;
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

## Output scaling + letterbox: `integerScaleToFitRect`

```cpp
IntRect integerScaleToFitRect(PixelSize drawable, PixelSize viewport) noexcept;
```

The faithful default output scaling, and a pure function you can unit-test independent of the GPU:
the largest integer multiple of the viewport that fits the window's drawable size, centred, with the
leftover split into letterbox/pillarbox margins. The renderer reads the window's `drawableSize()`
each frame and tracks resizes, so the scaled rect follows the window live. The scale clamps to a
minimum of 1× (a window smaller than the viewport shows content at 1× overflowing the window rather
than collapsing). The richer scaling-mode vocabulary (free fit / fullscreen / forced N×) is the
planned enhancement chain; this is the faithful baseline it builds on.

## Per-frame submission: `renderFrame`

```cpp
void renderFrame(const FrameDrawState& frame, float alpha);
```

Call this once per render callback. The game hands a whole `FrameDrawState` (the Z-sorted layer
stack — see [draw-state.md](draw-state.md)); the renderer composites the layers back-to-front into
the offscreen viewport, applies the frame-level colour transform, then blits the viewport
integer-scaled + letterboxed onto the swapchain and presents. `alpha` is the run loop's interpolation
factor. There is **no mid-frame state-change API** — a frame is computed whole and submitted whole,
every frame.

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
- **Output scaling beyond integer-fit-letterbox:** that's the planned enhancement chain (not yet
  available); `integerScaleToFitRect` is the faithful baseline it builds on.
