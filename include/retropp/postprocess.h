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

// The post-process composition layer (ENG-2.C.2.a): after the renderer composites the layer stack
// into the offscreen viewport, a chain of full-viewport passes can transform that finished image
// before it is blitted to the window. This header is the pure, headlessly unit-tested CPU side —
// the chain-build helper and the displacement math the GPU stage mirrors. An empty chain leaves
// the output byte-identical to the pre-chain baseline.
//
// The first (and currently only) engine stage is ROW DISPLACEMENT: the wavy-water / heat-haze /
// per-line-SCX effect, realized as f(row, phase) per-pixel in a shader — the faithful modern
// expression of an effect a GB achieved by rewriting SCX every scanline, with no reconstructed LY
// counter and no HBlank ISR (ENGINE_DECISIONS.md § Issue 14 / scanline-derived effects).
// The game advances `phase` per frame to animate.

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
// duplicate. constexpr mirror of the shader's boundary branch (PLAN Amendment A2).
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
// displace.frag shader expression byte-for-byte:
//   Horizontal: srcU = uv.u + (amplitude/viewportW)·sin(2π·(frequency·uv.v + phase));  srcV = uv.v
//   Vertical:   srcV = uv.v + (amplitude/viewportH)·sin(2π·(frequency·uv.u + phase));  srcU = uv.u
// `amplitude` is in viewport pixels; `frequency` is cycles across the modulated axis; `phase` is the
// game-advanced animation phase. amplitude == 0 (or a None/unknown kind) → identity (srcUv == uv).
//
// Not constexpr: std::sin is not a core-constant expression in C++20. The pure-arithmetic
// normalization is constexpr-tested via displacementOffset(); this full mirror is unit-tested at
// sine-exact arguments (sin(2π·0.25)=1) and amplitude 0, while the curve itself is GPU-verified
// (PLAN Amendment A1).
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
// peer of DisplaceParams (ENG-2.I.a). The renderer copies these into the GPU uniform (a byte-exact
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
// ripple.frag.hlsl byte-for-byte: the sample is displaced ALONG THE RADIUS from the (normalized) centre
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

// Whether a layer carries a per-layer screen-space effect (i.e. needs the per-layer realization at
// all). A None-kind effect — the default — is no effect, so the layer composites on the unchanged
// faithful path. ENG-2.C.2.b.
[[nodiscard]] constexpr bool layerHasScreenSpaceEffect(const DrawLayer& layer) noexcept {
    return layer.effect.kind != ScreenSpaceEffectKind::None;
}

// Whether an effect is the Below (adjustment-layer) scope vs the Layer (isolated) scope — the
// renderer routes the two differently (Below displaces the whole accumulator at this z; Layer
// displaces only this layer's own content). ENG-2.C.2.b.
[[nodiscard]] constexpr bool effectIsBelowScope(const ScreenSpaceEffect& effect) noexcept {
    return effect.scope == ScreenSpaceEffectScope::Below;
}

// ── Custom shader stages (ENG-2.C.3 / Issue 5) ────────────────────────────────────────

// Whether an effect runs a game-registered custom shader (vs a built-in kind). The renderer
// dispatches on this: a Custom effect binds the registered pipeline pair + pushes the game's
// uniform; a built-in (RowDisplacement) binds displace_/displaceBlend_ + the resolved DisplaceParams.
// Same scope/compositing/ping-pong plumbing either way.
[[nodiscard]] constexpr bool effectUsesCustomShader(const ScreenSpaceEffect& effect) noexcept {
    return effect.kind == ScreenSpaceEffectKind::Custom;
}

// A registration uniform size is valid iff it is 0 (a stage with no uniform cbuffer) or a positive
// multiple of 16 (SDL_GPU cbuffer register packing). registerPostProcessStage validates this; the
// per-pass uniform bytes must then match the registered size exactly (customStagePassValid).
[[nodiscard]] constexpr bool uniformSizeIsValid(std::uint32_t bytes) noexcept {
    return bytes % 16u == 0u;
}

// Whether a Custom effect's per-frame pass is renderable: its handle indexes a registered stage AND
// its uniform byte-count equals the size declared for that stage at registration. An out-of-range
// handle or a size mismatch is an invalid pass (the renderer throws under the Throw collision policy,
// else warns + skips the effect). Pure mirror of the renderer's per-pass validation, so it is
// device-free testable.
[[nodiscard]] constexpr bool customStagePassValid(const ScreenSpaceEffect& effect,
                                                  std::size_t registeredStageCount,
                                                  std::uint32_t registeredUniformSize) noexcept {
    return effect.kind == ScreenSpaceEffectKind::Custom &&
           static_cast<std::size_t>(effect.customShader) < registeredStageCount &&
           effect.uniform.size() == registeredUniformSize;
}

// ── Effect region gate (ENG-2.F) ──────────────────────────────────────────────────────

// Signed distance from `p` (viewport px, already in shape-local space after the region transform
// inverse) to the polygon of the first `n` vertices: the standard winding-number sign + min-edge-
// distance formula (negative inside, positive outside). One routine covers every shape because it
// degenerates cleanly: n==1 → distance-to-point (a CIRCLE once compared against radius); n==2 →
// distance-to-segment (a CAPSULE). The GPU region_select.frag mirrors this byte-for-byte (the
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
    return sdPolygon(local, std::span<const Point>(region.points)) - region.radius <= 0.0f;
}

// The region_select stage's resolved parameters — the CPU side of the region cbuffer the renderer
// fills (the displaceParams discipline). The VERTICES are NOT here — they go to a separate fragment
// storage buffer (unbounded count). `inv*` is the region transform's inverse homography
// (placement→shape) applied per-fragment before the SDF; count/radius gate it; invViewport maps the
// fragment UV→pixels. count==0 ⇒ the stage passes the effect through everywhere (no gate). The renderer
// mirrors these into the HLSL cbuffer byte-for-byte.
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

// ── Chain build ───────────────────────────────────────────────────────────────────────

// The ordered frame-level post-process chain: the frame's postEffects with the None pass-throughs
// filtered out, submission order preserved. The per-layer DrawLayer::effect is NOT part of this —
// it is realized separately (its displacement must happen before compositing); this is the
// frame-level (whole-composited-image) chain only. An empty / all-None postEffects → empty chain →
// the blit samples the composited viewport directly → byte-identical to the pre-chain baseline.
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
