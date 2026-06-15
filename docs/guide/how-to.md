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
- [Load a tileset from a PNG](#load-png)
- [Slice an atlas into addressable assets](#slice-atlas)
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

There is no priority flag — **depth is `z` alone**. To make the player pass behind a treetop, put the
treetop on a layer with a higher `z` than the player's layer:

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
hud.id     = "HUD";
hud.z      = 1000;            // above everything
hud.scroll = LayerScroll{0, 0};   // stays put while the world scrolls beneath it
```

The HUD can be tiles (a status bar) or sprites (icons, a cursor). Nothing else is special about it —
"HUD" is your meaning, not an engine role.

## Draw and animate a sprite <a id="animate-a-sprite"></a>

Put sprites on a layer with `SpriteContent`. Each `Sprite` names a position (in the layer's space,
before scroll), a size, and an atlas tile. Animate by changing the `tile` (or position) over time:

```cpp
Sprite hero{};
hero.x = heroX; hero.y = heroY;
hero.size = AssetDimensions::GameBoy8x16;
hero.tile = walkFrame;        // advance walkFrame on a timer for animation

std::array<Sprite, 1> sprites{hero};
layer.content = SpriteContent{spriteAtlas, std::span<const PaletteId>(palSet),
                              std::span<const Sprite>(sprites)};
```

Colour index 0 is transparent on sprites (the conventional OBJ-transparency), so sprite art reads
through to whatever is behind it. A sprite on a scrolling layer tracks the world; on a `{0,0}` layer
it stays fixed (a cursor). Details + flip/palette-select in [draw-state.md](draw-state.md). For
timed playback (looping / once / N-loops / palette-cycling) without hand-tracking the frame counter,
use the animation layer — see [Play an animation](#play-animation).

## Fade the screen / day-night tint <a id="screen-fade"></a>

Whole-frame colour is a frame-level **`ColorModifier`** (`out = clamp(in*mul + add)`) — a
post-composite transform, distinct from the per-pixel palette colouring. Fade to black by driving
`mul` from 1 → 0; tint for night by scaling the channels:

```cpp
frame.globalModifier.kind = ColorModifierKind::MultiplyAdd;
frame.globalModifier.mulR = frame.globalModifier.mulG = frame.globalModifier.mulB = fade; // 1→0 fade out
```

A cutscene flash is the sibling **`Blend`** (mix the frame toward a colour by `strength`). The default
of both is the identity, so a frame that sets neither is the faithful baseline byte-for-byte. See
[draw-state.md](draw-state.md#frame-level-colour-modifiers).

> **Photosensitivity:** flashes and fast full-frame colour swings drive luminance flicker. Keep them
> gentle and infrequent.

## Recolour a scene without new art <a id="recolour"></a>

Because colour is a palette applied at render time, you recolour by changing the palette — not the
art. Either upload a new palette and point the layer's set at it, or change which palette each cell
selects. A water-shimmer or palette-cycle is a per-frame palette swap, no new tiles and no shader
edit. See [tiles-and-colour.md](tiles-and-colour.md#where-to-change-things).

## Load a tileset from a PNG <a id="load-png"></a>

`loadPng` decodes an indexed/grayscale PNG into an index plane you feed straight to `uploadAtlas`:

```cpp
#include "retropp/image.h"

const LoadedImage img = loadPng("assets/tileset.png");
const AtlasId atlas = renderer.uploadAtlas(img.indices.data(), img.width, img.height);

// For a transparent colour (a hole that reveals the layer beneath), name its index on upload:
const AtlasId holed = renderer.uploadAtlas(img.indices.data(), img.width, img.height, /*transparentIndex=*/0);
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

Sprite frame{};
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
palette, duration }`. Because the frame names its own atlas, frames compose from a sliced sheet **and**
one-off images; because it names its own palette, **palette-cycling is the same mechanism** (hold the
slot, vary the palette). Durations are written as `std::chrono` at the call site:

```cpp
#include "retropp/animation.h"
using namespace std::chrono_literals;

const Animation walk{{
    {.label = "step0", .atlas = sheet.atlas, .slot = sheet[0], .palette = pal, .duration = 120ms},
    {.label = "step1", .atlas = sheet.atlas, .slot = sheet[1], .palette = pal, .duration = 120ms},
}};
```

When every frame comes from one sheet (the common case), `AtlasManifest::frame` is the shorthand — it
fills in `.atlas` + `.slot` so you give just a cell, a palette, a duration, and an optional label:

```cpp
const Animation walk{{ sheet.frame(0, pal, 120ms, "step0"),
                       sheet.frame(1, pal, 120ms, "step1") }};
```

Reach for the full literal only when a frame points at a *different* sheet (multi-sheet animation).

**How it plays is chosen when you play it**, not baked into the asset — a `PlaybackMode`:
`single()` (once, hold the last frame), `loopNTimes(n)`, `loopIndefinitely()` (the default), or
`playForDuration(2s)`. Hold a game-owned **`AnimationPlayer`**, advance it each sim tick, and thread
`current()` into draw state:

```cpp
// EngineConfig::setActive(config) at startup already fanned the configured cadence into
// AnimationPlayer::defaultTiming — so bare players inherit the engine cadence with nothing extra here.
// You can also set it directly anytime (e.g. AnimationPlayer::defaultTiming = loop.timing();) to set or
// override it without setActive.

AnimationPlayer p{.animation = &walk};        // bare — picks up defaultTiming; no per-player profile

loop.setTick([&](const InputState&) { p.advance(); });   // loops by default; pass a mode for others

loop.setRender([&](float) {
    const AnimationFrame& f = p.current();
    palSet[0]   = f.palette;                  // the frame's palette into the layer's set
    sprite.tile = f.slot.tile;                // the frame's art
    sprite.size = f.slot.dimensions;
    sprite.palette = 0;
    // … submit the layer …
});
```

Set `AnimationPlayer::defaultTiming` once at startup (from `loop.timing()`) and every bare player
inherits that cadence — no profile to pass per player; set `.profile` on a single player only to
override it. The player holds the elapsed-tick clock **in your object** — the engine keeps no playback
state and never ticks it (the immediate-mode invariant), so it's just a value you own like a `std::vector`.
`play()` / `pause()` / `stop()` / `restart()` / `seek(index_or_label)` control it; `finished()` reports
when a non-looping mode has ended. To **pin** a frame with no playback at all, index the animation
directly: `walk[2]` or `*walk.find("hurt")`. Want the pure form without the wrapper? Call
`frameAt(walk, elapsedTicks, profile, mode)` and own the tick counter yourself — both ship. A
multi-animation sheet (e.g. one row per facing) loads with `ContentKind::AnimationSeries` +
`framesPerAnimation`, then `manifest.group(g)` hands you each animation's frame slots. Worked end to
end — one button per playback mode — in
[`examples/animation_demo.cpp`](../../examples/animation_demo.cpp).

> **Photosensitivity:** keep frame and palette-cycle steps slow and avoid high-contrast flicker
> between adjacent frames.

## React to a button press (menus) <a id="button-press"></a>

The tick's `InputState` gives you held state **and edges**. Use edges for menus and "on press"
actions, held for movement:

```cpp
loop.setTick([&](const InputState& in) {
    if (in.justPressed(Button::A))    confirm();          // fires once, on the press
    if (in.justPressed(Button::Down)) moveCursor(+1);
    if (in.isHeld(Button::Right))     walk(+1);           // every tick while held
});
```

Edges are sim-tick-keyed, so they're deterministic and never double-fire from a fast display. Full
surface (button set, profiles, rebinding) in [input.md](input.md).

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
loop.setRender([&](float alpha) {
    frame.layers.clear();              // rebuild from scratch
    frame.layers.push_back(makeWorldLayer(state));
    frame.layers.push_back(makeHudLayer(state));
    renderer.renderFrame(frame, alpha);
});
```

`clear()` keeps the vector's capacity, so there's no per-frame heap churn — rebuilding is cheap.

**Retained** — build the layers once and mutate only what changed each frame. Good for mostly-static
scenes: a background you only scroll, a HUD that rarely changes. You don't re-describe unchanged
layers. This is what [`examples/controller_scrolling.cpp`](../../examples/controller_scrolling.cpp) does:

```cpp
// once, before the loop:
frame.layers.resize(1);
frame.layers[0].content = makeWorldLayer(state);

loop.setRender([&](float alpha) {
    frame.layers[0].scroll = LayerScroll{cam.x, cam.y};  // touch only what moved
    renderer.renderFrame(frame, alpha);
});
```

Both submit the same way and produce identical output; pick whichever fits how you think about a given
scene — you can even mix them (retain the static layers, rebuild a volatile one). There is no engine
"mode" to set: the choice lives entirely in your render code.

> **Lifetime note.** The renderer reads a layer's content spans (`cells`, `sprites`, `palettes`) *during*
> `renderFrame`. Whatever those spans point at must stay alive across the call — in the retained style,
> that means the backing arrays live as long as the frame does (declare them alongside it). The engine
> never copies your content; it references it.
