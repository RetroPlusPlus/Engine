# How-to recipes

Task-oriented snippets for common things you'll want to do. Each assumes you have the four core
objects wired (see [getting-started.md](getting-started.md)) and an atlas + palette uploaded. The
reference for every type used here is in [draw-state.md](draw-state.md),
[tiles-and-colour.md](tiles-and-colour.md), and [input.md](input.md).

Recipes:

- [Scroll a background](#scroll-a-background)
- [Make a character walk behind scenery](#walk-behind)
- [Add a HUD that doesn't scroll](#fixed-hud)
- [Draw and animate a sprite](#animate-a-sprite)
- [Fade the screen / day-night tint](#screen-fade)
- [Recolour a scene without new art](#recolour)
- [Fill a shape with a live effect (an outline that does stuff inside)](#fill-a-shape)
- [Load a tileset from a PNG](#load-png)
- [Slice an atlas into addressable assets](#slice-atlas)
- [Play an animation (frames + palette over time)](#play-animation)
- [Tween a value over time (fades, ramps, transitions)](#tween-a-value)
- [React to a button press (menus)](#button-press)
- [Retained vs rebuilt frame state](#retained-vs-rebuilt-frame)

---

## Scroll a background <a id="scroll-a-background"></a>

A layer's `scroll` offsets where the tilemap is sampled. By default the map wraps toroidally
(`TileContent::wrap == TileWrap::Repeat`), so a small map tiles across an arbitrarily large scroll;
set `wrap` to `Clamp` or `Blank` for a finite map (see [draw-state.md](draw-state.md)). Move the
camera in your tick, apply it in your render:

```cpp
// tick: advance a camera from input or game state
cam.x += speed;

// render: point the layer at the camera
frame.layers[bgIndex].scroll = LayerScroll{cam.x, cam.y};
```

Different layers with different scroll rates give you **parallax** for free — a far layer scrolls
slower than a near one.

## Make a character walk behind scenery <a id="walk-behind"></a>

**Depth is `z` alone.** To make the player pass behind a treetop, put the treetop on a layer with a
higher `z` than the player's layer:

```cpp
playerLayer.z  = 10;
treetopLayer.z = 20;   // higher z = drawn later = in front of the player
```

Give the treetop layer per-source transparency (so its empty pixels don't block the player) — see
[load a tileset from a PNG](#load-png) and [images-and-transparency.md](images-and-transparency.md)
for the transparent-index upload. Want the player *in front* sometimes and *behind* other times?
Change which layer's `z` is higher per frame — `z` is ordinary per-frame data.

## Add a HUD that doesn't scroll <a id="fixed-hud"></a>

A HUD is just another layer — give it a high `z` (so it's on top) and a fixed `scroll` of `{0, 0}`
(so it ignores the camera) while your world layers scroll:

```cpp
hud.key  = "HUD";
hud.z      = 1000;            // above everything
hud.scroll = LayerScroll{0, 0};   // stays put while the world scrolls beneath it
```

The HUD can be tiles (a status bar) or sprites (icons, a cursor). Nothing else is special about it —
"HUD" is your meaning, not an engine role.

## Draw and animate a sprite <a id="animate-a-sprite"></a>

Put sprites on a layer with `SpriteContent`. Each `Sprite` names a position (in the layer's space,
before scroll), a size, an atlas tile, and its own sheet + palette. Animate by changing the `tile` (or
position) over time:

```cpp
Sprite hero{.key = "hero"};   // key is required — the stable identity the interpolator tracks
hero.x = heroX; hero.y = heroY;
hero.size    = AssetDimensions::GameBoy8x16;
hero.atlas   = spriteAtlas;     // the sprite names its own sheet…
hero.tile    = walkFrame;       // advance walkFrame on a timer for animation
hero.palette = heroPal;         // …and its own palette

std::array<Sprite, 1> sprites{hero};
layer.content = SpriteContent{std::span<const Sprite>(sprites)};
```

Sprite transparency is opt-in, exactly like the tile path: a sheet declares which palette indices are
holes at upload (`uploadAtlas(..., TransparentIndices::GameBoy)` for the conventional index-0 OBJ hole,
or `::of({n})`), and a palette entry with alpha 0 is a hole too — either way sprite art reads through to
whatever is behind it. A sprite on a scrolling layer tracks the world; on a `{0,0}` layer it stays fixed
(a cursor). Details + flip in [draw-state.md](draw-state.md). For
timed playback (looping / once / N-loops / palette-cycling) without hand-tracking the frame counter,
use the animation layer — see [Play an animation](#play-animation).

To fade a single sprite — a respawn blink, a ghosting enemy — set its `alpha` (`hero.alpha = 0.5f`,
default `1.0` opaque). It multiplies under the layer's own `alpha`, so one sprite fades while the rest of
its layer stays solid, and it eases smoothly between ticks under the automatic interpolator.

## Fade the screen / day-night tint <a id="screen-fade"></a>

Whole-frame colour is a screen-space effect: a **`ColorFill`** region (no shape → whole viewport) plus a
blend mode, distinct from the per-pixel palette colouring. Day/night is a **Multiply** `ColorFill` (the
fill tints/darkens the whole scene); a fade or a flash is a **Normal** `ColorFill` at `alpha = strength`:

```cpp
// Fade to black: a Normal black fill, alpha 0 → 1.
frame.regions.push_back(Region{
    .key     = "screenFade",                 // key is required — the first member of every Region
    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{0, 0, 0}}},
    .alpha   = fade});                       // 0 = clear, 1 = black

// Day/night: a Multiply fill (scene · tint). A cutscene flash is the same with .fill = white, Normal blend.
frame.regions.push_back(Region{
    .key     = "dayNight",
    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = nightTint}},
    .blend   = BlendMode::Multiply});
```

Multiply darkens; to brighten use Add (`scene + fill`). Confine any of these to a shape for a glow / shadow
/ spotlight. The runnable showcase is `examples/colour_effects_demo`. See
[draw-state.md](draw-state.md#whole-frame-colour).

> **Photosensitivity:** flashes and fast full-frame colour swings drive luminance flicker. Keep them
> gentle and infrequent.

## Recolour a scene without new art <a id="recolour"></a>

Because colour is a palette applied at render time, you recolour by changing the palette — not the
art. Either upload a new palette and rewrite the cells' `palette` handle to it, or point each cell at a
different already-uploaded palette. A water-shimmer or palette-cycle is a per-frame palette swap, no new
tiles and no shader edit. See [tiles-and-colour.md](tiles-and-colour.md#where-to-change-things).

## Fill a shape with a live effect (an outline that does stuff inside) <a id="fill-a-shape"></a>

To make a *shape* whose interior does something — a porthole that ripples, a heat-shimmer pond, a
scrying lens — put an effect in a `Region` for that shape. Think of it as a **fill**: the region's shape
is the area, its effect is *what fills it*. Three independent pieces compose with no glue:

```cpp
#include "retropp/curve.h"        // the shape (a closed curve — or use ShapePoints::circle/rectangle/…)
#include "retropp/draw_state.h"   // Region, ShapePoints, ScreenSpaceEffect

Curve outline = Curve::quadratic({80, 32}, {128, 32}, {128, 72});   // a rounded shape, smooth boundary
outline.quadraticTo({128, 112}, {80, 112})
       .quadraticTo({32, 112}, {32, 72})
       .quadraticTo({32, 32}, {80, 32});

ScreenSpaceEffect fill{ .kind = ScreenSpaceEffectKind::Ripple, .amplitude = 3.0f,
                        .frequency = 5.0f, .center = Point{80, 72}, .decay = 2.0f };
frame.regions.push_back(Region{ .key     = "lens",                           // key is required (Region's first member)
                                .shape   = ShapePoints::fromCurve(outline),  // the shape IS the fill area
                                .effects = {fill} });                        // the region owns the effect
```

What fills the shape is whichever effect you pick:

| effect | the interior becomes |
|---|---|
| `Ripple` / `RowDisplacement` | a warped copy of the scene — shimmer, heat haze, water |
| `ColorFill` | a flat colour (`.fill`), solid — or translucent via the `Region`'s `alpha` |
| a `Custom` shader | anything it draws — a texture, a pattern, a procedural fill (it need not sample the scene) |

A flat-colour fill is the built-in **`ColorFill`** (`.kind = ScreenSpaceEffectKind::ColorFill`;
[reference](draw-state.md#painting-a-colour-into-a-region-colorfill)) — set `.fill` to an `Rgba8` colour;
no custom shader needed. Make it a translucent tint by giving the owning `Region` an `alpha` below 1. Give
the region's shape new coordinates or a `shape.transform` each frame and the fill glides with it — a roaming
spotlight or scanner sweep. A region works on a layer too (`DrawLayer::regions`), confining the fill to one
layer.

**Drawing the outline itself.** To show a *stroke* — a coloured line *along* the shape's boundary instead
of a filled interior — give the region a `shape.strokeWidth` and fill the band with `ColorFill`: a stroked
`Region` + `ColorFill` **is** a drawn line (a ring, a border, or — with an open `fromCurve` — an arbitrary
curved path). No walking the curve into sprites by hand. See
[stroke / outline](draw-state.md#confining-an-effect-to-a-shape-region) and
[`ColorFill`](draw-state.md#painting-a-colour-into-a-region-colorfill).

**A fill *adds*; it does not make a layer see-through.** Inside the shape you see the scene **plus** the
effect — the layers underneath are untouched. To instead *reveal what is below* a layer along a shape — a
true see-through window or cutout — use the **`stencil()`** helper (a `Transparency` effect in a region;
see [draw-state.md](draw-state.md#making-a-layer-see-through-the-stencil-helper)). In short: a region
**fills** a shape, `stencil()` makes a shape **see-through**.

See [draw-state.md](draw-state.md#confining-an-effect-to-a-shape-region) for the full region surface
(shapes, `radius`, `transform`, curved boundaries) and [curve.md](curve.md) for authoring the shape.

> **Photosensitivity:** keep the fill effect gentle and slow — a small amplitude, a low frequency. Fast
> or high-contrast distortion inside a region still drives luminance flicker.

## Load a tileset from a PNG <a id="load-png"></a>

`loadPng` decodes an indexed/grayscale PNG into an index plane you feed straight to `uploadAtlas`:

```cpp
#include "retropp/image.h"

const LoadedImage img = loadPng("assets/tileset.png");
const AtlasId atlas = renderer.uploadAtlas(img.indices.data(), img.width, img.height);

// For a transparent colour (a hole that reveals the layer beneath), name its index on upload:
const AtlasId holed = renderer.uploadAtlas(img.indices.data(), img.width, img.height, TransparentIndices::of({0}));
```

Author art as **indexed or grayscale** PNGs (the faithful console format); supply colour separately
via `uploadPalette`, or use the PNG's embedded palette (`img.palette`). Full routing + transparency
rules in [images-and-transparency.md](images-and-transparency.md).

## Slice an atlas into addressable assets <a id="slice-atlas"></a>

When a PNG holds a *grid* of tiles or sprite frames, `loadAtlas` uploads it once and hands back an
**`AtlasManifest`** — the atlas handle plus one **`AssetSlot`** per carved sub-asset (its top-left
atlas cell + dimensions), so you never hand-compute tile indices. Pick the asset size, a
**`ContentKind`** (`Single` / `Tileset` / `SpriteSeries`), and a **`ReadOrder`**:

```cpp
#include "retropp/renderer.h"   // AtlasManifest; ContentKind / ReadOrder come from image.h

// A 16-wide strip of 8×8 frames, read left-to-right (the default order):
const AtlasManifest walk =
    renderer.loadAtlas("assets/hero_walk.png", AssetDimensions::GameBoy8x8, ContentKind::SpriteSeries);

Sprite frame{.key = "hero"};   // key is required (see Draw and animate a sprite)
frame.size = walk[walkFrame].dimensions;   // walk[i] is the i-th carved slot
frame.tile = walk[walkFrame].tile;         // advance walkFrame on a timer
```

`Single` yields one slot covering the whole image; `Tileset` and `SpriteSeries` carve a grid
identically (the names just read your intent at the call site). The **read order** has all eight
permutations as named presets — `ReadOrder::LeftRightThenDown` (western default),
`TopBottomThenRight` (column-major), and the rest — for art laid out in non-western orders. If a sheet
has room for more cells than its art uses, pass a `count` so you get exactly the real frames:
`loadAtlas(path, size, ContentKind::SpriteSeries, ReadOrder::LeftRightThenDown, /*count=*/5)`. A
trailing partial cell is dropped (full cells only); a degenerate request yields an empty manifest.
To re-slice the same uploaded atlas in a different order/count without re-uploading, call the pure
`sliceLayout(...)` directly. Full reference in
[images-and-transparency.md](images-and-transparency.md#slicing).

## Play an animation (frames + palette over time) <a id="play-animation"></a>

The hand-rolled "advance `walkFrame` on a timer" above works, but `animation.h` removes the
bookkeeping. An **`Animation`** is a list of **`AnimationFrame`**s — each `{ label, atlas, slot,
palette, duration }` — and a game-owned **`AnimationPlayer`** plays it: advance it each sim tick and
thread `current()` into draw state.

```cpp
#include "retropp/animation.h"
using namespace std::chrono_literals;

// every frame from one sliced sheet → the AtlasManifest::frame shorthand
const Animation walk{{ sheet.frame(0, pal, 120ms, "step0"),
                       sheet.frame(1, pal, 120ms, "step1") }};

AnimationPlayer p{.animation = &walk};                  // inherits the engine cadence (setActive)

loop.setTick([&](const InputState&) { p.advance(); });  // loops by default
loop.setRender([&](float) {
    const AnimationFrame& f = p.current();
    sprite.atlas = f.atlas;       sprite.size = f.slot.dimensions;
    sprite.tile  = f.slot.tile;   sprite.palette = f.palette;   // the frame names its own sheet + palette
    // … submit the layer …
});
```

**How it plays is chosen when you play it** — pass `single()`, `loopNTimes(n)`, or `playForDuration(2s)`
to `advance()` (default `loopIndefinitely()`). Palette-cycling is the same type: vary `.palette`, hold
`.slot`. The full reference — the data model, the pure resolver, multi-clip sheets, and the player's
`play`/`pause`/`stop`/`seek`/`finished` controls — is in **[animation.md](animation.md)**, worked end to
end (one button per playback mode) in
[`examples/animation_demo.cpp`](../../examples/animation_demo.cpp).

> **Photosensitivity:** keep frame and palette-cycle steps slow and avoid high-contrast flicker
> between adjacent frames.

## Tween a value over time (fades, ramps, transitions) <a id="tween-a-value"></a>

Animations resolve elapsed ticks → *which frame*; **`tween.h`** resolves elapsed ticks → *a value* —
a layer's `alpha`, a `ColorFill` channel, an effect parameter, a transform angle. Same shape as
animations: the engine gives a pure resolver, you own a **`TweenPlayer<T>`** cursor and write the result
into draw state. A **`Tween<T>`** is a start anchor `from` plus a list of timed, eased moves (`of` for
the first, `then()` to chain) — so a **yoyo is just a 2-segment looped track**, no special mode:

```cpp
#include "retropp/tween.h"
using namespace std::chrono_literals;

// fade out and back, forever
const Tween<float> fade = Tween<float>::of(1.0f, 0.0f, 1s, Easing::InOutSine)
                                       .then(1.0f, 1s, Easing::InOutSine);

TweenPlayer<float> fader{.tween = &fade};

loop.setTick([&](const InputState&) { fader.advance(); });   // loops by default
loop.setRender([&](float) {
    upperLayer.alpha = fader.value();                        // write the value into any sink
    // … submit …
});
```

**An effect or shader parameter is the same sink** — write `value()` into a built-in effect's amplitude
or a custom shader's reflected param exactly like a layer's `alpha`. The full reference — the curve set
(`Easing`), the pure resolver, the playback modes, and the player's controls — is in
**[tween.md](tween.md)**, worked end to end (layer alpha + dusk ramp) in
[`examples/tween_demo.cpp`](../../examples/tween_demo.cpp).

> **Photosensitivity:** keep ramps slow and monotonic; the built-in easings never flicker, but a fast
> yoyo on a high-contrast value still can — pace it in seconds, not frames.

## React to an action press (menus) <a id="button-press"></a>

The tick's `InputState` gives you held state **and edges**, keyed by your own action enum. Use
edges for menus and "on press" actions, held for movement:

```cpp
enum class Action : std::uint8_t { Confirm, Down, Right };
// at startup: bind each action to its sources, then platform.setActions(map)

loop.setTick([&](const InputState& in) {
    if (in.justPressed(Action::Confirm)) confirm();       // fires once, on the press
    if (in.justPressed(Action::Down))    moveCursor(+1);
    if (in.isHeld(Action::Right))        walk(+1);        // every tick while held
});
```

Edges are sim-tick-keyed, so they're deterministic and never double-fire from a fast display. Full
surface (the action map, sources, presets, per-family rows) in [input.md](input.md).

## Retained vs rebuilt frame state <a id="retained-vs-rebuilt-frame"></a>

Each frame the renderer draws whatever `FrameDrawState` you hand it. **How you produce that state is
your choice** — the engine holds no opinion and no persistent per-layer state of its own. Two styles,
both fully supported, both shown in the examples:

**Rebuilt (immediate-mode)** — clear the layer stack and rebuild it every frame. Simplest mental
model: there's no state to keep in sync, the frame is purely a function of current game state. Good
when layers come and go a lot, or you just prefer stateless assembly. This is what
[`examples/beach_demo.cpp`](../../examples/beach_demo.cpp) and
[`examples/layer_transparency_demo.cpp`](../../examples/layer_transparency_demo.cpp) do:

```cpp
loop.setRender([&]() {
    frame.layers.clear();              // rebuild from scratch
    frame.layers.push_back(makeWorldLayer(state));
    frame.layers.push_back(makeHudLayer(state));
    renderer.renderFrame(frame);
});
```

`clear()` keeps the vector's capacity, so there's no per-frame heap churn — rebuilding is cheap.

**Retained** — build the layers once and mutate only what changed each frame. Good for mostly-static
scenes: a background you only scroll, a HUD that rarely changes. You don't re-describe unchanged
layers. This is what [`examples/controller_scrolling.cpp`](../../examples/controller_scrolling.cpp) does:

```cpp
// once, before the loop:
frame.layers.push_back(DrawLayer{.key = "world"});   // key is required — no default constructor
frame.layers[0].content = makeWorldContent(state);

loop.setRender([&]() {
    frame.layers[0].scroll = LayerScroll{cam.x, cam.y};  // touch only what moved
    renderer.renderFrame(frame);
});
```

Both submit the same way and produce identical output; pick whichever fits how you think about a given
scene — you can even mix them (retain the static layers, rebuild a volatile one). There is no engine
"mode" to set: the choice lives entirely in your render code.

> **Lifetime note.** The renderer reads a layer's content spans (`cells`, `sprites`) *during*
> `renderFrame`. Whatever those spans point at must stay alive across the call — in the retained style,
> that means the backing arrays live as long as the frame does (declare them alongside it). The engine
> never copies your content; it references it.
