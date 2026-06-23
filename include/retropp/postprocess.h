#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "retropp/draw_state.h"  // ScreenSpaceEffect, ScreenSpaceEffectKind, Axis, FrameDrawState
#include "retropp/geometry.h"    // PixelSize

namespace retropp {

// The post-process composition layer: after the renderer composites the layer stack into the
// offscreen viewport, a chain of full-viewport passes can transform that finished image before it is
// blitted to the window. This header is the pure, headlessly unit-tested CPU side — the chain-build
// helper and the displacement / ripple / region math the GPU stages mirror. An empty chain leaves
// the output unchanged.
//
// The built-in stages are ROW DISPLACEMENT (wavy water / heat haze / per-line scroll, f(row, phase))
// and RIPPLE (a radial water droplet, displaced along the radius from a centre) — each realized
// per-pixel in a shader, the modern expression of effects a GB achieved by rewriting SCX every
// scanline, with no reconstructed LY counter and no HBlank ISR. The game advances `phase` per frame
// to animate.

// ── Normalized texture coordinate ─────────────────────────────────────────────────────

// A UV sample coordinate (top-left origin, [0,1]² over the source). Identity is the named fields.
// A displaced UV may fall outside [0,1]: the displacement stage treats an out-of-source UV as the
// BLANK BACKDROP (the newly-exposed edge strip is left blank, NOT a stretched duplicate of the edge
// column — see withinSource() and the boundary note on displaceSourceUv). This mirror computes the
// raw displaced coordinate; the in/out-of-bounds decision is withinSource().
struct Uv {
    float u = 0.0f;
    float v = 0.0f;
    [[nodiscard]] constexpr bool operator==(const Uv&) const noexcept = default;
};

// Whether a (possibly displaced) UV samples real source pixels — i.e. lies within [0,1]². The
// displacement stage samples the source when this is true and returns the blank backdrop when it is
// false: a row pulled inward leaves the exposed edge strip blank rather than a stretched edge
// duplicate. constexpr mirror of the shader's boundary branch.
[[nodiscard]] constexpr bool withinSource(Uv uv) noexcept {
    return uv.u >= 0.0f && uv.u <= 1.0f && uv.v >= 0.0f && uv.v <= 1.0f;
}

// The displacement stage's per-fragment boundary decision (the CPU mirror of the shader branch):
// the fragment resolves to the blank backdrop only under the Blank edge AND a displaced UV outside
// the source. Under Stretch it always samples (the CLAMP_TO_EDGE sampler duplicates the edge column);
// in-bounds it always samples whatever the edge.
[[nodiscard]] constexpr bool resolvesToBackdrop(Uv displacedUv, DisplacementEdge edge) noexcept {
    return edge == DisplacementEdge::Blank && !withinSource(displacedUv);
}

// ── Displacement math (the CPU mirror the displace.frag shader reproduces) ─────────────

// The displacement offset for a given sine value: amplitude (in viewport pixels) normalized to UV
// by the inverse viewport dimension, times the sine. Pure arithmetic, genuinely constexpr (no sin)
// — so the px→UV normalization is static_assert-testable independent of the transcendental. A
// non-positive viewport dimension yields no displacement. displaceSourceUv() routes through this.
[[nodiscard]] constexpr float displacementOffset(float sinValue, float amplitude,
                                                 int viewportDim) noexcept {
    return viewportDim > 0 ? amplitude / static_cast<float>(viewportDim) * sinValue : 0.0f;
}

// For an output fragment at normalized `uv`, the source UV to sample under `effect`. Mirrors the
// displace.frag shader expression exactly:
//   Horizontal: srcU = uv.u + (amplitude/viewportW)·sin(2π·(frequency·uv.v + phase));  srcV = uv.v
//   Vertical:   srcV = uv.v + (amplitude/viewportH)·sin(2π·(frequency·uv.u + phase));  srcU = uv.u
// `amplitude` is in viewport pixels; `frequency` is cycles across the modulated axis; `phase` is the
// game-advanced animation phase. amplitude == 0 (or a None/unknown kind) → identity (srcUv == uv).
//
// Not constexpr: std::sin is not a core-constant expression in C++20. The pure-arithmetic
// normalization is constexpr-tested via displacementOffset(); this full mirror is unit-tested at
// sine-exact arguments (sin(2π·0.25)=1) and amplitude 0, while the curve itself is GPU-verified.
[[nodiscard]] inline Uv displaceSourceUv(Uv uv, const ScreenSpaceEffect& effect,
                                         PixelSize viewport) noexcept {
    if (effect.kind != ScreenSpaceEffectKind::RowDisplacement || effect.amplitude == 0.0f) {
        return uv;
    }
    constexpr float kTwoPi = 6.283185307179586f;
    if (effect.axis == Axis::Horizontal) {
        const float s = std::sin(kTwoPi * (effect.frequency * uv.v + effect.phase));
        return Uv{uv.u + displacementOffset(s, effect.amplitude, viewport.width), uv.v};
    }
    const float s = std::sin(kTwoPi * (effect.frequency * uv.u + effect.phase));
    return Uv{uv.u, uv.v + displacementOffset(s, effect.amplitude, viewport.height)};
}

// ── Stage uniform parameters ──────────────────────────────────────────────────────────

// The displacement stage's parameters, resolved from an effect + the viewport. The renderer copies
// these into the GPU uniform (a byte-exact mirror, DisplaceFragUniforms in renderer.cpp); keeping
// the resolution here makes the inverse-viewport normalization unit-testable without a device. The
// axis is carried as its raw enum value (0 = Horizontal, 1 = Vertical), matching the shader's uint.
struct DisplaceParams {
    float         amplitude        = 0.0f;
    float         frequency        = 0.0f;
    float         phase            = 0.0f;
    std::uint32_t axis             = 0;   // Axis as a uint (Horizontal=0, Vertical=1)
    float         invViewportW     = 0.0f;
    float         invViewportH     = 0.0f;
    std::uint32_t edge             = 0;   // DisplacementEdge as a uint (Blank=0, Stretch=1)
    std::uint32_t blankTransparent = 0;   // 0 = opaque backdrop (frame-level / Below), 1 = transparent (Layer)
    [[nodiscard]] constexpr bool operator==(const DisplaceParams&) const noexcept = default;
};

// `blankTransparent` is the scope-dependent blank colour: false (the default — frame-level postEffects
// and per-layer Below) leaves an exposed Blank-edge strip the opaque backdrop; true (per-layer Layer,
// the isolated scope) leaves it fully transparent so the strip reveals the layers below.
[[nodiscard]] constexpr DisplaceParams displaceParams(const ScreenSpaceEffect& effect,
                                                      PixelSize viewport,
                                                      bool blankTransparent = false) noexcept {
    DisplaceParams p;
    p.amplitude        = effect.amplitude;
    p.frequency        = effect.frequency;
    p.phase            = effect.phase;
    p.axis             = static_cast<std::uint32_t>(effect.axis);
    p.invViewportW     = viewport.width  > 0 ? 1.0f / static_cast<float>(viewport.width)  : 0.0f;
    p.invViewportH     = viewport.height > 0 ? 1.0f / static_cast<float>(viewport.height) : 0.0f;
    p.edge             = static_cast<std::uint32_t>(effect.edge);
    p.blankTransparent = blankTransparent ? 1u : 0u;
    return p;
}

// ── Ripple math (the CPU mirror the ripple.frag shader reproduces) ─────────────────────

// The radial-ripple stage's parameters, resolved from an effect + the viewport — the built-in
// peer of DisplaceParams. The renderer copies these into the GPU uniform (a byte-exact
// mirror, RippleFragUniforms in renderer.cpp), so keeping the resolution here makes the px→UV centre
// normalization unit-testable without a device. `center` arrives in viewport pixels and is normalized
// to UV here; `invViewportW/H` carry the px→UV amplitude scale (and the aspect via invH/invW). Field
// order mirrors ripple.frag's RippleUniforms cbuffer (two 16-byte registers).
struct RippleParams {
    float centerU      = 0.0f;
    float centerV      = 0.0f;
    float amplitude    = 0.0f;
    float frequency    = 0.0f;
    float phase        = 0.0f;
    float invViewportW = 0.0f;
    float invViewportH = 0.0f;
    float decay        = 0.0f;
    [[nodiscard]] constexpr bool operator==(const RippleParams&) const noexcept = default;
};

// Resolve a Ripple effect + viewport into the ripple cbuffer parameters. The centre is normalized
// px→UV by the inverse viewport dimension; a non-positive viewport dimension yields a 0 inverse (and
// thus a 0 centre on that axis) rather than dividing by zero. Pure arithmetic, genuinely constexpr
// (no transcendentals) — so the centre normalization is static_assert-testable independent of the
// sine curve, the displaceParams discipline.
[[nodiscard]] constexpr RippleParams rippleParams(const ScreenSpaceEffect& effect,
                                                  PixelSize viewport) noexcept {
    RippleParams p;
    p.invViewportW = viewport.width  > 0 ? 1.0f / static_cast<float>(viewport.width)  : 0.0f;
    p.invViewportH = viewport.height > 0 ? 1.0f / static_cast<float>(viewport.height) : 0.0f;
    p.centerU      = effect.center.x * p.invViewportW;
    p.centerV      = effect.center.y * p.invViewportH;
    p.amplitude    = effect.amplitude;
    p.frequency    = effect.frequency;
    p.phase        = effect.phase;
    p.decay        = effect.decay;
    return p;
}

// For an output fragment at normalized `uv`, the source UV to sample under a Ripple `effect`. Mirrors
// ripple.frag.hlsl exactly: the sample is displaced ALONG THE RADIUS from the (normalized) centre
//   delta   = uv − center                                  (UV)
//   dist    = length(delta.x · aspect, delta.y),  aspect = invH/invW   (circular in screen space)
//   offset  = amplitude · sin(2π·(frequency·dist − phase)) · exp(−decay·dist)   (viewport px)
//   src     = uv + (delta/dist) · (offset · invViewportW, offset · invViewportH)
// `amplitude == 0` (or a non-Ripple/unknown kind) → identity; the centre fragment (dist ≈ 0) → identity
// (no radial direction). Not constexpr: std::sin/exp/sqrt are not core-constant in C++20. The pure
// centre normalization is constexpr-tested via rippleParams(); this full mirror is unit-tested at
// sine-exact arguments while the curve itself is GPU-verified — the displaceSourceUv discipline.
[[nodiscard]] inline Uv rippleSourceUv(Uv uv, const ScreenSpaceEffect& effect,
                                       PixelSize viewport) noexcept {
    if (effect.kind != ScreenSpaceEffectKind::Ripple || effect.amplitude == 0.0f) {
        return uv;
    }
    const RippleParams p = rippleParams(effect, viewport);
    constexpr float kTwoPi = 6.283185307179586f;
    const float dx     = uv.u - p.centerU;
    const float dy     = uv.v - p.centerV;
    const float aspect = p.invViewportW > 0.0f ? p.invViewportH / p.invViewportW : 1.0f;
    const float cx     = dx * aspect;
    const float dist   = std::sqrt(cx * cx + dy * dy);  // corrected (circular) distance
    if (dist <= 1e-5f) return uv;                        // centre: no radial direction
    const float wave   = std::sin(kTwoPi * (p.frequency * dist - p.phase));
    const float env    = std::exp(-p.decay * dist);
    const float offset = p.amplitude * wave * env;       // viewport pixels
    return Uv{uv.u + (dx / dist) * (offset * p.invViewportW),
              uv.v + (dy / dist) * (offset * p.invViewportH)};
}

// ── Per-layer dispatch (the renderer's composite-loop branch, mirrored) ─────────────────

// Whether a layer carries any per-layer screen-space effect in its chain (i.e. needs the per-layer
// realization at all). An empty effects chain — or one holding only None-kind effects — is no effect,
// so the layer composites on the unchanged faithful path.
[[nodiscard]] inline bool layerHasScreenSpaceEffect(const DrawLayer& layer) noexcept {
    for (const ScreenSpaceEffect& e : layer.effects)
        if (e.kind != ScreenSpaceEffectKind::None) return true;
    return false;
}

// Whether an effect is the Below (adjustment-layer) scope vs the Layer (isolated) scope — the
// renderer routes the two differently (Below displaces the whole accumulator at this z; Layer
// displaces only this layer's own content).
[[nodiscard]] constexpr bool effectIsBelowScope(const ScreenSpaceEffect& effect) noexcept {
    return effect.scope == ScreenSpaceEffectScope::Below;
}

// ── Custom shader stages ──────────────────────────────────────────────────────────────

// Whether an effect runs a game-registered custom shader (vs a built-in kind). The renderer
// dispatches on this: a Custom effect binds the registered pipeline pair + pushes the game's
// uniform; a built-in (RowDisplacement) binds displace_/displaceBlend_ + the resolved DisplaceParams.
// Same scope/compositing/ping-pong plumbing either way.
[[nodiscard]] constexpr bool effectUsesCustomShader(const ScreenSpaceEffect& effect) noexcept {
    return effect.kind == ScreenSpaceEffectKind::Custom;
}

// Whether a Custom effect's per-frame pass is renderable: it is a Custom effect AND its handle indexes a
// registered stage. There is no uniform-size to validate — each custom stage carries its OWN reflected
// cbuffer, filled by its generated packer from the effect's inline fields, so an out-of-range
// handle is the only invalid case (the renderer throws under the Throw collision policy, else warns +
// skips). Pure mirror of the renderer's per-pass validation, device-free testable.
[[nodiscard]] constexpr bool customStagePassValid(const ScreenSpaceEffect& effect,
                                                  std::size_t registeredStageCount) noexcept {
    return effect.kind == ScreenSpaceEffectKind::Custom &&
           static_cast<std::size_t>(effect.customShader) < registeredStageCount;
}

// ── Effect region gate ────────────────────────────────────────────────────────────────

// Signed distance from `p` (viewport px, already in shape-local space after the region transform
// inverse) to the polygon of the first `n` vertices: the standard winding-number sign + min-edge-
// distance formula (negative inside, positive outside). One routine covers every shape because it
// degenerates cleanly: n==1 → distance-to-point (a CIRCLE once compared against radius); n==2 →
// distance-to-segment (a CAPSULE). The GPU region_select.frag mirrors this exactly (the
// displaceSourceUv discipline). n==0 is "no polygon" → +inf (regionContains short-circuits the whole-
// viewport case before calling). Not constexpr: std::sqrt is not core-constant in C++20.
[[nodiscard]] inline float sdPolygon(Point p, std::span<const Point> v) noexcept {
    const std::size_t n = v.size();
    if (n == 0) return std::numeric_limits<float>::infinity();
    if (n == 1) {
        const float dx = p.x - v[0].x;
        const float dy = p.y - v[0].y;
        return std::sqrt(dx * dx + dy * dy);
    }
    // General polygon (n≥3) AND the segment case (n==2). For n==2 the two directed "edges" (0→1 and
    // 1→0) coincide, the winding test is skipped, and the result stays the unsigned segment distance —
    // exactly the capsule's spine distance. For n≥3 the winding sign makes it negative inside.
    float d = (p.x - v[0].x) * (p.x - v[0].x) + (p.y - v[0].y) * (p.y - v[0].y);  // squared dist
    float s = 1.0f;
    for (std::size_t i = 0, j = n - 1; i < n; j = i, ++i) {
        const float ex = v[j].x - v[i].x, ey = v[j].y - v[i].y;  // edge
        const float wx = p.x - v[i].x,    wy = p.y - v[i].y;     // p relative to vertex i
        const float ee = ex * ex + ey * ey;
        const float t  = ee > 0.0f ? std::clamp((wx * ex + wy * ey) / ee, 0.0f, 1.0f) : 0.0f;
        const float bx = wx - ex * t, by = wy - ey * t;          // p → nearest point on the edge
        d = std::min(d, bx * bx + by * by);
        if (n >= 3) {
            const bool c1 = p.y >= v[i].y;
            const bool c2 = p.y <  v[j].y;
            const bool c3 = ex * wy > ey * wx;
            if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
        }
    }
    return s * std::sqrt(d);
}

// Whether a viewport-pixel fragment lies inside an effect's region. An empty region → no region →
// always true (the whole-viewport default, the byte-identical baseline). Otherwise map the fragment
// back into shape space via the region transform inverse (perspective divide included, like the tile
// path), then test sdPolygon - radius ≤ 0. The CPU mirror of the region_select.frag gate.
[[nodiscard]] inline bool regionContains(Point fragPx, const ShapePoints& region) noexcept {
    if (region.points.empty()) return true;
    const Transform inv = region.transform.inverse();
    const Point local{inv.applyX(fragPx.x, fragPx.y), inv.applyY(fragPx.x, fragPx.y)};
    const bool inside = sdPolygon(local, std::span<const Point>(region.points)) - region.radius <= 0.0f;
    return inside != region.invert;  // invert flips inside/outside (region.invert true = the OUTSIDE)
}

// The region_select stage's resolved parameters — the CPU side of the region cbuffer the renderer
// fills (the displaceParams discipline). The vertices themselves are NOT here: the renderer packs
// them into the region-select cbuffer separately (two-per-register, up to 64; a longer polygon is
// truncated with a warning). `inv*` is the region transform's inverse homography (placement→shape)
// applied per-fragment before the SDF; count/radius gate it; invViewport maps the fragment UV→pixels.
// count==0 ⇒ the stage passes the effect through everywhere (no gate). The renderer mirrors these
// into the HLSL cbuffer exactly.
struct RegionParams {
    float         invRow0[3] = {1.0f, 0.0f, 0.0f};
    float         invRow1[3] = {0.0f, 1.0f, 0.0f};
    float         invRow2[3] = {0.0f, 0.0f, 1.0f};
    float         invViewportW = 0.0f;
    float         invViewportH = 0.0f;
    std::uint32_t count  = 0;
    float         radius = 0.0f;
};

[[nodiscard]] inline RegionParams regionParams(const ShapePoints& region, PixelSize viewport) noexcept {
    RegionParams p;
    const Transform inv = region.transform.inverse();
    p.invRow0[0] = inv.m00; p.invRow0[1] = inv.m01; p.invRow0[2] = inv.m02;
    p.invRow1[0] = inv.m10; p.invRow1[1] = inv.m11; p.invRow1[2] = inv.m12;
    p.invRow2[0] = inv.m20; p.invRow2[1] = inv.m21; p.invRow2[2] = inv.m22;
    p.invViewportW = viewport.width  > 0 ? 1.0f / static_cast<float>(viewport.width)  : 0.0f;
    p.invViewportH = viewport.height > 0 ? 1.0f / static_cast<float>(viewport.height) : 0.0f;
    p.count  = static_cast<std::uint32_t>(region.points.size());
    p.radius = region.radius;
    return p;
}

// ── Curved region gate (the analytic linear+quadratic boundary the region_select_curve.frag mirrors) ──
//
// When a region's boundary is a closed Curve (ShapePoints::curve non-empty) made of Linear and
// Quadratic segments, the signed distance has a closed form — exact between control points, no facets,
// no vertex cap. This block is the pure CPU mirror the curve region_select shader reproduces (the
// displaceSourceUv / sdPolygon discipline), and it is verified against Curve::signedDistance (the
// reference): same containment sign, magnitude within tolerance. The distance arithmetic is the
// same closed-form depressed-cubic solve as curve.cpp's quadratic distance; the sign is an analytic
// even-odd ray cast (a +x ray, half-open [0,1) per segment) rather than the reference's dense-sample
// winding — the two agree on inside/outside for well-formed closed boundaries.

namespace detail {

// Distance from p to the segment a→b (clamped projection): the Linear-degree exact distance and the
// shader's pointSegment. Mirrors curve.cpp's pointSegmentDistance.
[[nodiscard]] inline float pointSegmentDist(Vec2 p, Vec2 a, Vec2 b) noexcept {
    const Vec2  ab = v2sub(b, a);
    const Vec2  ap = v2sub(p, a);
    const float ee = v2dot(ab, ab);
    const float t  = ee > 0.0f ? std::clamp(v2dot(ap, ab) / ee, 0.0f, 1.0f) : 0.0f;
    const Vec2  q  = v2add(a, v2mul(ab, t));
    const Vec2  pq = v2sub(p, q);
    return std::sqrt(v2dot(pq, pq));
}

// Exact unsigned distance from pos to the quadratic Bézier A, B(control), C — the closed-form
// depressed-cubic root solve (the standard sdBezier), the same arithmetic as curve.cpp's
// quadraticUnsignedDistance and the region_select_curve.frag mirror. A degenerate control (A−2B+C ≈ 0)
// falls back to the segment A→C.
[[nodiscard]] inline float quadraticDist(Vec2 pos, Vec2 A, Vec2 B, Vec2 C) noexcept {
    const Vec2  a  = v2sub(B, A);
    const Vec2  b  = v2add(v2sub(A, v2mul(B, 2.0f)), C);  // A − 2B + C
    const float bb = v2dot(b, b);
    if (bb < 1e-9f) return pointSegmentDist(pos, A, C);
    const Vec2  c  = v2mul(a, 2.0f);
    const Vec2  d  = v2sub(A, pos);
    const float kk = 1.0f / bb;
    const float kx = kk * v2dot(a, b);
    const float ky = kk * (2.0f * v2dot(a, a) + v2dot(d, b)) / 3.0f;
    const float kz = kk * v2dot(d, a);
    const float p  = ky - kx * kx;
    const float p3 = p * p * p;
    const float q  = kx * (2.0f * kx * kx - 3.0f * ky) + kz;
    const float h  = q * q + 4.0f * p3;
    const auto  dot2 = [](Vec2 v) noexcept { return v.x * v.x + v.y * v.y; };
    float res;
    if (h >= 0.0f) {  // one real root
        const float hs = std::sqrt(h);
        const float x0 = (hs - q) * 0.5f;
        const float x1 = (-hs - q) * 0.5f;
        const float t  = std::clamp(std::cbrt(x0) + std::cbrt(x1) - kx, 0.0f, 1.0f);
        res = dot2(v2add(d, v2mul(v2add(c, v2mul(b, t)), t)));
    } else {  // three real roots — take the nearest
        const float z = std::sqrt(-p);
        const float v = std::acos(q / (p * z * 2.0f)) / 3.0f;
        const float m = std::cos(v);
        const float n = std::sin(v) * 1.7320508075688772f;  // √3
        const float t0 = std::clamp((m + m) * z - kx, 0.0f, 1.0f);
        const float t1 = std::clamp((-n - m) * z - kx, 0.0f, 1.0f);
        const float t2 = std::clamp((n - m) * z - kx, 0.0f, 1.0f);
        const float r0 = dot2(v2add(d, v2mul(v2add(c, v2mul(b, t0)), t0)));
        const float r1 = dot2(v2add(d, v2mul(v2add(c, v2mul(b, t1)), t1)));
        const float r2 = dot2(v2add(d, v2mul(v2add(c, v2mul(b, t2)), t2)));
        res = std::min(r0, std::min(r1, r2));
    }
    return std::sqrt(std::max(res, 0.0f));
}

// Toggle `inside` for each [0,1) root of the segment's y(t) = p.y whose x(t) > p.x — the even-odd
// +x-ray cast, one segment. Half-open [0,1) counts a contiguous loop's shared endpoint exactly once.
inline void accumulateCrossings(const CurveSegment& s, Vec2 p, bool& inside) noexcept {
    const auto toggleRight = [&](float x) noexcept { if (x > p.x) inside = !inside; };
    if (s.degree == CurveDegree::Quadratic) {
        const Vec2  p0 = s.p0, ctrl = s.p1, p2 = s.p2;
        const float A = p0.y - 2.0f * ctrl.y + p2.y;
        const float B = 2.0f * (ctrl.y - p0.y);
        const float C = p0.y - p.y;
        const auto  bx = [&](float t) noexcept {
            const float mt = 1.0f - t;
            return mt * mt * p0.x + 2.0f * mt * t * ctrl.x + t * t * p2.x;
        };
        const auto consider = [&](float t) noexcept { if (t >= 0.0f && t < 1.0f) toggleRight(bx(t)); };
        if (std::abs(A) < 1e-7f) {                 // degenerate to linear in t
            if (std::abs(B) > 1e-12f) consider(-C / B);
            return;
        }
        const float disc = B * B - 4.0f * A * C;
        if (disc < 0.0f) return;                    // no real crossing
        const float sq = std::sqrt(disc);
        consider((-B - sq) / (2.0f * A));
        consider((-B + sq) / (2.0f * A));           // a tangency (double root) toggles twice → cancels
        return;
    }
    // Linear (and a Cubic chord — cubics are sampled to a polygon before this analytic path).
    const Vec2  a = s.p0, b = segmentEnd(s);
    const float dy = b.y - a.y;
    if (std::abs(dy) < 1e-12f) return;              // horizontal edge: no crossing
    const float t = (p.y - a.y) / dy;
    if (t >= 0.0f && t < 1.0f) toggleRight(a.x + t * (b.x - a.x));
}

}  // namespace detail

// Whether every segment of a curve boundary is Linear or Quadratic — the degrees the analytic gate
// evaluates exactly. A boundary carrying a Cubic segment (an explicit cubic or a Catmull-Rom
// throughPoints) is not analytic; the renderer samples it to a faceted polygon. Empty ⇒ vacuously
// analytic. Pure decision helper, device-free testable — the renderer routes on it.
[[nodiscard]] inline bool curveRegionIsAnalytic(std::span<const CurveSegment> segs) noexcept {
    for (const CurveSegment& s : segs) {
        if (s.degree == CurveDegree::Cubic) return false;
    }
    return true;
}

// Signed distance from `local` (viewport px, already in shape-local space after the region transform
// inverse) to the CLOSED curve boundary `segs`: negative inside, positive outside. Linear segments use
// the exact point-to-segment distance; Quadratic segments the closed-form sdBezier; the sign is the
// even-odd +x ray cast. The region_select_curve.frag mirrors this exactly. Empty ⇒ +inf (no boundary).
// Verified against Curve::signedDistance (sign agreement + magnitude tolerance).
[[nodiscard]] inline float sdCurveAnalytic(Point local, std::span<const CurveSegment> segs) noexcept {
    if (segs.empty()) return std::numeric_limits<float>::infinity();
    const Vec2 p{local.x, local.y};
    float      d      = std::numeric_limits<float>::infinity();
    bool       inside = false;
    for (const CurveSegment& s : segs) {
        switch (s.degree) {
            case CurveDegree::Linear:    d = std::min(d, detail::pointSegmentDist(p, s.p0, s.p1)); break;
            case CurveDegree::Quadratic: d = std::min(d, detail::quadraticDist(p, s.p0, s.p1, s.p2)); break;
            case CurveDegree::Cubic:
            default:                     d = std::min(d, detail::pointSegmentDist(p, s.p0, segmentEnd(s)));
                                         break;
        }
        detail::accumulateCrossings(s, p, inside);
    }
    return inside ? -d : d;
}

// Whether a viewport-pixel fragment lies inside an effect's CURVE region. A curve-free region defers to
// the polygon regionContains (byte-identical). Otherwise map the fragment back into shape space via the
// region transform inverse (perspective divide included, like the tile path), then test the curve SDF
// inflated by radius. The CPU mirror of the region_select_curve.frag gate.
[[nodiscard]] inline bool curveRegionContains(Point fragPx, const ShapePoints& region) noexcept {
    if (region.curve.empty()) return regionContains(fragPx, region);
    const Transform inv = region.transform.inverse();
    const Point local{inv.applyX(fragPx.x, fragPx.y), inv.applyY(fragPx.x, fragPx.y)};
    const bool inside = sdCurveAnalytic(local, std::span<const CurveSegment>(region.curve)) - region.radius <= 0.0f;
    return inside != region.invert;  // invert flips inside/outside (region.invert true = the OUTSIDE)
}

// The curve region_select stage's resolved parameters — the CPU side of the curve region cbuffer the
// renderer fills (the regionParams discipline; the per-segment control points are packed separately by
// the renderer, two registers per segment, up to a segment cap with truncate-and-warn). `inv*` is the
// region transform's inverse homography; segmentCount/radius gate it; invViewport maps fragment UV→px.
struct CurveRegionParams {
    float         invRow0[3]   = {1.0f, 0.0f, 0.0f};
    float         invRow1[3]   = {0.0f, 1.0f, 0.0f};
    float         invRow2[3]   = {0.0f, 0.0f, 1.0f};
    float         invViewportW = 0.0f;
    float         invViewportH = 0.0f;
    std::uint32_t segmentCount = 0;
    float         radius       = 0.0f;
};

[[nodiscard]] inline CurveRegionParams curveRegionParams(const ShapePoints& region,
                                                         PixelSize viewport) noexcept {
    CurveRegionParams p;
    const Transform inv = region.transform.inverse();
    p.invRow0[0] = inv.m00; p.invRow0[1] = inv.m01; p.invRow0[2] = inv.m02;
    p.invRow1[0] = inv.m10; p.invRow1[1] = inv.m11; p.invRow1[2] = inv.m12;
    p.invRow2[0] = inv.m20; p.invRow2[1] = inv.m21; p.invRow2[2] = inv.m22;
    p.invViewportW = viewport.width  > 0 ? 1.0f / static_cast<float>(viewport.width)  : 0.0f;
    p.invViewportH = viewport.height > 0 ? 1.0f / static_cast<float>(viewport.height) : 0.0f;
    p.segmentCount = static_cast<std::uint32_t>(region.curve.size());
    p.radius       = region.radius;
    return p;
}

// ── Stencil (region erase) ──────────────────────────────────────────────────────────────
//
// The subtractive sibling of the region gate: where regionContains confines an effect to ADD inside a
// shape, a Stencil effect ERASES the layer's own pixels in/around the shape to reveal what is behind it
// (the layers below, or the backdrop). The boundary reuses the region SDF wholesale — sdPolygon for a
// polygon / circle / capsule, sdCurveAnalytic for an analytic curve — so a stencil boundary curves for
// free. These two helpers turn the SDF's signed distance into the survival factor the (premultiplied)
// source colour is scaled by; the region_stencil.frag / region_stencil_curve.frag shaders mirror them.

// Coverage = how far INSIDE the shape, ramped over `feather` (shape-local px, the same units as radius),
// centered on the boundary. feather == 0 → a hard step (1 inside, 0 outside); feather > 0 → a linear
// ramp: 1 at signedDist ≤ -feather/2, 0.5 at the boundary (signedDist == 0), 0 at signedDist ≥ +feather/2,
// clamped past the ends. Pure arithmetic (no transcendentals) → constexpr, static_assert-testable.
[[nodiscard]] constexpr float stencilCoverage(float signedDist, float feather) noexcept {
    if (feather > 0.0f) {
        const float c = 0.5f - signedDist / feather;
        return c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    }
    return signedDist <= 0.0f ? 1.0f : 0.0f;
}

// The survival factor the (premultiplied) source is scaled by, per mode:
//   EraseInside  → 1 - coverage  (deep inside coverage 1 → factor 0 = erased; outside → 1 = kept)
//   EraseOutside → coverage      (inside → 1 kept; outside coverage 0 → factor 0 = erased)
// The fragment output is source * survival across all four (premultiplied) channels.
[[nodiscard]] constexpr float stencilSurvival(StencilMode mode, float coverage) noexcept {
    return mode == StencilMode::EraseInside ? 1.0f - coverage : coverage;
}

// The stencil stage's resolved parameters — the region SDF params (reused verbatim from regionParams)
// plus the two stencil scalars (mode as a uint, feather). The CPU side of the stencil cbuffer the
// renderer fills (the regionParams discipline). A curve-free region takes this polygon path.
struct StencilParams {
    RegionParams  region;
    std::uint32_t mode    = 0;     // StencilMode as a uint (EraseInside = 0, EraseOutside = 1)
    float         feather = 0.0f;  // shape-local px; 0 = hard edge
};

[[nodiscard]] inline StencilParams stencilParams(const ShapePoints& region, StencilMode mode,
                                                 float feather, PixelSize viewport) noexcept {
    return StencilParams{regionParams(region, viewport), static_cast<std::uint32_t>(mode), feather};
}

// The curve-boundary peer of StencilParams — the curve region params plus the same two stencil scalars.
struct CurveStencilParams {
    CurveRegionParams region;
    std::uint32_t     mode    = 0;     // StencilMode as a uint
    float             feather = 0.0f;  // shape-local px; 0 = hard edge
};

[[nodiscard]] inline CurveStencilParams curveStencilParams(const ShapePoints& region, StencilMode mode,
                                                           float feather, PixelSize viewport) noexcept {
    return CurveStencilParams{curveRegionParams(region, viewport), static_cast<std::uint32_t>(mode),
                              feather};
}

// ── Chain build ───────────────────────────────────────────────────────────────────────

// The ordered frame-level post-process chain: the frame's postEffects with the None pass-throughs
// filtered out, submission order preserved. The per-layer DrawLayer::effect is NOT part of this —
// it is realized separately (its displacement must happen before compositing); this is the
// frame-level (whole-composited-image) chain only. An empty / all-None postEffects → empty chain →
// the blit samples the composited viewport directly (no post-process pass runs).
[[nodiscard]] inline std::vector<ScreenSpaceEffect>
activeFrameEffects(const FrameDrawState& frame) {
    std::vector<ScreenSpaceEffect> active;
    active.reserve(frame.postEffects.size());
    for (const ScreenSpaceEffect& e : frame.postEffects) {
        if (e.kind != ScreenSpaceEffectKind::None) active.push_back(e);
    }
    return active;
}

}  // namespace retropp
