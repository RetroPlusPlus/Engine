# Blend modes

A compositing **container** — a `Region`, a `DrawLayer`, or the whole `FrameDrawState` — carries a
`BlendMode` beside its `alpha`. `alpha` is *how much* the container contributes; `blend` is *how* its
pixels combine with what they sit on. The default, `Normal`, is the alpha-over of a layer stack; the other
five are the standard separable blend operators a retro look reaches for — additive glows, multiplicative
shadows, screen bloom, a halved-average translucency.

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
struct Region          { /* … */ float alpha; BlendMode blend = BlendMode::Normal; };
struct DrawLayer       { /* … */ float alpha; BlendMode blend = BlendMode::Normal; /* … */ };
struct FrameDrawState  { /* … */             BlendMode blend = BlendMode::Normal; /* … */ };
```

- **`Region::blend`** — how the region's effects combine over the scene inside its shape.
- **`DrawLayer::blend`** — how the whole layer composites over the layers beneath it.
- **`FrameDrawState::blend`** — how the frame's whole-frame `postEffects` / `regions` combine over the
  composited image.

A frame-level region (in `FrameDrawState::regions`) uses its own `Region::blend`, like any region.

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
