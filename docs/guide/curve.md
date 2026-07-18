# Curve

A small **curve primitive** — one composable value type for a smooth 2-D shape, with the four queries a
consumer reaches for: the point at a parameter, the direction of travel, the arc length (and the point a
given distance along), and the signed distance from an arbitrary point to the curve. It is pure data plus
pure functions: a `Curve` carries no playback state and the engine never ticks it into anything — you
hold a `Curve` and call a query, the same relationship [`Transform`](draw-state.md), [`Tween`](tween.md),
and [`Animation`](animation.md) keep.

A `Curve` is the **shape** (a parameter → a point). A timing driver — a [`Tween`](tween.md) with an
`Easing` — is the **speed along** it. They are orthogonal and compose: `curve.atDistance(driver(time))`
walks a curve under any easing, and the straight-line case (`Curve::line`) equals a `Tween<Vec2>` lerp
exactly. Curve answers *where*; the tween answers *how fast*.

```cpp
#include "retropp/curve.h"   // Curve, CurveSegment, CurveDegree (+ the constexpr per-segment evaluators)
```

Coordinates are `Vec2` in viewport pixels (top-left origin), the same space sprites and layers use.

## Contents

- [The model](#the-model)
- [Authoring — the three front doors](#authoring--the-three-front-doors)
- [The queries](#the-queries)
- [Reusing the arc-length table](#reusing-the-arc-length-table)
- [Worked example — walking a path at constant speed](#worked-example--walking-a-path-at-constant-speed)
- [Where to change things](#where-to-change-things)

## The model

Internally a `Curve` is a list of mixed-degree Bézier **segments** plus an open/closed flag. You author
through the named constructors below — you rarely build a `CurveSegment` by hand — but the segment is the
data the queries evaluate:

```cpp
enum class CurveDegree : std::uint8_t { Linear = 1, Quadratic = 2, Cubic = 3 };

struct CurveSegment {
    Vec2        p0, p1, p2, p3;          // control points; `degree` says how many are live
    CurveDegree degree = CurveDegree::Cubic;
    //  Linear    = p0 → p1                  (p2, p3 ignored)
    //  Quadratic = p0, p1 (control), p2     (p3 ignored)
    //  Cubic     = p0, p1, p2 (controls), p3
};

struct Curve {
    std::vector<CurveSegment> segments;
    bool                      closed = false;   // closed = the last end joins the first start

    std::size_t count() const;   // number of segments
    bool        empty() const;
    // … authoring + queries below …
};
```

Segments are **mixed-degree** (not all cubic): a straight line is genuinely `Linear` (the degenerate
curve) and a quadratic stays a quadratic, so each query uses the cheapest exact form for the shape it is
given. `closed` marks a region-boundary loop — it joins the last segment's end back to the first
segment's start and gives `signedDistance` a sign; leave it `false` for an open path.

Four `constexpr` per-segment free functions expose the raw segment math the whole-curve `at` / `tangent`
build on — they fold at compile time, so you can evaluate one segment directly (the `u` parameter is
**local** to the segment, `∈ [0,1]`):

```cpp
Vec2 evalSegment(const CurveSegment& s, float u);            // point at local u (de Casteljau, clamped)
Vec2 evalSegmentDerivative(const CurveSegment& s, float u);  // RAW (non-unit) derivative dB/du; direction of travel
Vec2 segmentStart(const CurveSegment& s);                    // the live first control point (p0)
Vec2 segmentEnd(const CurveSegment& s);                      // the live last control point (by degree)
```

## Authoring — the three front doors

Three ways to author into the one internal Bézier form. A straight line, a single Bézier, points to pass
through, or a start/end with directions — pick whichever you have:

```cpp
static Curve line(Vec2 a, Vec2 b);                         // a straight segment
static Curve quadratic(Vec2 p0, Vec2 ctrl, Vec2 p1);       // one quadratic Bézier
static Curve cubic(Vec2 p0, Vec2 c0, Vec2 c1, Vec2 p1);    // one cubic Bézier

// Catmull-Rom: the curve passes THROUGH every input point (≥ 2 points; fewer → an empty curve).
// `closed` wraps into a loop; open reflects the endpoint tangents.
static Curve throughPoints(std::span<const Vec2> pts, bool closed = false);

// Hermite: a cubic that LEAVES p0 along tangent0 and ARRIVES at p1 along tangent1.
static Curve hermite(Vec2 p0, Vec2 tangent0, Vec2 p1, Vec2 tangent1);
```

- **`throughPoints`** is the one to reach for when you have waypoints and want a smooth curve that
  *interpolates* them (passes exactly through each), not one that is merely pulled toward them. Uniform
  Catmull-Rom; each input point lands on a segment join.
- **`hermite`** takes a start and end plus a **direction (tangent) at each** — a "leave here heading this
  way, arrive there heading that way" curve. The directional vector is the tangent; its length sets how
  hard the curve is pulled along it.

Build a multi-segment path by chaining; each appender starts at the current end (the origin if the curve
is empty):

```cpp
Curve& lineTo(Vec2 to);                       // append a straight segment
Curve& quadraticTo(Vec2 ctrl, Vec2 to);       // append a quadratic
Curve& cubicTo(Vec2 c0, Vec2 c1, Vec2 to);    // append a cubic
```

```cpp
Curve path;
path.lineTo({40, 0}).cubicTo({60, 40}, {100, 40}, {120, 0});   // a line then a cubic, joined

const std::array<Vec2, 4> pts{{{20, 36}, {60, 22}, {104, 52}, {142, 30}}};
const Curve smooth = Curve::throughPoints(std::span<const Vec2>(pts));   // passes through all four
```

Aggregate initialization stays available if you already have segments (`Curve{.segments = {…}, .closed =
true}`); the named constructors are the ergonomic shorthand.

## The queries

```cpp
Vec2  at(float t) const;                // point at global t ∈ [0,1] (uniform PER SEGMENT)
Vec2  tangent(float t) const;           // UNIT direction of travel at t (zero vector if degenerate)
float length() const;                   // total arc length
Vec2  atDistance(float s) const;        // CONSTANT-SPEED: the point at arc-length s ∈ [0, length]
Vec2  tangentAtDistance(float s) const; // UNIT facing at arc-length s (matches atDistance)
float signedDistance(Vec2 p) const;     // min distance to the curve; signed for closed curves
bool  contains(Vec2 p) const;           // closed curves only: signedDistance(p) <= 0
ArcLengthTable arcTable() const;        // bake a reusable arc-length table (see below)
```

**`at(t)` vs `atDistance(s)` — two different meanings, both shipped.** `at(t)` maps `t ∈ [0,1]`
*uniformly per segment*: each segment owns an equal `1/N` slice of `t` regardless of how long it is, so
`at` moves faster along short segments and slower along long ones. `atDistance(s)` is the
**constant-speed** query — `s` is an arc-length, so equal steps in `s` are equal distances along the
curve. For a non-uniform curve the two differ: `atDistance(length()/2)` is the true geometric midpoint,
while `at(0.5)` is the parameter midpoint (often a segment boundary). Use `atDistance` to move something
along a path at an even pace; use `at` when you just want N samples of the shape.

`tangent(t)` returns a **unit** vector pointing along travel at the parameter `t`; a degenerate point (a
zero-length derivative, e.g. a `line` whose endpoints coincide) returns the zero vector rather than a
NaN. To **orient a mover that advances by `atDistance`, use `tangentAtDistance(s)`, not `tangent(s /
length())`** — `s / length()` is a parameter, not an arc-length, so on a non-uniform curve it reads the
heading at a *different point* than the mover actually sits at. `tangentAtDistance(s)` resolves the
arc-length the same way `atDistance(s)` does, so the position and the facing always agree.

`length()`, `atDistance(s)`, and `tangentAtDistance(s)` resample the curve on **each call** (cost linear
in the segment count). When you query one curve repeatedly, bake the table once and reuse it — see below.

## Reusing the arc-length table

When you query **one** curve every frame — a mover walking a path, or many movers on the same path —
build an `ArcLengthTable` once and query that instead of the curve, so the sampling happens a single time:

```cpp
const ArcLengthTable arc = path.arcTable();   // samples the curve ONCE

// every frame:
const Vec2 pos = arc.atDistance(s);
const Vec2 dir = arc.tangentAtDistance(s);
```

```cpp
struct ArcSample {                          // one baked table entry (the element type of `samples`)
    float         distance = 0.0f;          // cumulative arc-length at this sample
    std::uint32_t segment  = 0;             // the curve parameter (segment index +
    float         localU   = 0.0f;          //   local u) sitting at that distance
};

struct ArcLengthTable {
    std::vector<CurveSegment> segments;     // a copy of the source curve's segments
    std::vector<ArcSample>    samples;      // the cumulative arc-length table (one ArcSample per sample)

    float length() const;                   // total arc length
    Vec2  atDistance(float s) const;        // constant-speed point at arc-length s
    Vec2  tangentAtDistance(float s) const; // unit facing at arc-length s
};
```

`ArcLengthTable` is pure data you own (the engine never holds one, exactly like a `Curve`), self-contained
(it copies the curve's segments, so the source curve need not outlive it), and returns results **identical**
to the curve's own on-call queries — it just skips the re-sampling. The curve's own `atDistance` /
`tangentAtDistance` bake a throwaway table internally each call, so they are the one-shot convenience;
`arcTable()` is the reuse path. Build it once the curve is final; rebuild it if the curve's points change.

**`signedDistance(p)`** is the curve's distance field. The magnitude is the minimum distance from `p` to
the curve — exact for `Linear` and `Quadratic` segments, and accurate to a subdivision tolerance for
`Cubic`. For a **`closed`** curve it carries a sign: **negative inside, positive outside** (by winding
number), zero on the boundary. For an **open** curve the sign is meaningless and the unsigned distance is
returned. `contains(p)` is the convenience for closed curves (`signedDistance(p) <= 0`); it is always
`false` for an open curve, since containment is only defined for a boundary loop. An empty curve's
`signedDistance` is infinity.

```cpp
const std::array<Vec2, 4> corners{{{0, 0}, {100, 0}, {100, 100}, {0, 100}}};
const Curve region = Curve::throughPoints(std::span<const Vec2>(corners), /*closed=*/true);
region.contains({50, 50});            // true  — inside the smooth loop
region.signedDistance({50, 50});      // negative
region.signedDistance({250, 50});     // positive — outside
```

## Worked example — walking a path at constant speed

`atDistance` advances something along a curve at an even pace; `tangent` orients it. Hold the arc-length
cursor yourself (the engine owns no curve state), advance it each tick, and read the position each frame:

```cpp
const Curve  path = Curve::throughPoints(std::span<const Vec2>(waypoints));
const float  len  = path.length();
float        s    = 0.0f;            // arc-length cursor — game-owned

loop.simTick([&](const InputState&) {
    s += 0.24f;                       // ~14 px/s at 59.7275 Hz — even spacing, no lurching
    if (len > 0.0f && s > len) s -= len;
});

loop.renderLoop([&](float) {
    const Vec2 pos = path.atDistance(s);          // where the mover is now (constant speed)
    const Vec2 dir = path.tangentAtDistance(s);   // facing — the heading AT that same arc-length point
    // … place a sprite at pos, orient it by dir, submit …
});
```

Pair `atDistance` with a [`Tween`](tween.md) to vary the pace: drive `s` from a `Tween<float>` under an
`Easing` and the mover eases along the same path — the curve is the shape, the tween is the timing.

> **Photosensitivity:** keep motion along a curve slow and monotonic; advance the arc-length cursor in
> small steps so a walker drifts rather than jumps.

A runnable visual — a Catmull-Rom curve through waypoints, a constant-speed walker, tangent ticks, and a
Hermite curve — is in [`examples/curve_demo/`](../../examples/curve_demo/main.cpp).

## Where to change things

- **A smooth curve through known points:** `Curve::throughPoints(pts)` — it passes through every point.
  Add `closed = true` for a region-boundary loop.
- **A curve with a start/end heading:** `Curve::hermite(p0, dir0, p1, dir1)` — the directions are
  tangents; longer tangents pull the curve harder along them.
- **A multi-segment path:** start empty and chain `lineTo` / `quadraticTo` / `cubicTo`.
- **Move something along a path at even speed:** `atDistance(s)` with a game-owned arc-length cursor —
  not `at(t)`, which is uniform per segment, not per distance.
- **Orient a mover along the path:** `tangentAtDistance(s)` (the facing at the same arc-length point as
  `atDistance(s)`) — not `tangent(s / length())`, which reads a different point on a non-uniform curve.
- **Test whether a point is inside a closed shape:** mark the curve `closed` and call `contains(p)` (or
  `signedDistance(p) <= 0`).
- **Vary the pace along a path:** drive the arc-length cursor from a [`Tween<float>`](tween.md) — the
  curve is the shape, the tween is the timing.
- **Query one path every frame (a mover):** bake `path.arcTable()` once and call `atDistance` /
  `tangentAtDistance` on the returned `ArcLengthTable` — it samples the curve once, not per call.
- **Walk a curve over time (position + facing from elapsed ticks):** hold a
  [`PathWalker`](path-walker.md) — it bakes the table, paces travel (constant speed / eased / a
  `Tween<float>` distance profile), and hands you the position and facing each tick, so you don't hand-roll
  the arc-length cursor.
