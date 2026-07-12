# Blend modes

A compositing **container** — a `Sprite`, a `Region`, a `DrawLayer`, or the whole `FrameDrawState` —
carries a `BlendMode` beside its `alpha`. `alpha` is *how much* the container contributes; `blend` is *how*
its pixels combine with what they sit on. The default, `Normal`, is the alpha-over of a layer stack; the
other five are the standard separable blend operators a retro look reaches for — additive glows,
multiplicative shadows, screen bloom, a halved-average translucency.

Blend is a property of the **container that owns the pixels**, never of a screen-space effect. An effect is
a colour *source*; the region / layer / frame that holds it decides how that source merges.

## The modes

```cpp
enum class BlendMode : std::uint8_t { Normal, Add, Subtract, Multiply, Screen, Half };
```

For a source colour `src` combining over a destination `dst`, each mode applies a per-channel operator
`B(dst, src)`:

| Mode | `B(dst, src)` | Looks like |
|---|---|---|
| `Normal` | `src` | plain alpha-over (the default; unchanged output) |
| `Add` | `dst + src` | glows, fire, light — brightens |
| `Subtract` | `dst − src` | darkens hard |
| `Multiply` | `dst · src` | shadows, tints — darkens |
| `Screen` | `1 − (1 − dst)(1 − src)` | bloom — lifts toward light |
| `Half` | `(dst + src) / 2` | a halved average — translucency |

The full combine, source-alpha-weighted and clamped, is:

```
out.rgb = clamp( (1 − src.a)·dst.rgb + src.a·B(dst.rgb, src.rgb) )
out.a   = clamp( src.a + dst.a·(1 − src.a) )          // standard over alpha, mode-independent
```

`Normal` reduces to standard alpha-over, so a container left at the default is unchanged.

## Where blend lives

Each container carries the mode beside its `alpha`:

```cpp
struct Sprite          { /* … */ float alpha; BlendMode blend = BlendMode::Normal; /* … */ };
struct Region          { /* … */ float alpha; BlendMode blend = BlendMode::Normal; };
struct DrawLayer       { /* … */ float alpha; BlendMode blend = BlendMode::Normal; /* … */ };
struct FrameDrawState  { /* … */             BlendMode blend = BlendMode::Normal; /* … */ };
```

- **`Sprite::blend`** — how the sprite's own pixels combine over its container's image (see
  [Per-sprite blend](#per-sprite-blend)).
- **`Region::blend`** — how the region's effects combine over the scene inside its shape.
- **`DrawLayer::blend`** — how the whole layer composites over the layers beneath it.
- **`FrameDrawState::blend`** — how the frame's whole-frame `postEffects` / `regions` combine over the
  composited image.

A frame-level region (in `FrameDrawState::regions`) uses its own `Region::blend`, like any region.

## Scope: what a grade combines over

A whole-container colour grade is a `ColorFill` under a container blend — a `Region` carrying one `ColorFill`,
its `blend` the grade and its `alpha` the strength. A region with **no shape** covers its whole container, so
an empty-shape region on a layer or the frame is the whole-layer / whole-frame colour modifier: day/night, a
tint, a flash, a fade.

Where that grade lands is the effect's `scope` (`ScreenSpaceEffectScope`, on a per-layer effect):

- **`Layer`** — the grade combines over the layer's **own content**, in place. The layer's transparent pixels
  stay transparent: a hole in the graded layer reveals the layers below *ungraded*. Tint one plane (a sprite
  flashes, a single layer goes to dusk) without touching the rest of the scene.
- **`Below`** — the grade combines over the **accumulated image** at the layer's z: this layer's content and
  everything beneath it, coherently, including *through* the layer's holes. A wash that sits over the world (a
  whole-scene day/night, a screen flash) while layers above the grade's z ride over it untouched.

A frame-level grade (`FrameDrawState::regions` / `postEffects` under `FrameDrawState::blend`) is inherently
whole-frame — it combines over the finished composite, the reach of a `Below` grade at the top of the stack.

The grade math is `applyBlendMode` at both scopes; only the **destination it reads** changes — the layer's own
pixels (`Layer`) or the composited scene (`Below`). So the same fill and mode give, over an opaque pixel:

| Mode | Over a scene pixel `dst` (fill `f`, region alpha `a`) |
|---|---|
| `Normal` | `(1−a)·dst + a·f` — a flash/fade toward `f` |
| `Multiply` | `(1−a)·dst + a·(dst·f)` — a tint / shadow / day-night (`f`>1 via `fillIntensity` brightens) |
| `Add` | `(1−a)·dst + a·clamp(dst+f)` — a glow |
| `Screen` | `(1−a)·dst + a·(1−(1−dst)(1−f))` — bloom |
| `Subtract` | `(1−a)·dst + a·clamp(dst−f)` — a hard darken |
| `Half` | `(1−a)·dst + a·((dst+f)/2)` — a halved wash |

A transparent pixel under a `Layer` grade is left transparent (there is no own-content there to grade).

## Per-sprite blend

A `Sprite` carries `blend` beside its `alpha`, so a single sprite grades against its container's image the
same way a layer or region does — `alpha` is how much the sprite contributes, `blend` is how. A `Multiply`
sprite is a shadow decal that darkens the scene under it; an `Add` sprite is a flare or glow that lifts it;
`Screen` is a soft bloom. The grade uses `applyBlendMode` over the sprite's own opaque pixels, so it lands
on the sprite's silhouette — its transparent texels contribute nothing.

```cpp
// A soft shadow decal under a character, and a bright muzzle flare, in one sprite layer:
Sprite shadow{.key = "shadow", .x = 64, .y = 96, .atlas = decals, .tile = kShadow,
              .palette = greys, .alpha = 0.7f, .blend = BlendMode::Multiply};
Sprite flare {.key = "flare",  .x = 80, .y = 60, .atlas = decals, .tile = kFlare,
              .palette = warm,  .blend = BlendMode::Add};
```

### The container: what a sprite grades against

`blend` grades a sprite against its **compositing container** — the image the sprite layer draws into at
that moment:

- A sprite in an ordinary layer (one that composites straight into the scene) grades against the
  **accumulated scene beneath it**, plus this layer's earlier-`z` sprites already drawn. A `Multiply`
  shadow there darkens the world under the sprite.
- A sprite in a layer that is itself composited in isolation — one carrying its own `DrawLayer::blend` or a
  per-layer effect chain — grades against that **layer's own image**: the within-layer content beneath the
  sprite, and nothing else. A non-Normal sprite over the layer's transparent area has no backdrop to grade
  against, so it contributes nothing there — a shadow needs a surface. Put the sprites that a blended
  sprite should darken or light in the **same layer**, beneath it in `z`.

Discrete like the flips and `z`, `blend` snaps to each submission — it is never interpolated. Ease *toward*
a blend by easing the sprite's `alpha` with a [`Tween`](tween.md), or by resubmitting.

Blend runs of same-mode sprites cost one composite pass per run — the cost scales with how many distinct
blend modes a layer's sprites use in `z` order, never with the sprite count. A layer whose sprites are all
`Normal` (the default) takes the plain instanced draw, unchanged.

## Per-sprite effects and regions

A `Sprite` also carries an effect **chain** and a **regions** list — the same surface a `DrawLayer`, a
`Region`, and the frame carry, applied to one sprite:

```cpp
struct Sprite {
    // … placement, art, alpha, blend, anchors …
    std::vector<ScreenSpaceEffect> effects;  // whole-silhouette colour transforms, in list order
    std::vector<Region>            regions;   // confined effects: a quad-space shape ∩ the silhouette
};
```

Both default empty; a sprite that sets neither composites exactly as a plain sprite. The effect domain is
the **sprite**, not the atlas texture behind it — the art sits in an infinite transparent field, so an
effect lands on the sprite's visible pixels and its transparent texels stay clear.

`effects` transforms the sprite's own pixel in list order: `ColorFill` replaces the colour, `Gleam` adds a
luminance-keyed sheen, `Transparency` makes the whole silhouette see-through. `regions` then applies, each
`Region` grading its effects over the sprite's pixel by its own `alpha` + `blend`, confined to its `shape`
intersected with the silhouette. A region `shape` is read in the sprite's **quad space** (the
pivot / origin / anchor space, sprite-local pixels) and rides the sprite's transform like the art does; an
empty shape covers the whole silhouette. Region shapes use the polygon path — `circle` / `capsule` /
`rectangle` / any polygon, with `radius` / `strokeWidth` / `inverted()`; a curved sprite-region boundary is
not evaluated inline and is skipped with a warning.

```cpp
Sprite hero{.key = "hero", .x = 60, .y = 72, .atlas = sheet, .tile = kHero, .palette = pal};

// A damage flash over the whole sprite — a white ColorFill region eased in and out via alpha:
Region flash{.key = "flash",  // empty shape ⇒ the whole silhouette
             .effects = {{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{255, 255, 255, 255}}},
             .alpha = damage};  // damage ∈ [0,1], a Tween drives it

// A Multiply shadow tint on the lower half only (quad-space rectangle over y ∈ [8,16] of a 16px sprite):
Region shade{.key = "shade", .shape = ShapePoints::rectangle(Point{0, 8}, 16, 8),
             .effects = {{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{90, 90, 130, 255}}},
             .blend = BlendMode::Multiply};

// A wake glow keyed to the sprite's own brightness:
hero.effects = {{.kind = ScreenSpaceEffectKind::Gleam, .sweep = 0.5f, .width = 0.6f, .gain = 1.5f}};
hero.regions = {flash, shade};
```

`effects` applies before `regions`, matching a layer's effects-then-regions order. Effect parameters are
per-tick data — the interpolator never eases them; drive a flash by easing a value with a [`Tween`](tween.md)
and resubmitting. `fillIntensity > 1` on a `Multiply` region brightens (the intermediates carry the
headroom). Evaluation is inline in the sprite fragment — no added render passes, so the pass count stays
flat in the sprite count. The CPU mirror is `retropp::evalSpriteFxRecords`.

## Examples

A translucent layer — averaged with the scene, no per-pixel alpha:

```cpp
DrawLayer glassPane;
glassPane.key = "glassPane";
glassPane.blend = BlendMode::Half;          // (scene + pane) / 2
glassPane.content = /* … */;
frame.layers.push_back(glassPane);
```

An additive glow and a multiply shadow, each a `Region` with a `ColorFill` source:

```cpp
const ScreenSpaceEffect warm{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{90, 60, 20}};
const ScreenSpaceEffect grey{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{120, 120, 130}};

frame.regions.push_back(Region{.shape   = ShapePoints::circle(Point{120, 48}, 26),
                               .effects = {warm},
                               .blend   = BlendMode::Add});       // a glow that brightens the scene

frame.regions.push_back(Region{.shape   = ShapePoints::rectangle(Point{16, 84}, 56, 40),
                               .effects = {grey},
                               .blend   = BlendMode::Multiply});  // a soft shadow that darkens it
```

A whole-frame overlay — a `ColorFill` `postEffect` combined over the composited image with the frame mode:

```cpp
frame.postEffects.push_back(
    ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{34, 44, 78}});
frame.blend = BlendMode::Screen;            // lift the whole scene gently toward light
```

A `ColorFill` is a pure source colour; the owning container's blend decides whether it replaces (`Normal`),
multiplies into a shadow, adds into a glow, or screens into a bloom — one fill colour, every grade. To
**darken** use `Multiply` (`scene · fill`); to **brighten** use `Add` (`scene + fill`), `Screen`, or a
`Multiply` whose fill exceeds 1 (see `fillIntensity` below). This is how whole-frame colour looks are built —
day/night is a Multiply `ColorFill`, a cutscene flash a `Normal` white `ColorFill` at `alpha = strength`, a
fade a `Normal` black one; see [draw-state.md](draw-state.md#whole-frame-colour).

### Multiplicative exposure — `Multiply` that brightens

`Multiply` is `scene · fill`. With a fill in [0,1] it can only darken. `ColorFill::fillIntensity` scales the
fill past 1, so a `Multiply` becomes a multiplicative *exposure* that lifts the scene while preserving its
contrast — what `Add` and `Screen` cannot do (they change the operator). The offscreen pipeline is float16, so
a fill above 1 survives the effect→blend round-trip; the final blit to the screen clamps back into range.

```cpp
// Brighten the whole frame 1.5× — every pixel scaled up, highlights preserved (a daytime/overexposed look):
frame.postEffects.push_back(ScreenSpaceEffect{.kind          = ScreenSpaceEffectKind::ColorFill,
                                              .fill          = Rgba8{255, 255, 255},
                                              .fillIntensity = 1.5f});
frame.blend = BlendMode::Multiply;
```

`fillIntensity` defaults to 1 (the plain fill) and only has visible effect through a headroom-carrying blend
(`Multiply`, `Screen`, `Add`); below 1 it dims the fill, 0 paints black.

## The CPU mirror

`retropp::applyBlendMode(Vec4 dst, Vec4 src, BlendMode)` (in `postprocess.h`) is the `constexpr` authority
the compositor shaders reproduce. It is `static_assert`-anchored and unit-tested, so the blend math is
verifiable without a GPU:

```cpp
// Multiply grades a 50%-grey fill over a scene to a half-strength shadow:
Vec4 shadow = applyBlendMode(scene, Vec4{0.5f, 0.5f, 0.5f, 1.0f}, BlendMode::Multiply);
```

## Notes

- A container left at `BlendMode::Normal` (the default everywhere) renders exactly as it did without the
  mode — the plain alpha-over path.
- Operators clamp to `[0, 1]`: `Add` saturates at white, `Subtract` floors at black.
- Alpha composites with the standard over rule regardless of mode — the blend grades colour, not coverage.
  A partially transparent source contributes proportionally (a hole in a blended layer reveals what is
  beneath).
- Blend never appears on `ScreenSpaceEffect`. To grade an effect, set the blend on the `Region` that owns
  it.

## Where to change

- The modes and their math: `BlendMode` in `draw_state.h`; the operator and combine in
  `retropp::applyBlendMode` / `retropp::blendChannel` (`postprocess.h`). The shaders
  (`blend.frag.hlsl`, `region_select.frag.hlsl`, `region_select_curve.frag.hlsl`) mirror that math —
  change the CPU helper and the shaders together.

## See also

- [draw-state.md](draw-state.md) — the `Region` / `DrawLayer` / `FrameDrawState` containers, `alpha`, and
  the `ColorFill` source colour.
- [rendering.md](rendering.md) — the compositor and the post-process chain blend lands in.
