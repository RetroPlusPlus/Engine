# Draw state

The submission envelope: the C++ shape a game hands the renderer each frame. This is the heart of
the "how do I draw something" path — an arbitrary stack of Z-sorted layers, each carrying tiles or
sprites, plus frame-level colour modifiers. The colour *model* (indexed atlases + palettes) is
[tiles-and-colour.md](tiles-and-colour.md); how a frame is composited and presented is
[rendering.md](rendering.md).

```cpp
#include "retropp/draw_state.h"   // FrameDrawState, DrawLayer, TileContent, SpriteContent, …
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
    std::vector<ScreenSpaceEffect> postEffects;     // frame-level whole-frame effects on the composited image
    std::vector<Region>            regions;         // frame-level shape-confined effects (additive; see below)
};

struct DrawLayer {
    LayerId           id{};        // human-readable label — identity; NO role in depth
    std::int32_t      z = 0;       // back-to-front sort key; unique within a frame
    PixelSize         size{};      // independent per-layer dimensions
    LayerScroll       scroll{};    // independent scroll offset {x, y}
    float             alpha = 1.0f;// [0,1], default opaque
    LayerContent      content{ TileContent{} };  // tiles OR sprites
    ScreenSpaceEffect effect{};    // per-layer whole-layer effect; None by default (scope: Layer / Below)
    std::vector<Region> regions;   // per-layer shape-confined effects (additive; see below)
    Transform         transform{}; // per-layer geometric transform; identity by default (see Transforms)
    DisplacementEdge  transformEdge = DisplacementEdge::Blank;  // what fills the transformed footprint's exposed area
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
([`beach_demo`](../../examples/beach_demo.cpp) rebuilds;
[`controller_scrolling`](../../examples/controller_scrolling.cpp) retains).

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
    TileWrap                   wrap = TileWrap::Repeat;  // how the map samples beyond its bounds
};

enum class TileWrap : std::uint8_t { Repeat, Clamp, Blank };

struct TileCell {
    std::uint16_t tile    = 0;     // index into the layer's indexed atlas
    std::uint8_t  palette = 0;     // which palette in the layer's set
    bool          flipX   = false;
    bool          flipY   = false;
};
```

A tile layer is a row-major grid of cells sampled per-pixel in the shader against the layer's scroll,
so arbitrary layer sizes and wrapping are handled on the GPU. Each cell picks an atlas tile,
a palette within the layer's set, and flips — see [tiles-and-colour.md](tiles-and-colour.md) for the
colour mechanism. `atlas`, `palettes`, and `cells` are game-owned and must outlive the `renderFrame`
call. There is no priority flag — depth is `z` alone; "the player walks behind the treetops" is just a
higher-`z` layer.

`wrap` chooses how the tilemap is sampled outside its `widthInTiles × heightInTiles` bounds:

- **`Repeat`** (default) — toroidal: the map tiles infinitely on both axes.
- **`Clamp`** — clamp the world coordinate to the map's edge row/column, smearing the border tile.
- **`Blank`** — a **finite** map: a coordinate outside the map on either axis is a transparent hole,
  so the map renders exactly once and can never show a wrap seam. This is the mode a finite overworld
  map wants (no infinite repeat as the camera scrolls past the edge).

One `wrap` governs both axes. It is independent of, and composes with, the per-layer `transform`: the
transform maps a destination pixel into the layer footprint, then `wrap` governs sampling the tilemap.

### `SpriteContent` + `Sprite` — placed sprites

```cpp
struct SpriteContent {
    AtlasId                    atlas{};       // indexed sprite atlas
    std::span<const PaletteId> palettes;      // the layer's palette set; a sprite selects within it
    std::span<const Sprite>    sprites;
};

struct Sprite {
    int             x = 0, y = 0;      // top-left in the LAYER's space (before scroll)
    AssetDimensions size = AssetDimensions::GameBoy8x8;
    std::uint16_t   tile = 0;          // top-left atlas cell (8px grid)
    std::uint8_t  palette = 0;         // palette-select within the layer's set
    bool          flipX = false, flipY = false;
};
```

Sprites are instanced per-quad. `x`/`y` are in the layer's coordinate space, so a sprite on a
world-scrolling layer tracks the background while a HUD layer at `scroll {0,0}` stays fixed. A sprite
reads a `size.width × size.height` pixel rectangle from the atlas at its `tile` cell's origin (a 16×16
sprite spans a contiguous 2×2 cell block). Colour index 0 is OBJ-transparent on the sprite path
(discarded). `AssetDimensions` (in [geometry.h]) is a `{width, height}` tuple with named console
presets (`AssetDimensions::Snes16x16`, …) — a preset or a raw size interchangeably — and is also the
unit the atlas slicer carves an image into (see
[images-and-transparency.md](images-and-transparency.md#slicing)).

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
that sets neither passes its composited pixels through unchanged. `frameColorTransform(modifier,
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
    ScreenSpaceEffectKind kind = ScreenSpaceEffectKind::None;  // None | RowDisplacement | Ripple | Custom | Stencil
    PostProcessStageId customShader{};  // kind == Custom: your registered shader (below)
    float amplitude = 0;   // displacement magnitude, in viewport pixels (RowDisplacement, Ripple)
    float frequency = 0;   // RowDisplacement: cycles across the axis; Ripple: rings across the field
    float phase     = 0;   // animation phase (advance it per frame) (RowDisplacement, Ripple)
    Axis  axis = Axis::Horizontal;            // Horizontal = displace columns by row; Vertical = rows by column (RowDisplacement)
    DisplacementEdge edge = DisplacementEdge::Blank;  // frame-edge behaviour, below (RowDisplacement)
    ScreenSpaceEffectScope scope = ScreenSpaceEffectScope::Layer;  // per-layer reach (DrawLayer::effect only)
    Point center{};        // ripple centre, in viewport pixels (Ripple)
    float decay = 0;       // ripple radial falloff rate; 0 = no falloff (Ripple)
    StencilMode stencil = StencilMode::EraseInside;  // Stencil: which side goes see-through (EraseInside | EraseOutside)
    float       feather = 0;   // Stencil: soft-edge width in shape-local px; 0 = hard edge
    ShapePoints shape{};   // Stencil: the geometry it goes see-through along (a Stencil carries its OWN shape)
    std::vector<ScreenSpaceEffect> insideRegion;   // Stencil: optional effects on the INSIDE of `shape`
    std::vector<ScreenSpaceEffect> outsideRegion;  // Stencil: optional effects on the OUTSIDE of `shape`
    // kind == Custom: your shader's OWN params, reflected from its cbuffer and surfaced here BY NAME
    // (e.g. `.pivot`, `.strength`) — set them inline like a built-in's. Generated from the custom shaders
    // your build references (empty if none). See "Custom shader stages" below.
};
```

An effect carries **no shape of its own** (the lone exception is `Stencil`, whose geometry *is* the
effect). To confine an effect to a shape you put it in a **`Region`** (next section), which owns the
shape and the effects applied inside it — *the region owns the effect, not the reverse*.

`RowDisplacement` (axis-aligned wave) and `Ripple` (radial droplet) are the engine's **built-in
effects** — name the kind and set parameters, the engine owns the shader; `Custom` runs **your own
shader** (see "Custom shader stages" below). Build one with plain **designated-init** — set `.kind` and
the fields that kind consults; every field is settable inline, so you keep full control (`.scope`,
`.edge`, all of it). Which fields each kind reads: **RowDisplacement** → amplitude, frequency,
phase, axis, edge; **Ripple** → amplitude, frequency, phase, center, decay; **Custom** →
`.customShader` (which registered shader) + **your shader's own reflected params** (set by name, inline);
**Stencil** → shape, stencil, feather, insideRegion, outsideRegion (it makes the layer **see-through**
rather than colouring it — see "Making a layer see-through" below). `scope` applies to every kind; only
`Stencil` carries geometry — every other effect is confined by a `Region` that owns it (below). The full built-in roadmap is in
[effect-library-roadmap.md](../effect-library-roadmap.md). All built-ins flow through the same two
attachment points — the same type drives the effect at two places:

- **Frame-level — `FrameDrawState::postEffects`.** Each effect is a full-viewport pass on
  the **already-composited image**, run after every layer composites and before the window blit. The
  whole frame wobbles together. Push a `RowDisplacement` to wave the screen; an empty list applies no
  effect (the composited frame blits unchanged). Stack several and they run in submission order.
- **Per-layer — `DrawLayer::effect`.** A composable, Photoshop-style layer effect. `scope`
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

### Confining an effect to a shape (`Region`)

By default an effect covers its whole reach (the viewport for `postEffects`, the layer for
`DrawLayer::effect`). To confine it to a **shape**, put it in a **`Region`** — a shape bound to the
effects applied inside it:

```cpp
struct Region {
    ShapePoints                    shape;    // where the effects apply (viewport pixels)
    std::vector<ScreenSpaceEffect> effects;  // applied inside `shape`, in order
};
```

A layer owns a list of them (`DrawLayer::regions`) and so does the frame (`FrameDrawState::regions`).
Regions are **additive** — they sit alongside the whole-reach `DrawLayer::effect` / `FrameDrawState::postEffects`,
never replacing them; a frame using neither renders exactly as before. Inside the shape the effects apply;
outside, the source passes through untouched. Every other property — `kind`, `scope`, custom shader,
`edge`, the animation — still applies, *inside* the shape, identically for `RowDisplacement`, `Ripple`,
and a `Custom` shader.

```cpp
#include "retropp/draw_state.h"   // Region, ScreenSpaceEffect, ShapePoints

// A ripple confined to a circle, owned by the frame:
frame.regions.push_back(Region{
    .shape   = ShapePoints::circle({80, 72}, 30),
    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = 4, .frequency = 6,
                                  .center = {80, 72}, .decay = 2}}});

// A wave confined to the bottom half of one layer:
bg.regions.push_back(Region{
    .shape   = ShapePoints::rectangle({0, 72}, 160, 72),
    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 4,
                                  .frequency = 2.5f, .phase = t}}});
```

One region can carry **several** effects (applied in order, each seeing the last one's output), and one
effect drops into **many** regions with no duplication — the payoff of region-owns-effect.

A region's `shape` is a `ShapePoints` — a polygon of **ordered viewport-pixel vertices**, plus a `radius`
and a `Transform`. The points *are* the position. Containment is a signed-distance test, so one type
covers every shape, and `radius` rounds it:

| points | radius | shape |
|---|---|---|
| empty | — | no shape — the whole reach (the effects cover the region's whole reach) |
| 1 | r | a **circle** of radius r |
| 2 | r | a **capsule** (a thick line segment) |
| ≥ 3 | 0 | a **sharp polygon** — including arbitrary **concave** outlines |
| ≥ 3 | > 0 | a **rounded polygon** |

Named-constructor presets build them — `ShapePoints::circle / capsule / rectangle / roundedRectangle /
triangle / regularPolygon` — or build any polygon by hand: `shape.points = {{x0,y0}, {x1,y1}, …};`,
concave included. (Unbounded in the API; the GPU currently carries up to **64 vertices** and truncates a
longer polygon with a logged warning.)

**Inside or outside (`inverted()`).** By default the region is the **inside** of the shape. To confine the
effects to the **outside** instead, give the region the inverted shape — `shape.inverted()` returns a copy
with its inside and outside swapped, so the effects run everywhere *except* the shape:

```cpp
frame.regions.push_back(Region{
    .shape   = ShapePoints::circle({80, 72}, 30).inverted(),  // the effects run OUTSIDE the circle
    .effects = {someEffect}});
```

(`inverted()` toggles the underlying `ShapePoints::invert` flag; an empty shape ignores it. The same flag
flips a curve boundary, and a `Stencil`'s `EraseInside`/`EraseOutside` is the equivalent for its own shape.)

**Curved boundaries (`curve`).** A polygon's edges are straight. For a **smooth curved** boundary — exact
between control points, no facets at any zoom — give the shape a closed [`Curve`](curve.md) instead of
points, via `ShapePoints::fromCurve`:

```cpp
#include "retropp/curve.h"   // Curve

// A rounded boundary of four quadratic Béziers — a smooth "window" with no straight edges.
Curve outline = Curve::quadratic({80, 32}, {128, 32}, {128, 72});  // N → corner → E
outline.quadraticTo({128, 112}, {80, 112})                          // E → corner → S
       .quadraticTo({32, 112}, {32, 72})                            // S → corner → W
       .quadraticTo({32, 32}, {80, 32});                            // W → corner → N (back to the start)
region.shape = ShapePoints::fromCurve(outline, /*radius=*/0.0f);    // the boundary IS the curve
```

The curve is treated as a **closed loop** (the last segment's end joins the first's start); `radius` and
`transform` compose exactly as they do for a polygon. You can also set `shape.curve = {…segments…}`
directly; `points` is ignored whenever `curve` is non-empty.

| boundary segments | result |
|---|---|
| **Linear / Quadratic** | evaluated exactly — a true curved edge, no facets |
| **Cubic / Catmull-Rom** | sampled to a faceted polygon (renders correctly; the smooth edge is a follow-on) |

The GPU carries up to **32 curve segments** (a longer boundary truncates with a logged warning). The
`curve_region_demo` example confines a ripple to a quadratic boundary beside a sampled-polygon
approximation of the same outline, so the no-facets difference reads directly.

**Transform + motion.** `shape.transform` is a `Transform` — the same scale / stretch / skew / rotate /
perspective / translate type layers and sprites carry — composed on top of the shape, about any pivot.
And because the frame is recomputed every frame, you **move** a shaped effect just by giving its region a
new shape each frame:

```cpp
// a wavy "porthole" gliding left↔right; nothing else animates
const float cx = 80.0f + 56.0f * std::sin(t * 0.01f);
bg.regions.push_back(Region{.shape = ShapePoints::circle({cx, 72.0f}, 30.0f), .effects = {wave}});

// or hold the shape and warp it instead:
ShapePoints box = ShapePoints::rectangle({40, 42}, 80, 60);
box.transform = Transform::scale(1.5f, 1.0f, 80, 72);  // stretch about the centre
bg.regions.push_back(Region{.shape = box, .effects = {wave}});
```

The `region_shapes_demo`, `region_transform_demo`, `region_motion_demo`, `region_vertical_wave_demo`,
`region_ripple_demo`, and `region_showcase_demo` examples each demonstrate one facet; the
showcase combines them (top-half parallax, a vertical wave confined to the bottom half, a roaming
built-in ripple).

### Making a layer see-through (`Stencil`)

`Stencil` makes part of a layer **see-through** along a shape — nothing is erased or destroyed; the pixels
there go **transparent** so what's behind shows through. Unlike the other effects it is **not** confined
by a `Region`; it carries its **own** `shape` (its geometry *is* the effect), and from that one shape it
derives two sides — an **inside** and an **outside**, each a live, effect-able region.

```cpp
#include "retropp/draw_state.h"   // ScreenSpaceEffect, ScreenSpaceEffectKind, StencilMode, ShapePoints

// A round see-through window in a wall layer — the layers below show through it:
wall.effect = ScreenSpaceEffect{
    .kind    = ScreenSpaceEffectKind::Stencil,
    .stencil = StencilMode::EraseInside,            // the inside of the shape goes see-through
    .shape   = ShapePoints::circle({80, 72}, 30),
};

// Keep only a soft-edged patch of this layer solid; the rest goes see-through:
wall.effect = ScreenSpaceEffect{
    .kind    = ScreenSpaceEffectKind::Stencil,
    .stencil = StencilMode::EraseOutside,
    .feather = 6,                                   // soft edge, 6 px
    .shape   = ShapePoints::circle({80, 72}, 30),
};
```

`stencil` picks which side goes see-through; `feather` softens the boundary:

| `stencil` | see-through side | result |
|---|---|---|
| `EraseInside` (default) | the pixels **inside** the shape | a **window** — what's behind shows through it |
| `EraseOutside` | the pixels **outside** the shape | keeps **only the inside** solid; the rest is see-through |

| `feather` | edge |
|---|---|
| `0` (default) | hard — a crisp boundary |
| `> 0` | soft — a coverage ramp centered on the boundary, in shape-local pixels (the same units as `shape.radius`) |

**What shows through follows the effect's `scope`** — the same field that governs every per-layer effect:

- **Per-layer `Layer` (default).** Only **this layer** goes see-through, so the layers composited
  **below** it show through. An "x-ray window onto the layer behind."
- **Per-layer `Below`.** This layer **and everything beneath it** at this `z` go see-through, so the
  **backdrop** shows through — a true cut-out down to nothing.
- **Frame-level `postEffects`.** The whole composited frame goes see-through inside the shape → the
  **backdrop** shows through. (Scope does not apply at frame level.)

The shape is an ordinary `ShapePoints`, so **every shape and every curve works** — circle, capsule,
polygon (concave included), and a closed [`Curve`](curve.md) via `ShapePoints::fromCurve`; `radius` and
`transform` compose exactly as for a region, so a stencil shape rotates, stretches, and drifts the same way.

**Effects on each side (`insideRegion` / `outsideRegion`).** Because each side of `shape` is a live
region, you can apply effects to them — optional lists, empty by default (no extra work):

```cpp
wall.effect = ScreenSpaceEffect{
    .kind          = ScreenSpaceEffectKind::Stencil,
    .stencil       = StencilMode::EraseInside,
    .shape         = ShapePoints::circle({80, 72}, 30),
    .insideRegion  = {rippleEffect},   // ripple what shows THROUGH the see-through inside
    .outsideRegion = {waveEffect},     // wave the still-solid outside
};
```

A side effect runs on the **composited scene** at that point — so an effect placed on a see-through side
reaches the layers showing *through* it (the revealed content itself ripples), not merely the layer's own
transparent pixels. `insideRegion` is confined to `shape`, `outsideRegion` to `shape.inverted()`.

The `stencil_demo` example makes a drifting shape in a brick wall see-through over a vivid rear scene,
with a ripple inside and a wave outside: `A` toggles which side is see-through, `B` cycles the shape
(including a scalloped quadratic curve), `Up` toggles the feathered edge, `Down` switches `Layer` (reveal
the rear scene) versus `Below` (reveal the backdrop), and `Start` toggles the side effects on/off.

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

Your shader declares its **own** params in a cbuffer and writes only `main()`; the engine injects the
source texture + sampler:

```hlsl
// game/shaders/swirl.frag.hlsl — your own params, your own names
cbuffer Params : register(b1, space3) { float2 center; float angle; };
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    /* swirl uv about center by angle … then: */ return sampleSource(swirledUv);  // honours the edge policy
}
```

Two steps in C++:

```cpp
// 1) Once, at load time — register your fragment BY PATH. No uniform type, no ShaderVariants, no
//    generated-header include, no CMake rule: the build scans this source, sees the .hlsl path, compiles
//    + embeds it, reflects its cbuffer, and registers it. See rendering.md "Custom shader stages" for the
//    fragment contract + the one-time per-target build wiring.
PostProcessStageId swirl = renderer.registerPostProcessStage("game/shaders/swirl.frag.hlsl");

// 2) Per frame — attach it as an effect, setting YOUR shader's own params inline (the build surfaced
//    `.center` / `.angle` on ScreenSpaceEffect by reflecting the cbuffer). No uniform struct, no as_bytes.
frame.postEffects.push_back(ScreenSpaceEffect{
    .kind = ScreenSpaceEffectKind::Custom,
    .customShader = swirl,
    .center = {0.5f, 0.5f}, .angle = 0.3f});  // mutate live off the frame counter
```

`amplitude`/`frequency`/`phase`/`edge` are **not** read for a `Custom` effect — your shader + its own
params define its behaviour. `scope` still applies (a per-layer `Custom` effect is `Layer`-isolated or
a `Below` adjustment, exactly like a built-in — scope is a compositing decision the engine makes, not
the shader). Order is purely list order: a `Custom` entry and a built-in `RowDisplacement` in the same
`postEffects` run in whatever order you push them. The build reflects each shader's cbuffer into the
effect's inline fields and fills it per frame via a generated packer — so there is nothing to keep alive
across `renderFrame()` and no size to match by hand. The `custom_stage_test` exercises the reflection +
packing device-free; the custom path is for effects the built-in library doesn't cover (the useful
ones — ripple, the wave — are built-ins).

## Transforms

```cpp
#include "retropp/transform.h"   // Transform
```

Every layer **and every sprite** carries a **`Transform`** — an arbitrary 2D geometric transform (scale,
rotation, skew, translation, **and perspective**), about any pivot, with no per-console hardware ceiling.
The default is the identity, so content that sets no transform renders unchanged. The
transform applies to both the **tile** path (`DrawLayer::transform`) and the **sprite** path
(`DrawLayer::transform` *and* per-sprite `Sprite::transform` — see *Per-sprite transforms* below).

`Transform` is a value type that *is* a 3×3 projective matrix — you build it with named constructors and
compose with `.then()`:

```cpp
DrawLayer floor{};
floor.transform = Transform::rotation(degrees, 80.0f, 72.0f)   // yaw about the viewport centre …
                      .then(Transform::perspective(0.0f, -0.0045f));  // … then a receding Mode-7 floor
```

- `Transform::identity()`, `translation(dx,dy)`, `scale(sx,sy, pivotX,pivotY)`, `rotation(deg, pivotX,pivotY)`,
  `skew(kx,ky, pivotX,pivotY)`, `perspective(gx,gy)`. Pivots are in **content-local pixels** (a 160×144 layer
  rotates about its centre with pivot `(80, 72)`).
- `a.then(b)` applies `a` first, then `b`. The affine constructors are `constexpr`; `rotation` is not (it uses
  `std::sin`/`cos`). `perspective` adds the foreshortening that makes a rotating ground recede — the spinning
  "Mode-7" floor — done per-pixel on the GPU, **not** as any per-scanline hardware trick.

**The footprint edge.** A transform places the layer's `[0, size)` rectangle as a *finite footprint* (rotate it
and it's a diamond; the viewport area outside it is exposed). `DrawLayer::transformEdge` chooses what fills that
exposed area, using the same `DisplacementEdge` vocabulary as the wavy effect:

```cpp
floor.transformEdge = DisplacementEdge::Blank;    // default — transparent; the layers below show through
floor.transformEdge = DisplacementEdge::Stretch;  // clamp-to-edge; the footprint border smears outward
```

The area *behind* a perspective horizon (where the projection has no content) is always blank in both modes — it
is the sky above the floor, never a smear. (`transformEdge` is a tile-path footprint concept; a sprite is finite
geometry, so it has no footprint edge — see below.)

> **Note.** The transform path samples nearest (crisp pixels, the faithful look). Rotating or non-integer-scaling
> low-resolution pixel art shimmers and renders cells unevenly — that is inherent to nearest-sampling a
> transformed grid, not a defect. A tile layer wraps per its `TileContent::wrap` mode: the default `Repeat`
> tiles toroidally (author tiling content to tile seamlessly at the tilemap's dimensions), while `Blank` makes
> the map finite (it ends at the edge — no seam possible) and `Clamp` smears the edge tile.

### Per-sprite transforms

A `Sprite` carries its own `Transform`, applied in **sprite-local pixel space** — the `[0, width) × [0, height)`
rectangle of the sprite's own art. It composes with the sprite's layer transform: the sprite's transform runs
**first** (in sprite-local space), then the layer's transform (in viewport space), exactly the order a tile
layer's content travels. So a single sprite can spin about its own centre while its whole layer also rotates.

```cpp
Sprite s{};
s.size = AssetDimensions{16, 16};
s.transform = Transform::rotation(degrees, 8.0f, 8.0f);  // spin about ITS OWN centre (w/2, h/2)
// …and the layer it rides can carry its own transform too — the two compose:
spriteLayer.transform = Transform::rotation(slow, 80.0f, 72.0f);  // the whole layer orbits
```

- The pivot is whatever you encode — the engine imposes **no** default (pivot `(0,0)` is the sprite's top-left).
  Rotate an 8×8 about its centre with `Transform::rotation(deg, 4, 4)`; you compute `(w/2, h/2)` from the
  sprite's own size.
- **Perspective works on sprites too** — a sprite carrying `Transform::perspective(...)` foreshortens into a
  trapezoid (real projective geometry, perspective-correct texture). A sprite tilted so far that a corner passes
  *behind* the projection plane is clipped by the GPU's near plane — an extreme case; gentle foreshortening and
  all affine transforms are unaffected.
- **Flips compose independently.** `Sprite::flipX`/`flipY` mirror the *texture* (a fragment-side UV op), separate
  from the quad geometry — a flipped + rotated sprite mirrors its art and rotates its quad, both at once.
- Identity (the default) is a no-op — a plain axis-aligned sprite.

## Where to change things

- **Scale / rotate / skew / perspective a layer:** `DrawLayer::transform` (a `Transform`) + `transformEdge` for
  the exposed footprint — see Transforms above.
- **Transform one sprite (spin/scale/foreshorten it about its own pivot):** `Sprite::transform` — composes with
  the layer transform; see *Per-sprite transforms* above.
- **Stacking order / "walk behind":** set the layer's `z` — depth is `z` only.
- **Parallax:** give layers different `scroll` rates.
- **A see-through layer:** per-layer `alpha` (whole-layer translucency), or per-source index-hole
  transparency on the atlas (see [images-and-transparency.md](images-and-transparency.md)).
- **Day/night, fades, flashes:** `globalModifier` / `blend` (whole-frame).
- **The colour of the art itself:** the palette set + per-cell/per-sprite palette-select
  ([tiles-and-colour.md](tiles-and-colour.md)).
