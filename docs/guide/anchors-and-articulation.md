# Anchors, pivots & articulation

How multi-part figures are built from plain sprites: named points published by a sprite (**anchors**),
the point a sprite is placed and transformed by (**the pivot**), and the per-sprite **z** that stacks
the parts within one layer. The engine's part is pure geometry — three resolvers and a sort key on
`Sprite`; the game owns what the points *mean* (joints, mounts, muzzles) and any chain logic.

```cpp
#include "retropp/draw_state.h"   // Sprite, Anchor, Point, orientPoint, spriteDrawOrder
```

## Contents

- [The model](#the-model)
- [Anchors: published points](#anchors-published-points)
- [The pivot: placement handle + transform centre](#the-pivot-placement-handle--transform-centre)
- [Attaching: pivot on an anchor](#attaching-pivot-on-an-anchor)
- [Flips and rotation: anchors follow the art](#flips-and-rotation-anchors-follow-the-art)
- [Stacking the parts: `Sprite::z`](#stacking-the-parts-spritez)
- [Anything attaches to an anchor](#anything-attaches-to-an-anchor)
- [Interpolation](#interpolation)
- [Gotchas](#gotchas)
- [Where to change things](#where-to-change-things)

## The model

One sentence: **a sprite pivots on a point; sprites publish points.**

- An **anchor** is a named point on a sprite's *art* — a socket, hinge, muzzle, emitter. Query it and
  get back where that point is *right now*, the sprite's transform applied.
- The **pivot** is the point `Sprite::x/y` place *and* the point the sprite's `transform` applies
  about — one point doing both jobs, so placement and hinge can never disagree.
- **Attachment is re-feeding**: each tick the game writes a parent's anchor into a child's `x/y`. No
  binding is held anywhere — the anchor query is a pure resolver like `frameAt(tick)` or
  `valueAt(t)`; the value flows wherever the game routes it.

There is no skeleton, no bone hierarchy, no parenting registry. A chain of joints is a few lines of
game code walking outward (see [Attaching](#attaching-pivot-on-an-anchor)); the engine answers one
question per query: *where is stored point K under this sprite's current placement.*

## Anchors: published points

```cpp
struct Anchor {
    std::string_view label;   // the durable address — also addressable by index
    float            x = 0.0f;  // ART-space px (the art as it sits on the sheet)
    float            y = 0.0f;
};
```

A sprite carries `std::span<const Anchor> anchors` — game-owned, like a layer's `cells`/`sprites`
spans. A `static constexpr` table is the natural shape:

```cpp
static constexpr Anchor kArmAnchors[] = {
    {.label = "root",  .x = 2.0f,  .y = 4.0f},   // socket end — pivots on the parent
    {.label = "elbow", .x = 13.0f, .y = 4.0f},   // far end — the next segment's socket
};
static_assert(!findDuplicateAnchorLabel(kArmAnchors));   // compile-time label-uniqueness check

Sprite arm{.key = "arm", .size = AssetDimensions{16, 8}, .anchors = kArmAnchors};
```

Two resolvers, each addressable by label **or** index:

```cpp
Point q = arm.anchorLocal("elbow");   // QUAD space: where the art feature sits on the placed quad
                                      //   (flips/rotation applied; before transform + placement)
Point p = arm.anchorAt("elbow");      // LAYER space: through transform + placement — where it IS now,
                                      //   rotation included. p == Point{x,y} + transform·(q − pivot)
Point i = arm.anchorAt(std::size_t{1});  // the same point by index
```

Labels are the durable address: reorder the table and an index silently names a different point,
while a label keeps naming the same one — and a *missing* label **throws** `std::out_of_range`
(so a typo fails loudly, the same posture as a missing save document or an unknown action id). A
duplicated label resolves to the first match; `findDuplicateAnchorLabel` is the `constexpr`
uniqueness check to `static_assert` over a fixed table.

`anchorAt` answers in the **layer's coordinate space** — the space `x`/`y` live in, which is what a
sibling sprite on the same layer consumes. It is a pure function of the sprite's own fields; it never
sees the layer's scroll or transform. Consumers on *other* layers map between layer spaces themselves
(the game owns both layers' scroll and transforms).

## The pivot: placement handle + transform centre

```cpp
Sprite s{.key = "wheel", .size = AssetDimensions{16, 16}};
s.pivot     = Point{8.0f, 8.0f};              // QUAD-space px; default {0,0} = the top-left
s.x         = 80;  s.y = 72;                  // x/y place THE PIVOT — the wheel's centre sits here
s.transform = Transform::rotation(angle);     // …and it spins about that same point
```

A local quad point `p` lands at `(x, y) + transform·(p − pivot)`. Under any origin-fixing transform
(the plain `rotation(θ)` / `scale` / `skew` forms) the pivot itself stays exactly at `(x, y)` — the
sprite is *held* by that point. With the default pivot `{0,0}`, `x/y` place the top-left corner and a
plain `rotation(θ)` turns about that corner.

A useful shorthand: **setting the pivot re-picks the sprite's origin** — the one reference point that
placement and transforms both use. It does not renumber the sprite's local coordinates (anchors and
art keep `{0,0}` at the sheet's top-left; an anchor at `(13, 4)` stays `(13, 4)` whatever the pivot
is) — it picks *which* of those local points is the handle, not where the ruler starts.

This differs from a pivot baked into the matrix (`Transform::rotation(deg, 8, 8)`): the baked form
only affects the spin and composes inside the transform, while `Sprite::pivot` affects the spin
**and** what point `x/y` place — the coupling attachment needs.

`Sprite::toLayer(Point)` is the same mapping for any quad-space point you already have — `anchorAt`
is `toLayer(anchorLocal(k))`.

## Attaching: pivot on an anchor

"Attach the forearm to the elbow" is literal: put the forearm's pivot at its own socket, and write
the upper arm's elbow anchor into the forearm's position. Rotation about the pivot is then rotation
about the joint *by construction* — the placement point and the hinge are the same point.

```cpp
// One articulated chain, walked outward each tick — angles accumulate.
auto attach = [](Sprite& child, const char* socket, const Sprite& parent, const char* joint,
                 float degrees) {
    child.pivot     = child.anchorLocal(socket);        // the hinge, mirrored with the art if flipped
    const Point p   = parent.anchorAt(joint);           // where the joint is right now
    child.x         = static_cast<int>(std::lround(p.x));
    child.y         = static_cast<int>(std::lround(p.y));
    child.transform = Transform::rotation(degrees);
};

attach(upperArm, "root",  body,     "shoulder", shoulderAngle);
attach(forearm,  "root",  upperArm, "elbow",    shoulderAngle + elbowAngle);
attach(claw,     "hinge", forearm,  "wrist",    shoulderAngle + elbowAngle + wristAngle);
```

That lambda is the ceiling of engine-adjacent convenience — the chain itself (which joint parents
which, how angles accumulate) is game logic and stays in the game. The working end-to-end example is
[`examples/articulation_demo/`](../../examples/articulation_demo/main.cpp).

## Flips and rotation: anchors follow the art

`flipX`/`flipY`/`rotation` are texture operations — they reorient which art pixel each quad pixel
reads, and the quad itself never moves. Stored points split accordingly:

- **Anchors live on the art and ride those ops.** Flip a leg and its socket mirrors with the pixels;
  `anchorLocal`/`anchorAt` return the mirrored position automatically — no per-facing anchor tables.
- **The pivot lives on the quad and ignores them.** It is the placement handle; the default `{0,0}`
  is always the quad's top-left, flipped art or not.

When the hinge *is* an art feature (it usually is), bridge the two with one line — re-read the anchor
into the pivot after setting the flip:

```cpp
claw.flipX = facingLeft;
claw.pivot = claw.anchorLocal("hinge");   // follows the flipped art
```

**What goes wrong without the bridge.** Suppose the claw's hinge is drawn at `(2, 4)` on 8-wide art
and you set `pivot = {2, 4}` by hand. Unflipped, that is exactly what `anchorLocal("hinge")` returns —
no difference. Flip the claw and they diverge: the drawn hinge now sits at `(6, 4)` within the quad,
but the pivot still names `(2, 4)` — which is now where the claw's *tip* is drawn. Nothing breaks
mechanically; the sprite is still placed by its pivot and still rotates about it — but it is the wrong
point, hinged through the wrong pixels:

1. **Placement attaches the wrong pixels.** `x/y` put quad point `(2, 4)` on the wrist, so the claw's
   tip sits welded to the joint while the drawn hinge floats off to the side — the joint looks
   dislocated.
2. **It swings about the wrong point.** Rotation stays welded to the pivot (that part always works),
   but the pivot is the tip — the claw sweeps a wrong arc and the drawn hinge *orbits* the wrist
   instead of staying pinned to it. The classic off-pivot wobble.

Set the pivot by hand and you own keeping it in sync with every flip/rotation state. Set it from
`anchorLocal` and it re-answers "where is the drawn hinge in my quad *right now*" each tick — the
desync is structurally impossible.

The underlying map is `orientPoint(p, w, h, rotation, flipX, flipY)` — a pure helper, exposed for
game-side use on any art-space point. For a non-square sprite, `Rot90`/`Rot270` transpose the art
extents, the same transpose the texture read makes.

## Stacking the parts: `Sprite::z`

An articulated figure lives on **one** layer, and its parts need explicit stacking that survives
whatever order the chain math produced them in — hand in front of arm, arm behind torso:

```cpp
claw.z  = 30;   // frontmost
fore.z  = 20;
upper.z = 10;
body.z  = 0;    // default
```

Within a sprite layer, sprites draw back-to-front by ascending `z`. Unlike `DrawLayer::z` it is
**not unique**: any values are legal (negative included), and equal-z sprites keep their submission
order (the sort is stable — deterministic, no frame-to-frame reshuffle). Nothing validates it and
nothing throws. The classic top-down Y-sort is one assignment per sprite: `s.z = s.y + s.size.height`
(sort by feet), no layer per row. `spriteDrawOrder(sprites)` is the pure ordering function if game
code wants the same order the renderer draws.

## Anything attaches to an anchor

`anchorAt` returns a `Point`, and points are the engine's common currency — the query's answer feeds
anything that takes a position, not just another sprite:

```cpp
const Point tail = body.anchorAt("tail");
Curve trail = Curve::quadratic(Vec2{tail.x, tail.y},          // a curve ORIGIN riding the body
                               Vec2{tail.x - 18.0f, tail.y - 6.0f},
                               Vec2{tail.x - 34.0f, tail.y + 10.0f});
```

A curve origin, a `PathWalker` frame of reference, a tween target, a particle-emitter position, a
region centre — the anchor never knows what consumes it; the game reads a point per tick and routes
it. Re-author the consumer from the fresh point each tick (the immediate-mode model), and the whole
construction rides the sprite's motion.

## Interpolation

The pivot is a continuous field: like position, alpha, and the transform, it eases between simulation
ticks under the automatic interpolator (keyed by the sprite's `key`), so a pivot that moves between
ticks eases as a moving hinge. `z` is discrete — it snaps to the current submission like the flips.

Re-fed chains stay glued mid-ease without any engine help: the interpolator lerps each part's
transform coefficient-wise and each part's position, and a matrix lerp applied to a fixed point
equals the lerp of the transformed points — parent and child ease along the same track. The residual
is the integer rounding of `x/y` (under one pixel). Anchor queries always answer from the tick state
the game holds; there is no render-time (eased) anchor query.

## Gotchas

- **`x/y` place the pivot, and the default pivot is the top-left.** A sprite that never sets `pivot`
  behaves exactly as its `x/y` read — top-left placement. Set a centre pivot and the same `x/y` now
  name the centre; adjust positions when introducing one.
- **A transform with its own translation moves the pivot off `(x, y)`.** The stays-at-position
  property holds for origin-fixing transforms (`rotation(θ)`, `scale(sx, sy)`, `skew(kx, ky)` — the
  no-pivot-argument forms). A matrix carrying a baked pivot or a `translation(...)` component adds
  that displacement, as authored.
- **Unknown anchor names throw.** `anchorLocal`/`anchorAt` throw `std::out_of_range` for a missing
  label or index — catch nothing; fix the name.
- **Anchor positions quantize at the write into `x/y`.** The queries are exact floats; `Sprite::x/y`
  are ints, so round once at the write (`std::lround`) — the sub-pixel remainder is below the
  viewport grid anyway.
- **A z change pops.** It is discrete: changing a part's rank mid-motion restacks it immediately,
  never a cross-fade.

## Where to change things

- **Publish a point on a sprite:** add it to the sprite's `Anchor` table (label it).
- **Hold a sprite by a different point:** `Sprite::pivot` (quad-space px).
- **Put a part in front of / behind its siblings on the same layer:** `Sprite::z`.
- **Whole-object stacking against other layers:** the layer's `z`, as ever — see
  [draw-state.md](draw-state.md).
- **Per-sprite spin/scale/foreshorten:** `Sprite::transform`, applied about the pivot — see
  [draw-state.md](draw-state.md#per-sprite-transforms).
- **Drive a part along a route instead of a joint:** [sprite-path.md](sprite-path.md) — paths and
  anchors compose as world-space points, not as a parenting system.
