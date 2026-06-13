# Draw state

The submission envelope: the C++ shape a game hands the renderer each frame. This is the heart of
the "how do I draw something" path — an arbitrary stack of Z-sorted layers, each carrying tiles or
sprites, plus frame-level colour modifiers. The colour *model* (indexed atlases + palettes) is
[tiles-and-colour.md](tiles-and-colour.md); how a frame is composited and presented is
[rendering.md](rendering.md).

```cpp
#include "gbcpp/draw_state.h"   // FrameDrawState, DrawLayer, TileContent, SpriteContent, …
```

## The model: a whole frame, computed fresh

The frame's draw state is computed **whole every frame** from the game's logical inputs. There is no
mid-frame state-change API, no reconstructed scanline interrupt, no register poke. A game recomputes
and resubmits a fresh `FrameDrawState` each frame — layer existence, z, scroll, size, alpha, and
effect parameters are all fresh every frame (the one amortized exception is the atlas/palette texels,
uploaded when they change). Effects a Game Boy expressed through hardware tricks are expressed here
as **layers, per-tile/per-region colour attributes, frame-level modifiers, and screen-space-effect
declarations** — never as a hardware-register idiom.

**Identity is a typed, first-class field throughout** — `LayerId`, `AtlasId`, the `*Kind` enums —
never an array position, never a packed byte behind a comment.

## `FrameDrawState` + `DrawLayer`

```cpp
struct FrameDrawState {
    std::vector<DrawLayer>         layers;          // arbitrary N; compositor stable-sorts by z
    ColorModifier                  globalModifier{};// day/night; None by default
    Blend                          blend{};         // cutscene flash; None by default
    std::vector<ScreenSpaceEffect> postEffects;     // frame-level effects on the composited image (row-displacement available)
};

struct DrawLayer {
    LayerId           id{};        // human-readable label — identity; NO role in depth
    std::int32_t      z = 0;       // back-to-front sort key; unique within a frame
    PixelSize         size{};      // independent per-layer dimensions
    LayerScroll       scroll{};    // independent scroll offset {x, y}
    float             alpha = 1.0f;// [0,1], default opaque
    LayerContent      content{ TileContent{} };  // tiles OR sprites
    ScreenSpaceEffect effect{};    // per-layer screen-space effect; None by default (scope: Layer / Below)
};
```

The compositor draws the layers **back-to-front by `z`**. There is **no semantic layer model** — the
engine imposes no "background" / "sprite" / "window" roles. A layer is just tiles-or-sprites at a
depth; "the player walks behind that tree" is simply a higher-`z` layer, not a priority flag the
engine evaluates.

**How you produce the `layers` each frame is your choice.** Either rebuild it — `clear()` the vector
(it keeps capacity, so arbitrary N with no steady-state heap churn) and push the layers you want — or
build it once and mutate only what changed. Both are fully supported and produce identical output; the
engine holds no persistent per-layer state of its own. See
[the retained-vs-rebuilt recipe](how-to.md#retained-vs-rebuilt-frame)
([`beach_demo`](../../examples/beach_demo.cpp) rebuilds; [`hello_world`](../../examples/hello_world.cpp)
retains).

### Layer identity vs depth

`LayerId` is a **human-readable label** (`id = "ParallaxClouds"`), constructed implicitly from a
string literal. It is identity only and **fully independent of `z`**: `z` alone controls depth, the
id plays no part in ordering. The engine uses the id only to tell layers apart and to name them in
diagnostics. The name must outlive the `renderFrame()` call (string literals always do).

### Layer-key uniqueness is a contract

Within one frame, no two layers may share a `z` (their order would be undefined) **or** a `LayerId`
(identity must be unambiguous). The engine enforces this two ways:

```cpp
// Compile-time: turn a fixed layer stack's collision into a BUILD error.
static_assert(layerKeysAreUnique(kMyFixedLayers), "z/id collision in layer stack");

// Runtime: layerDrawOrder() validates and reacts per the renderer's collision policy
// (Throw in debug, WarnAndResolve in release — see rendering.md).
```

`findLayerKeyCollision(layers)` returns the first collision (or `nullopt`); `layerKeysAreUnique` is
the boolean form for `static_assert`. For runtime-built layer stacks the renderer calls
`layerDrawOrder` for you each frame.

## Layer content: tiles or sprites

```cpp
enum class LayerContentKind : std::uint8_t { Tiles, Sprites };
using LayerContent = std::variant<TileContent, SpriteContent>;
LayerContentKind contentKind(const LayerContent&) noexcept;
```

A layer carries exactly one content alternative — a tile map or a set of sprites. Tile and sprite
layers **interleave freely by `z`** in one compositing pass, so a sprite layer can sit between two
tile layers or vice versa, entirely the consumer's choice.

### `TileContent` — a scrolling tile map

```cpp
struct TileContent {
    AtlasId                    atlas{};       // indexed tile atlas (palette indices, not colour)
    std::span<const PaletteId> palettes;      // the layer's palette set; a cell selects within it
    int                        widthInTiles  = 0;
    int                        heightInTiles = 0;
    std::span<const TileCell>  cells;         // row-major, widthInTiles * heightInTiles
};

struct TileCell {
    std::uint16_t tile     = 0;     // index into the layer's indexed atlas
    std::uint8_t  palette  = 0;     // which palette in the layer's set
    bool          flipX    = false;
    bool          flipY    = false;
    bool          priority = false; // carried (BG-over-OBJ data); NOT evaluated by the engine
};
```

A tile layer is a row-major grid of cells sampled per-pixel in the shader against the layer's scroll,
so arbitrary layer sizes and toroidal wrapping are handled on the GPU. Each cell picks an atlas tile,
a palette within the layer's set, and flips — see [tiles-and-colour.md](tiles-and-colour.md) for the
colour mechanism. `atlas`, `palettes`, and `cells` are game-owned and must outlive the `renderFrame`
call. `priority` is carried so the cell layout is final, but it is **advisory consumer data** — the
engine evaluates no cross-layer priority (arrange depth with `z` instead).

### `SpriteContent` + `Sprite` — placed sprites

```cpp
struct SpriteContent {
    AtlasId                    atlas{};       // indexed sprite atlas
    std::span<const PaletteId> palettes;      // the layer's palette set; a sprite selects within it
    std::span<const Sprite>    sprites;
};

struct Sprite {
    int           x = 0, y = 0;        // top-left in the LAYER's space (before scroll)
    SpriteSize    size = SpriteSize::GameBoy8x8;
    std::uint16_t tile = 0;            // top-left atlas cell (8px grid)
    std::uint8_t  palette = 0;         // palette-select within the layer's set
    bool          flipX = false, flipY = false;
    bool          priority = false;    // carried (behind-BG data); NOT evaluated by the engine
};
```

Sprites are instanced per-quad. `x`/`y` are in the layer's coordinate space, so a sprite on a
world-scrolling layer tracks the background while a HUD layer at `scroll {0,0}` stays fixed. A sprite
reads a `size.width × size.height` pixel rectangle from the atlas at its `tile` cell's origin (a 16×16
sprite spans a contiguous 2×2 cell block). Colour index 0 is OBJ-transparent on the sprite path
(discarded). `SpriteSize` is a `{width, height}` tuple with named console presets
(`SpriteSize::Snes16x16`, …) — a preset or a raw size interchangeably.

## Frame-level colour modifiers

```cpp
struct ColorModifier {                 // whole-frame: out = clamp(in * mul + add)
    ColorModifierKind kind = ColorModifierKind::None;   // None | MultiplyAdd
    float mulR = 1, mulG = 1, mulB = 1;
    float addR = 0, addG = 0, addB = 0;
};

struct Blend {                         // cutscene flash: mix the frame toward (r,g,b) by strength
    BlendKind kind = BlendKind::None;  // None | Flash
    float r = 0, g = 0, b = 0, strength = 0;
};
```

`FrameDrawState::globalModifier` and `blend` are a **whole-frame post-composite transform** folded
into the blit stage — `clamp(in·mul + add)`, then a flash mix — applied to the already-composited
frame. This is the modern post-effect path for fades, day/night, and cutscene flashes; it is **not**
the colouring mechanism (that is index + palette). The default of both is the identity, so a frame
that sets neither renders the faithful baseline **byte-for-byte**. `frameColorTransform(modifier,
blend)` is the pure, unit-tested CPU mirror of the blit shader's math.

> **Photosensitivity note.** `Blend`/`Flash` and rapid `ColorModifier` changes drive full-frame
> luminance flicker. Keep flashes gentle and infrequent and avoid sustained high-frequency
> full-screen oscillation.

## Screen-space effects

A screen-space effect is a function `f(row, phase)` the GPU evaluates per-pixel (wavy water, heat
haze, per-line scroll) — no reconstructed scanline counter, no HBlank interrupt. The game advances
`phase` per frame to animate.

```cpp
struct ScreenSpaceEffect {             // frame-level (postEffects) and per-layer (DrawLayer::effect)
    ScreenSpaceEffectKind kind = ScreenSpaceEffectKind::None;  // None | RowDisplacement | Custom
    float amplitude = 0;   // displacement magnitude, in viewport pixels (RowDisplacement)
    float frequency = 0;   // cycles across the displaced axis (RowDisplacement)
    float phase     = 0;   // animation phase (advance it per frame) (RowDisplacement)
    Axis  axis = Axis::Horizontal;            // Horizontal = displace columns by row; Vertical = rows by column
    DisplacementEdge edge = DisplacementEdge::Blank;  // frame-edge behaviour, below
    ScreenSpaceEffectScope scope = ScreenSpaceEffectScope::Layer;  // per-layer reach (DrawLayer::effect only)
    PostProcessStageId         customShader{};  // kind == Custom: your registered shader (below)
    std::span<const std::byte> uniform{};       // kind == Custom: your per-frame uniform bytes
};
```

`RowDisplacement` is the engine's built-in effect; `Custom` runs **your own shader** (see "Custom
shader stages" below). Both flow through the same two attachment points — the same type drives the
effect at two places:

- **Frame-level — `FrameDrawState::postEffects` (available).** Each effect is a full-viewport pass on
  the **already-composited image**, run after every layer composites and before the window blit. The
  whole frame wobbles together. Push a `RowDisplacement` to wave the screen; an empty list is the
  faithful baseline (byte-identical to no effect). Stack several and they run in submission order.
- **Per-layer — `DrawLayer::effect` (available).** A composable, Photoshop-style layer effect. `scope`
  chooses its reach:
  - **`Layer` (default — isolated).** Displaces **only this layer's own content**, before it
    composites. A wavy water layer distorts while the layers and sprites composited above it stay
    still — the faithful per-line-scroll water. An exposed `Blank` edge here is *transparent*, so it
    reveals the layers below.
  - **`Below` (adjustment layer).** Displaces the **whole accumulated image at this layer's `z`** —
    this layer's own content *and* everything beneath it, coherently — then the layers above this `z`
    composite on top, undisplaced. A **content-less** `Below` layer placed just under your HUD wobbles
    the world while the HUD rides steady; a **content-bearing** `Below` layer wobbles itself together
    with the scene beneath. (Frame-level `postEffects` is the same idea applied above the whole stack.)

  A `None`-kind effect (the default) is no effect — that layer composites on the unchanged faithful
  path. A "blank" layer is just a layer with empty content plus an effect.

### Frame edge: `DisplacementEdge`

When a displacement pulls a row (or column) inward, it exposes a strip at the frame edge with no
source pixel behind it. You choose what fills it, per effect:

```cpp
enum class DisplacementEdge { Blank, Stretch };
effect.edge = DisplacementEdge::Blank;    // default — the exposed strip is backdrop-blank
effect.edge = DisplacementEdge::Stretch;  // duplicate the edge pixel outward (smears the border)
```

`Blank` is the default. What "blank" means depends on the scope: at frame-level and for a `Below`
effect it is the backdrop colour (those displace an opaque image, so the strip shows the backdrop
rather than a stretched edge); for a `Layer` (isolated) effect it is *transparent*, so the exposed
strip reveals the layers composited below this one. `Stretch` (edge-duplicate) is offered for the look
some effects want, at any scope. (A genuinely *seamless* wrapping water — where a pulled-in row reveals
the next wrapped tile instead of an exposed strip at all — would displace inside the tile sampler; that
is a possible future option, not built today. For most cases, sizing the wavy content to its own layer
and letting the `Blank` strip reveal what's below is the simpler answer.)

> **Photosensitivity note.** Keep displacement slow and low-frequency — animate `phase` gently. A fast
> or high-amplitude wave over fine art produces a shimmering, strobe-like moiré.

### Custom shader stages — your own effect (`kind = Custom`)

When the built-in effect vocabulary stops, register your own fragment shader and use it **exactly like
a built-in effect** — at either attachment point, composing with the built-ins. Your shader is a
first-class effect kind, not a stage bolted on at the end.

Two steps:

```cpp
// 1) Once, at load time — register your fragment, declaring its uniform's byte size (0, or a
//    multiple of 16). Returns a handle. You author the HLSL and compile it through the engine's
//    build-time generator; see rendering.md "Custom shader stages" for the shader contract + build.
PostProcessStageId ripple = renderer.registerPostProcessStage(rippleFragmentVariants, sizeof(RippleUniforms));

// 2) Per frame — attach it as an effect, frame-level or per-layer, with your uniform bytes.
RippleUniforms u{ /* your params: centre, amplitude, phase, … */ };
frame.postEffects.push_back(ScreenSpaceEffect{
    .kind = ScreenSpaceEffectKind::Custom,
    .customShader = ripple,
    .uniform = std::as_bytes(std::span<const RippleUniforms>(&u, 1))});  // you own the storage
```

`amplitude`/`frequency`/`phase`/`edge` are **not** read for a `Custom` effect — your shader + your
uniform define its behaviour. `scope` still applies (a per-layer `Custom` effect is `Layer`-isolated or
a `Below` adjustment, exactly like a built-in — scope is a compositing decision the engine makes, not
the shader). Order is purely list order: a `Custom` entry and a built-in `RowDisplacement` in the same
`postEffects` run in whatever order you push them. The uniform span must point at storage that outlives
the `renderFrame()` call (like the layer content spans), and its byte-count must equal the size you
registered. The included `custom_shader_demo` registers a radial water-droplet **ripple** — an effect
the axis-aligned built-in can't express — and stacks it with the built-in wave.

## Where to change things

- **Stacking order / "walk behind":** set the layer's `z` — depth is `z` only.
- **Parallax:** give layers different `scroll` rates.
- **A see-through layer:** per-layer `alpha` (whole-layer translucency), or per-source index-hole
  transparency on the atlas (see [images-and-transparency.md](images-and-transparency.md)).
- **Day/night, fades, flashes:** `globalModifier` / `blend` (whole-frame).
- **The colour of the art itself:** the palette set + per-cell/per-sprite palette-select
  ([tiles-and-colour.md](tiles-and-colour.md)).
