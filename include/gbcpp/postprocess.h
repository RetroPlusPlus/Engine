#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "gbcpp/draw_state.h"  // ScreenSpaceEffect, ScreenSpaceEffectKind, Axis, FrameDrawState
#include "gbcpp/geometry.h"    // PixelSize

namespace gbcpp {

// The post-process composition layer (ENG-2.C.2.a): after the renderer composites the layer stack
// into the offscreen viewport, a chain of full-viewport passes can transform that finished image
// before it is blitted to the window. This header is the pure, headlessly unit-tested CPU side —
// the chain-build helper and the displacement math the GPU stage mirrors. An empty chain leaves
// the output byte-identical to the pre-chain baseline.
//
// The first (and currently only) engine stage is ROW DISPLACEMENT: the wavy-water / heat-haze /
// per-line-SCX effect, realized as f(row, phase) per-pixel in a shader — the faithful modern
// expression of an effect a GB achieved by rewriting SCX every scanline, with no reconstructed LY
// counter and no HBlank ISR (GB_PORT_ENGINE_DECISIONS.md § Issue 14 / scanline-derived effects).
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

}  // namespace gbcpp
