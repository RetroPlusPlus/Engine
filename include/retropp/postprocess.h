#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
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

// ── Per-row effect data table ─────────────────────────────────────────────────────────
//
// An effect can carry a per-row data table — an arbitrary array of Vec4 the game fills each frame,
// one entry per scanline (or per region id; the consumer's shader decides what the index means). The
// renderer uploads every table into one flat data store (a width-1 RGBA32F texture, the tables stacked
// vertically) and the effect's shader reads its rows by integer Load. These helpers are the pure CPU
// mirror of that layout — the store addressing and the per-effect stacking — unit-tested without a device.

// One table's location in the flat row-data store: `rows` rows starting at `storeY`. rows == 0 marks an
// effect with no table. Identity is the named fields.
struct RowTableLoc {
    std::uint32_t storeY = 0;
    std::uint32_t rows   = 0;
    [[nodiscard]] constexpr bool operator==(const RowTableLoc&) const noexcept = default;
};

// The store texel a shader reads for row `i` of a table at `storeY`: column 0, row storeY + i. The store
// holds one Vec4 per row (width 1), so addressing is just the stacked y — the CPU mirror of the shader's
// RowDataTexture.Load(int3(0, storeY + i, 0)). Reuses PaletteTexel as the {x, y} type.
[[nodiscard]] constexpr PaletteTexel rowDataStoreTexel(std::uint32_t row, std::uint32_t storeY) noexcept {
    return PaletteTexel{0u, storeY + row};
}

// Assign each table a non-overlapping vertical region by stacking in order: table k starts where table
// k-1 ended. `rowCounts` is each table's row count in submission order (0 for an effect with no table —
// it gets {currentY, 0} and adds no height). The store's total height is the sum of all counts. The
// renderer's per-frame layout uses this identical rule.
[[nodiscard]] inline std::vector<RowTableLoc>
stackRowTables(std::span<const std::uint32_t> rowCounts) {
    std::vector<RowTableLoc> locs;
    locs.reserve(rowCounts.size());
    std::uint32_t y = 0;
    for (const std::uint32_t n : rowCounts) {
        locs.push_back(RowTableLoc{y, n});
        y += n;
    }
    return locs;
}

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

// ── Viewport-grid snap (the crisp-evaluation CPU mirror) ────────────────────────────────
//
// On the Viewport evaluation grid the analytic paths evaluate their spatial math at a viewport-cell
// point so the result matches the viewport-resolution rasterization (crisp under upscale); these mirror
// the shaders' snap exactly, on BOTH grid settings (unsnapped = the Output grid = today's behaviour).
// Not constexpr: std::floor is not core-constant until C++23.

// The centre of the viewport cell a normalized coordinate falls in: (floor(uv·dim) + 0.5) / dim per axis
// — the point displace.frag / ripple.frag evaluate at on the Viewport grid. A non-positive dimension
// leaves that axis unchanged.
[[nodiscard]] inline Uv snapUvToCellCenter(Uv uv, PixelSize viewport) noexcept {
    Uv c = uv;
    if (viewport.width  > 0) c.u = (std::floor(uv.u * static_cast<float>(viewport.width))  + 0.5f) /
                                   static_cast<float>(viewport.width);
    if (viewport.height > 0) c.v = (std::floor(uv.v * static_cast<float>(viewport.height)) + 0.5f) /
                                   static_cast<float>(viewport.height);
    return c;
}

// A viewport-pixel fragment moved to its cell centre: floor + 0.5 per axis — the region shaders' fragPx
// snap on the Viewport grid, before the SDF / inverse transform.
[[nodiscard]] inline Point snapFragToCellCenter(Point fragPx) noexcept {
    return Point{std::floor(fragPx.x) + 0.5f, std::floor(fragPx.y) + 0.5f};
}

// Round-half-up quantization of a pixel displacement: floor(v + 0.5). The displacement quantization the
// sampling effects apply on the Viewport grid — HLSL round() is round-to-even and breaks scale-1 parity
// at .5 ties, so this exact form is the mirror.
[[nodiscard]] inline float roundHalfUpPx(float v) noexcept { return std::floor(v + 0.5f); }

// The source UV a custom shader's sampleSource(requested) actually samples — the CPU mirror of the
// preamble's snap math (retropp_effect.hlsli). A custom shader's generated entry point evaluates the
// shader at `evalUv` (the viewport-cell centre of the fragment's true uv on the Viewport grid; the true
// uv itself on the Output grid); with `snap` set, the displacement the shader asked for relative to that
// evaluation point is quantized to whole viewport pixels (round-half-up per axis) and applied to the
// fragment's TRUE uv, so each source cell's output-resolution interior is copied intact. With `snap`
// false — or a non-positive viewport dimension — the requested coordinate passes through unchanged
// (the Output grid's smooth evaluation). The effect's edge policy applies downstream, on the returned uv.
[[nodiscard]] inline Uv customSampleSourceUv(Uv requested, Uv trueUv, Uv evalUv,
                                             PixelSize viewport, bool snap) noexcept {
    if (!snap || viewport.width <= 0 || viewport.height <= 0) return requested;
    const float w = static_cast<float>(viewport.width);
    const float h = static_cast<float>(viewport.height);
    return Uv{trueUv.u + roundHalfUpPx((requested.u - evalUv.u) * w) / w,
              trueUv.v + roundHalfUpPx((requested.v - evalUv.v) * h) / h};
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
// `snap` selects the evaluation grid: false (the default — the Output grid, today's behaviour) evaluates the
// wave at `uv` and offsets by the continuous px→UV displacement; true (the Viewport grid) evaluates the wave
// at the fragment's viewport-cell centre and offsets by the round-half-up whole-pixel displacement (the crisp
// path, byte-identical to the viewport-resolution rasterization under nearest sampling). Mirrors displace.frag
// exactly on both settings.
[[nodiscard]] inline Uv displaceSourceUv(Uv uv, const ScreenSpaceEffect& effect,
                                         PixelSize viewport, bool snap = false) noexcept {
    if (effect.kind != ScreenSpaceEffectKind::RowDisplacement || effect.amplitude == 0.0f) {
        return uv;
    }
    constexpr float kTwoPi = 6.283185307179586f;
    const Uv e = snap ? snapUvToCellCenter(uv, viewport) : uv;  // wave evaluated at the cell centre when snapping
    if (effect.axis == Axis::Horizontal) {
        const float s = std::sin(kTwoPi * (effect.frequency * e.v + effect.phase));
        const float off = snap
            ? (viewport.width > 0 ? roundHalfUpPx(effect.amplitude * s) / static_cast<float>(viewport.width) : 0.0f)
            : displacementOffset(s, effect.amplitude, viewport.width);
        return Uv{uv.u + off, uv.v};
    }
    const float s = std::sin(kTwoPi * (effect.frequency * e.u + effect.phase));
    const float off = snap
        ? (viewport.height > 0 ? roundHalfUpPx(effect.amplitude * s) / static_cast<float>(viewport.height) : 0.0f)
        : displacementOffset(s, effect.amplitude, viewport.height);
    return Uv{uv.u, uv.v + off};
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
// `snap` selects the evaluation grid: false (the default — the Output grid, today's behaviour) evaluates the
// ripple at `uv` and displaces by the continuous per-axis px→UV offset; true (the Viewport grid) evaluates at
// the fragment's viewport-cell centre and displaces by the round-half-up whole-pixel per-axis offset (the
// crisp path, byte-identical to the viewport-resolution rasterization under nearest sampling). Mirrors
// ripple.frag exactly on both settings.
[[nodiscard]] inline Uv rippleSourceUv(Uv uv, const ScreenSpaceEffect& effect,
                                       PixelSize viewport, bool snap = false) noexcept {
    if (effect.kind != ScreenSpaceEffectKind::Ripple || effect.amplitude == 0.0f) {
        return uv;
    }
    const RippleParams p = rippleParams(effect, viewport);
    constexpr float kTwoPi = 6.283185307179586f;
    const Uv    e      = snap ? snapUvToCellCenter(uv, viewport) : uv;  // ripple evaluated at the cell centre
    const float dx     = e.u - p.centerU;
    const float dy     = e.v - p.centerV;
    const float aspect = p.invViewportW > 0.0f ? p.invViewportH / p.invViewportW : 1.0f;
    const float cx     = dx * aspect;
    const float dist   = std::sqrt(cx * cx + dy * dy);  // corrected (circular) distance
    if (dist <= 1e-5f) return uv;                        // centre: no radial direction
    const float wave   = std::sin(kTwoPi * (p.frequency * dist - p.phase));
    const float env    = std::exp(-p.decay * dist);
    const float offset = p.amplitude * wave * env;       // viewport pixels
    const float dirx   = dx / dist;
    const float diry   = dy / dist;
    if (snap) {
        return Uv{uv.u + roundHalfUpPx(dirx * offset) * p.invViewportW,
                  uv.v + roundHalfUpPx(diry * offset) * p.invViewportH};
    }
    return Uv{uv.u + dirx * (offset * p.invViewportW),
              uv.v + diry * (offset * p.invViewportH)};
}

// ── Colour-fill math (the CPU mirror the colorfill.frag shader reproduces) ─────────────

// The colour-fill stage's resolved parameters — the built-in peer of DisplaceParams / RippleParams: the
// fill colour as normalized [0,1] floats (the shader and this mirror work in floats; the public ColorFill
// carries an Rgba8). The renderer copies these straight into the GPU uniform (ColorFillFragUniforms in
// renderer.cpp). The transform replaces the pixel's rgb with the fill colour; the layer alpha sets opacity.
struct ColorFillParams {
    float r = 0.0f, g = 0.0f, b = 0.0f;  // fill colour, normalized
    [[nodiscard]] constexpr bool operator==(const ColorFillParams&) const noexcept = default;
};

// Resolve a ColorFill effect into the colour-fill parameters — normalize the Rgba8 fill's rgb (0..255 →
// 0..1) and scale by fillIntensity, so a fillIntensity > 1 pushes a channel past 1 (the headroom a Multiply
// container brightens with, carried by the float16 intermediates). Genuinely constexpr (pure arithmetic) →
// static_assert-testable. The renderer fills this on the ColorFill branch.
[[nodiscard]] constexpr ColorFillParams colorFillParams(const ScreenSpaceEffect& e) noexcept {
    constexpr float inv = 1.0f / 255.0f;
    return ColorFillParams{static_cast<float>(e.fill.r) * inv * e.fillIntensity,
                           static_cast<float>(e.fill.g) * inv * e.fillIntensity,
                           static_cast<float>(e.fill.b) * inv * e.fillIntensity};
}

// An RGB colour in [0,1] floats — the colorfill mirror's in/out type (rgb; the stage keeps the pixel's
// alpha). Named channels per the no-positional-opacity discipline.
struct ColorFillRgb {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    [[nodiscard]] constexpr bool operator==(const ColorFillRgb&) const noexcept = default;
};

// The output rgb is the fill colour — a solid replace; the layer alpha sets opacity. The exact
// colorfill.frag rgb math; pure arithmetic → genuinely constexpr, so it is static_assert-testable.
[[nodiscard]] constexpr ColorFillRgb applyColorFill(ColorFillRgb /*in*/, const ColorFillParams& p) noexcept {
    return ColorFillRgb{p.r, p.g, p.b};
}

// ── Gleam math (the CPU mirror the gleam.frag shader reproduces) ───────────────────────

// The gleam stage's resolved parameters — a straight copy of the four Gleam fields (sweep/width/slant are
// UV-space, gain unitless; no normalization). The renderer copies these into the GPU uniform
// (GleamFragUniforms in renderer.cpp).
struct GleamParams {
    float sweep = 0.0f, width = 0.1f, gain = 0.0f, slant = 0.35f;
    [[nodiscard]] constexpr bool operator==(const GleamParams&) const noexcept = default;
};

// Resolve a Gleam effect into the gleam parameters — a straight field copy. Genuinely constexpr (pure
// arithmetic) → static_assert-testable. The renderer fills this on the Gleam branch.
[[nodiscard]] constexpr GleamParams gleamParams(const ScreenSpaceEffect& e) noexcept {
    return GleamParams{e.sweep, e.width, e.gain, e.slant};
}

// The gleam colour transform (in/out rgb in [0,1] floats — reuses ColorFillRgb). For an output fragment at
// normalized `uv`, the sheen boost is a luminance-keyed diagonal band: d = u + v·slant, a soft crest of
// half-width `width` centred on `sweep`, with the WHOLE contribution scaled by `gain` — so gain == 0
// returns `in` unchanged (the identity contract). Mirrors gleam.frag exactly (same op order, luma weights,
// and 0.6 lift); pure arithmetic → constexpr, so static_assert-testable, the applyColorFill discipline.
[[nodiscard]] constexpr ColorFillRgb applyGleam(ColorFillRgb in, float u, float v,
                                                const GleamParams& p) noexcept {
    const float d    = u + v * p.slant;
    float       ad   = d - p.sweep; ad = ad < 0.0f ? -ad : ad;        // abs
    float       band = 1.0f - ad / p.width;
    band = band < 0.0f ? 0.0f : (band > 1.0f ? 1.0f : band);          // saturate
    const float crest = band * band;
    const float lum   = in.r * 0.299f + in.g * 0.587f + in.b * 0.114f;
    const float g     = p.gain * crest;
    const float lift  = lum * g * 0.6f;
    return ColorFillRgb{in.r * (1.0f + g) + lift,
                        in.g * (1.0f + g) + lift,
                        in.b * (1.0f + g) + lift};
}

// ── ColorSaturation math (the CPU mirror the saturation.frag shader reproduces) ────────

// The saturation stage's resolved parameter — the developer's uint8 saturation normalized to [0,1] (255 →
// 1.0 = full saturation = identity, 0 → greyscale). The renderer copies this into the GPU uniform
// (SaturationFragUniforms in renderer.cpp).
struct SaturationParams {
    float saturation = 1.0f;  // normalized 0..1
    [[nodiscard]] constexpr bool operator==(const SaturationParams&) const noexcept = default;
};

// Resolve a ColorSaturation effect — normalize the uint8 saturation (0..255 → 0..1), the same uint8-to-float
// surface as an Rgba8 channel. Genuinely constexpr (pure arithmetic) → static_assert-testable. The renderer
// fills this on the ColorSaturation branch.
[[nodiscard]] constexpr SaturationParams saturationParams(const ScreenSpaceEffect& e) noexcept {
    return SaturationParams{static_cast<float>(e.saturation) / 255.0f};
}

// The saturation colour transform (in/out rgb in [0,1] floats — reuses ColorFillRgb). Each channel is pulled
// toward the pixel's own luminance by `1 - saturation`, so saturation == 1 returns `in` unchanged (the
// identity contract, byte-exact: at amount == 0 the multiply-by-zero + subtract-zero is exact, mirroring
// Gleam's ×(1+0)+0). saturation == 0 collapses every channel to the luma (greyscale). Mirrors saturation.frag
// exactly (same op order, same Rec. 601 luma weights Gleam uses); pure arithmetic → constexpr, so
// static_assert-testable, the applyColorFill / applyGleam discipline.
[[nodiscard]] constexpr ColorFillRgb applySaturation(ColorFillRgb in, const SaturationParams& p) noexcept {
    const float lum    = in.r * 0.299f + in.g * 0.587f + in.b * 0.114f;
    const float amount = 1.0f - p.saturation;
    return ColorFillRgb{in.r - (in.r - lum) * amount,
                        in.g - (in.g - lum) * amount,
                        in.b - (in.b - lum) * amount};
}

// ── Container blend math (the CPU mirror the blend compositor reproduces) ──────────────
//
// How a compositing container (a Region's effects, a DrawLayer's content, the frame's whole-frame
// postEffects) combines its colour SOURCE `src` over what it composites onto, `dst`, under a BlendMode.
// The separable blend operator B(dst, src) is applied per channel, source-alpha-weighted:
//   out.rgb = (1 - src.a)·dst.rgb + src.a·B(dst.rgb, src.rgb)   (clamped to [0,1])
//   out.a   = src.a + dst.a·(1 - src.a)                         (standard over alpha; mode-independent)
// Normal (B = src) reduces to the standard alpha-over, so a Normal container's output is the alpha-over
// it always was. The region-select gate and the blend composite shader reproduce this exactly — it is
// the single authority both mirror. Colours are straight (non-premultiplied) [0,1] floats. Genuinely
// constexpr (pure arithmetic) → static_assert-testable, the applyColorFill / applyBlendMode discipline.

// The separable blend operator B(d, s) for one channel, per mode (rgb only; alpha composites separately).
// The result is clamped by applyBlendMode, not here.
[[nodiscard]] constexpr float blendChannel(BlendMode mode, float d, float s) noexcept {
    switch (mode) {
        case BlendMode::Add:      return d + s;
        case BlendMode::Subtract: return d - s;
        case BlendMode::Multiply: return d * s;
        case BlendMode::Screen:   return 1.0f - (1.0f - d) * (1.0f - s);
        case BlendMode::Half:     return (d + s) * 0.5f;
        case BlendMode::Normal:
        default:                  return s;
    }
}

// Combine source `src` over `dst` under `mode`: the separable blend, source-alpha-weighted, with the
// standard over alpha. Normal reduces to plain alpha-over. The exact math the gate + blend shaders mirror.
[[nodiscard]] constexpr Vec4 applyBlendMode(Vec4 dst, Vec4 src, BlendMode mode) noexcept {
    const float sa = src.w;
    return Vec4{
        std::clamp((1.0f - sa) * dst.x + sa * blendChannel(mode, dst.x, src.x), 0.0f, 1.0f),
        std::clamp((1.0f - sa) * dst.y + sa * blendChannel(mode, dst.y, src.y), 0.0f, 1.0f),
        std::clamp((1.0f - sa) * dst.z + sa * blendChannel(mode, dst.z, src.z), 0.0f, 1.0f),
        std::clamp(sa + dst.w * (1.0f - sa), 0.0f, 1.0f)};
}

// The standard alpha-over, the reference Normal reduces to: applyBlendMode(dst, src, Normal) == alphaOver.
[[nodiscard]] constexpr Vec4 alphaOver(Vec4 dst, Vec4 src) noexcept {
    const float sa = src.w;
    return Vec4{std::clamp((1.0f - sa) * dst.x + sa * src.x, 0.0f, 1.0f),
                std::clamp((1.0f - sa) * dst.y + sa * src.y, 0.0f, 1.0f),
                std::clamp((1.0f - sa) * dst.z + sa * src.z, 0.0f, 1.0f),
                std::clamp(sa + dst.w * (1.0f - sa), 0.0f, 1.0f)};
}

// Normal reduces to the standard alpha-over (the byte-identity anchor: a Normal container is unchanged).
static_assert(applyBlendMode(Vec4{0.2f, 0.4f, 0.6f, 0.7f}, Vec4{0.8f, 0.1f, 0.3f, 0.5f}, BlendMode::Normal)
                  == alphaOver(Vec4{0.2f, 0.4f, 0.6f, 0.7f}, Vec4{0.8f, 0.1f, 0.3f, 0.5f}),
              "BlendMode::Normal must reduce to standard alpha-over");
// Each mode's operator against an opaque source over an opaque backdrop (src.a = 1 ⇒ out.rgb = B(dst, src)),
// at binary-exact values so the anchor is value-precise.
static_assert(applyBlendMode(Vec4{0.5f, 0, 0, 1}, Vec4{0.25f, 0, 0, 1}, BlendMode::Add).x == 0.75f,
              "Add: dst + src");
static_assert(applyBlendMode(Vec4{0.5f, 0, 0, 1}, Vec4{0.25f, 0, 0, 1}, BlendMode::Subtract).x == 0.25f,
              "Subtract: dst - src");
static_assert(applyBlendMode(Vec4{0.5f, 0, 0, 1}, Vec4{0.5f, 0, 0, 1}, BlendMode::Multiply).x == 0.25f,
              "Multiply: dst * src");
static_assert(applyBlendMode(Vec4{0.5f, 0, 0, 1}, Vec4{0.5f, 0, 0, 1}, BlendMode::Screen).x == 0.75f,
              "Screen: 1 - (1 - dst)(1 - src)");
static_assert(applyBlendMode(Vec4{0.5f, 0, 0, 1}, Vec4{0.25f, 0, 0, 1}, BlendMode::Half).x == 0.375f,
              "Half: (dst + src) / 2");
// Add clamps at the top of the range (0.8 + 0.5 = 1.3 → 1).
static_assert(applyBlendMode(Vec4{0.8f, 0, 0, 1}, Vec4{0.5f, 0, 0, 1}, BlendMode::Add).x == 1.0f,
              "Add clamps to 1");

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

// Transform a shape's boundary signed distance (sdShape - radius; negative inside the fill) into the
// signed distance to a STROKE BAND of width `strokeWidth` centered on that boundary: |d| - strokeWidth/2,
// negative only within ±strokeWidth/2 of the boundary. strokeWidth == 0 returns `d` unchanged (the filled
// region — the byte-identical default). The four region/stencil fragment shaders apply the identical
// transform to their signed distance immediately before the invert step, so a stroked region confines its
// effects (the gate) and its see-through (Transparency) to the shape's OUTLINE. Sign-independent (the
// abs), so an open curve boundary strokes into an open band. Genuinely constexpr (no std::abs, which is
// not core-constant until C++23) → static_assert-testable.
[[nodiscard]] constexpr float bandSignedDistance(float d, float strokeWidth) noexcept {
    if (strokeWidth <= 0.0f) return d;
    const float ad = d < 0.0f ? -d : d;
    return ad - strokeWidth * 0.5f;
}

// Whether a viewport-pixel fragment lies inside an effect's region. An empty region → no region →
// always true (the whole-viewport default, the byte-identical baseline). Otherwise map the fragment
// back into shape space via the region transform inverse (perspective divide included, like the tile
// path), then test (sdPolygon - radius), routed through bandSignedDistance (a stroke confines to the
// boundary band), ≤ 0. The CPU mirror of the region_select.frag gate.
// `snap` selects the evaluation grid: false (the default — the Output grid) tests the fragment as given;
// true (the Viewport grid) snaps it to its viewport-cell centre first, so the gate resolves per viewport
// pixel (the crisp path). Mirrors region_select.frag on both settings.
[[nodiscard]] inline bool regionContains(Point fragPx, const ShapePoints& region, bool snap = false) noexcept {
    if (region.points.empty()) return true;
    if (snap) fragPx = snapFragToCellCenter(fragPx);
    const Transform inv = region.transform.inverse();
    const Point local{inv.applyX(fragPx.x, fragPx.y), inv.applyY(fragPx.x, fragPx.y)};
    const float d = bandSignedDistance(
        sdPolygon(local, std::span<const Point>(region.points)) - region.radius, region.strokeWidth);
    return (d <= 0.0f) != region.invert;  // invert flips inside/outside (region.invert true = the OUTSIDE)
}

// The COMPOSE-GRID bounding box a region-confined effect's costly runEffect pass can be scissored to,
// so the effect shader shades only the pixels the region gate might keep — not the whole frame. The gate
// (region_select) discards everything outside the shape, so the effect result matters only inside the
// shape's (radius + stroke)-inflated box; computing it frame-wide then masking is pure fill-rate waste.
// Output is byte-identical — the gate is untouched, and the scissored pixels are exactly the ones it reads.
//
// Coordinate space: the intermediate targets are sized viewport × composeScale, but shapes are in viewport
// px, so the box is scaled by composeScale and clamped to [0,composeW] × [0,composeH]. Shapes that cannot be
// bounded tight return the full compose rect {0,0,composeW,composeH} — the "no scissor" sentinel: `invert`
// (the region is the OUTSIDE → spans the frame), a non-identity `transform`, a curved boundary
// (`curve` non-empty — a baked `curveMask` is only consulted when `curve` carries a cubic, so this covers it),
// or empty `points`. A circle (points=[c], radius=r) and a capsule (points=[a,b], radius=r) bound exactly.
//
// The gate has NO anti-aliasing (region_select.frag resolves containment as a hard step), and under CRISP
// snapping tests the fragment's viewport-cell centre (within its own pixel). So the box need only cover every
// compose pixel whose SDF test point lands inside the shape: floor/ceil the viewport-space bbox to whole
// viewport px, then ±1 px outward for float32 SDF slop, then scale to the compose grid. A fully-offscreen
// shape collapses to a 1×1 degenerate-but-valid rect (backends need not accept a zero-area scissor; nothing
// inside the shape is visible, so the gate never reads the effect result there).
[[nodiscard]] inline IntRect regionScissorRect(const ShapePoints& shape, int composeScale,
                                               int composeW, int composeH) noexcept {
    const IntRect full{0, 0, composeW, composeH};
    if (shape.invert || shape.points.empty() || !shape.curve.empty() || shape.transform != Transform{})
        return full;

    float minX = std::numeric_limits<float>::infinity(), minY = minX;
    float maxX = -std::numeric_limits<float>::infinity(), maxY = maxX;
    for (const Point& p : shape.points) {
        minX = std::min(minX, p.x);  minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);  maxY = std::max(maxY, p.y);
    }
    const float inflate = shape.radius + shape.strokeWidth * 0.5f;  // SDF fill reach + stroke half-width
    minX -= inflate;  minY -= inflate;  maxX += inflate;  maxY += inflate;

    // floor/ceil to whole viewport px, ±1 px outward (float32 slop), then scale to the compose grid.
    int x0 = (static_cast<int>(std::floor(minX)) - 1) * composeScale;
    int y0 = (static_cast<int>(std::floor(minY)) - 1) * composeScale;
    int x1 = (static_cast<int>(std::ceil(maxX)) + 1) * composeScale;
    int y1 = (static_cast<int>(std::ceil(maxY)) + 1) * composeScale;

    x0 = std::clamp(x0, 0, composeW);  y0 = std::clamp(y0, 0, composeH);
    x1 = std::clamp(x1, 0, composeW);  y1 = std::clamp(y1, 0, composeH);

    if (x1 <= x0 || y1 <= y0) {  // fully offscreen / clamped to nothing → a 1×1 valid scissor at the corner
        const int cx = std::clamp(x0, 0, std::max(0, composeW - 1));
        const int cy = std::clamp(y0, 0, std::max(0, composeH - 1));
        return IntRect{cx, cy, 1, 1};
    }
    return IntRect{x0, y0, x1 - x0, y1 - y0};
}

// ── Region batching (the instanced-additive fast path) ─────────────────────────────────────────
//
// Many same-shader region-confined effects on one site are the O(N)-pass GPU cliff: each is modelled as
// its own pair of full-frame passes (runEffect then the region-select gate), so N regions ⇒ ~2N
// serialized passes chaining on a read-after-write hazard the GPU cannot pipeline. When the effect is a
// custom shader whose output is its source PLUS a source-independent term (out = sampleSource(uv) + D(uv);
// the shader opts in with a `// @retropp:additive` declaration), those N regions collapse into ONE
// instanced additive render pass: each region is a covering quad, the region gate is replicated in the
// quad's fragment, and hardware additive blending accumulates the deltas — no gate pass, pass count
// independent of N. These pure helpers are the CPU side the renderer drives: the eligibility predicate,
// the per-region instance record, and the run-grouping that keeps composition on the fast path. The
// batched additive output equals the per-region path within float-rounding order (Tol::OneStep), so a
// region whose eligibility flips frame-to-frame changes route without visibly changing output.

// Whether a confined step CAN take the batched additive path — everything the fast path requires EXCEPT
// that the stage actually has a batched (additive-declared) pipeline, which is a runtime-registry fact
// the renderer ANDs in. A step qualifies iff it is a Custom effect (built-ins are not additive-declared),
// its owning Region is Normal-blend at full alpha (a non-Normal blend / alpha < 1 reads the destination,
// which one hardware-blended pass can't express for all N), it carries no per-row paramTable, and its
// shape is a plain circle (1 point) or capsule (2 points) with no invert / stroke / curve / transform (the
// shapes whose gate the batched fragment replicates exactly, and whose bounding box the instance record
// covers tight). A whole-reach step (no shape → 0 points) and a whole-viewport shapeless region
// (synthesized 4-point rectangle) are naturally excluded. Pure mirror of the renderer's routing predicate.
[[nodiscard]] inline bool regionBatchEligible(const ScreenSpaceEffect& eff, const ShapePoints& shape,
                                             float regionAlpha, BlendMode regionBlend) noexcept {
    if (eff.kind != ScreenSpaceEffectKind::Custom) return false;
    if (regionBlend != BlendMode::Normal || regionAlpha != 1.0f) return false;
    if (!eff.paramTable.empty()) return false;
    if (shape.invert || shape.strokeWidth != 0.0f) return false;
    if (!shape.curve.empty()) return false;
    if (shape.transform != Transform{}) return false;
    const std::size_t n = shape.points.size();
    return n == 1 || n == 2;
}

// One batched region's instance record: the covering quad (COMPOSE-grid px, regionScissorRect verbatim —
// the single authority for "the box that covers a shape"), the shape spine (viewport px; a circle's p1 ==
// p0), and the SDF radius (viewport px) the batched fragment gates against. The renderer packs this into
// the instanced draw's storage buffer; the batched vertex stage rasterizes the box and the fragment keeps
// only pixels within `radius` of the spine (the n≤2 point-segment gate). Identity is the named fields.
struct RegionBatchInstance {
    IntRect box;              // covering quad, compose-grid px (regionScissorRect(shape, …))
    Point   p0;               // shape spine start, viewport px
    Point   p1;               // shape spine end,   viewport px (circle: p1 == p0)
    float   radius = 0.0f;    // SDF inflation, viewport px
    [[nodiscard]] bool operator==(const RegionBatchInstance&) const noexcept = default;
};

// Build the instance record for an eligible shape (circle / capsule). The box is regionScissorRect (same
// floor/ceil ±1 px slop + clamping the scissored per-region path used, so a batched region covers exactly
// the pixels the gate can keep); p0/p1 are the spine (a circle repeats p0; a capsule takes its two
// vertices); radius is the shape's SDF inflation. Assumes eligibility (1 or 2 points) — a degenerate empty
// shape yields a zero spine, harmless (the box collapses to the offscreen 1×1 rect).
[[nodiscard]] inline RegionBatchInstance regionBatchInstance(const ShapePoints& shape, int composeScale,
                                                            int composeW, int composeH) noexcept {
    RegionBatchInstance r;
    r.box = regionScissorRect(shape, composeScale, composeW, composeH);
    if (!shape.points.empty()) {
        r.p0 = shape.points.front();
        r.p1 = shape.points.size() >= 2 ? shape.points[1] : shape.points.front();
    }
    r.radius = shape.radius;
    return r;
}

// ── Run grouping ───────────────────────────────────────────────────────────────────────────────
//
// Given a site's ordered confined steps, decide which coalesce into batched additive passes. The rule:
// within a maximal CONTIGUOUS stretch of eligible steps (an ineligible step is a hard boundary — it may
// not commute, so nothing merges across it, preserving composition order), additive deltas commute, so
// the eligible steps are grouped by (stage, packed cbuffer bytes) and every group of ≥ 2 becomes one run
// — a single instanced additive pass. Groups of 1 (and every ineligible step) stay on the existing
// per-region / whole-reach path at equal cost, in their exact list position. Grouping (not mere
// same-shader contiguity) is what keeps COMPOSITION on the fast path: N regions each carrying two
// different additive effects interleave as A₁ B₁ A₂ B₂ … and still yield two full runs. Pure and
// device-free: the renderer feeds per-step keys and consumes the returned dispositions.

// One step's batch key: whether it is eligible, its stage handle, and its packed uniform bytes (grouped
// by exact byte equality — memcmp). The renderer builds these from the confined steps (eligibility ANDed
// with the batched-pipeline existence; params from the stage's generated packer).
struct BatchStep {
    bool                       eligible = false;
    std::uint32_t              stage    = 0;   // PostProcessStageId (customShader handle) as a uint
    std::span<const std::byte> params;         // packed cbuffer bytes; empty for a parameterless shader
};

// One batched run: the stage + the ascending indices (into the site's step list) of the ≥ 2 eligible
// steps that share a (stage, params) key within one contiguous eligible stretch. The renderer issues one
// additive pass per run, drawing one instance per step.
struct RegionBatchRun {
    std::uint32_t              stage = 0;
    std::vector<std::uint32_t> steps;   // indices into the input; size ≥ 2, ascending
};

// The grouping result: the runs, plus a per-step disposition — stepRun[i] is the run index step i belongs
// to, or -1 if step i is processed individually (an ineligible step, or an eligible singleton). The
// renderer walks its step list in order: at step i, if stepRun[i] < 0 it takes the existing per-step path;
// otherwise it issues the batched pass once (at the run's first step) and skips the run's other steps.
struct RegionBatchGrouping {
    std::vector<RegionBatchRun> runs;
    std::vector<int>            stepRun;
};

// Whether two packed-uniform byte spans are byte-identical (the memcmp grouping key). Same length + equal
// bytes; two empty spans (parameterless shaders) are equal.
[[nodiscard]] inline bool sameBatchParams(std::span<const std::byte> a,
                                          std::span<const std::byte> b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// Group a site's steps into batched runs (see the section note). Maximal contiguous eligible stretches;
// within each, group by (stage, params) with ≥ 2 members forming a run; everything else stays solo (-1).
[[nodiscard]] inline RegionBatchGrouping groupRegionBatches(std::span<const BatchStep> steps) {
    RegionBatchGrouping g;
    g.stepRun.assign(steps.size(), -1);
    std::size_t i = 0;
    while (i < steps.size()) {
        if (!steps[i].eligible) { ++i; continue; }
        std::size_t j = i;                                   // maximal eligible stretch [i, j)
        while (j < steps.size() && steps[j].eligible) ++j;
        for (std::size_t a = i; a < j; ++a) {
            if (g.stepRun[a] != -1) continue;                // already claimed by an earlier group
            std::vector<std::uint32_t> members{static_cast<std::uint32_t>(a)};
            for (std::size_t b = a + 1; b < j; ++b) {
                if (g.stepRun[b] == -1 && steps[b].stage == steps[a].stage &&
                    sameBatchParams(steps[b].params, steps[a].params)) {
                    members.push_back(static_cast<std::uint32_t>(b));
                }
            }
            if (members.size() >= 2) {
                const int runIdx = static_cast<int>(g.runs.size());
                for (const std::uint32_t m : members) g.stepRun[m] = runIdx;
                g.runs.push_back(RegionBatchRun{steps[a].stage, std::move(members)});
            }
        }
        i = j;
    }
    return g;
}

// ── Gathered region rendering (the replace-effect fast path) ─────────────────────────────────────
//
// The instanced-additive batching above collapses N same-shader ADDITIVE regions into one additive pass —
// but only additive effects, whose delta against a zero source IS the effect. A source-DEPENDENT custom
// shader (sampleSource(uv + duv) — a reality-warp wake that refracts the image beneath it) has no such
// delta, so that path leaves N regions as ~2N serialized replace passes: the O(N) cliff for the replace
// class. Gathering closes it with ONE fullscreen pass per same-stage run that reads the previous image once
// and, per fragment, tests "inside any of the N shapes" (records from a storage buffer, O(N) ALU with a
// uv-bbox quick-reject) and applies the effect with the winning region's own params; outside every shape,
// exact passthrough. Routing is AUTOMATIC (a custom stage can only see sampleSource + its own params, so the
// gathered output is never garbage) — the developer writes zero extra lines. These pure helpers are the CPU
// side the renderer drives: the eligibility predicate, the run grouping (which differs from the additive
// grouping — replace effects do not commute), and the per-run GPU record packing.

// Whether a confined step CAN take the gather path — the SAME shape criteria as the additive path
// (regionBatchEligible): a Custom effect, Normal-blend at full alpha, no paramTable, a plain circle /
// capsule with no invert / stroke / curve / transform (the shapes whose gate the gather fragment replicates
// exactly). The two predicates share this set by design; the two dispositions never contend because the
// renderer ANDs in a DIFFERENT runtime fact — the additive path requires a batched (additive-declared)
// pipeline, gathering requires a gather pipeline, and a stage has one XOR the other XOR neither (the
// emission rule excludes additive- and no-gather-declared shaders from gather). Delegating keeps the single
// shape-eligibility authority; a change to the shapes the gate fragment can replicate updates both at once.
[[nodiscard]] inline bool gatherEligible(const ScreenSpaceEffect& eff, const ShapePoints& shape,
                                         float regionAlpha, BlendMode regionBlend) noexcept {
    return regionBatchEligible(eff, shape, regionAlpha, regionBlend);
}

// One step's gather key: whether it is eligible and its stage handle. Params are DELIBERATELY absent — a
// gather run's whole point is per-region params riding the records (e.g. a warp whose amplitude tapers per
// circle), so grouping by params would find zero runs when the params vary. The renderer builds these from
// the confined steps (eligibility ANDed with the gather-pipeline existence).
struct GatherStep {
    bool          eligible = false;
    std::uint32_t stage    = 0;   // PostProcessStageId (customShader handle) as a uint
};

// One gather run: the stage + the ascending CONTIGUOUS indices (into the site's step list) of the ≥ 2
// gather-eligible same-stage steps it collapses. The renderer issues one gather pass per run, uploading one
// record per step. Contiguous by construction (see groupGatherRuns) — the site walk treats [first..last]
// as one step issued at `first`, no skip-scatter.
struct GatherRun {
    std::uint32_t              stage = 0;
    std::vector<std::uint32_t> steps;   // indices into the input; size ≥ 2, ascending + contiguous
};

// The grouping result: the runs, plus a per-step disposition — stepRun[i] is the run index step i belongs
// to, or -1 if step i is processed individually (an ineligible step, or an eligible singleton). Same shape
// as RegionBatchGrouping; the renderer walks its step list identically.
struct GatherGrouping {
    std::vector<GatherRun> runs;
    std::vector<int>       stepRun;
};

// Group a site's steps into gather runs. Unlike the additive groupRegionBatches (additive deltas commute, so
// it groups interleaved same-key steps within a contiguous eligible stretch), replace effects do NOT commute
// — so a gather run is a MAXIMAL CONTIGUOUS stretch of gather-eligible steps sharing ONE stage. Any boundary —
// an ineligible step OR a different stage — ends the run. `A₁B₁A₂B₂` yields four singletons (no gather);
// `A₁A₂B₁B₂` yields two runs, chained sequentially by the renderer (run B reads run A's output). Runs of
// ≥ 2 gather; singletons (and every ineligible step) stay solo (-1) on the existing per-region path. Pure
// and device-free: the renderer feeds per-step keys and consumes the returned dispositions.
[[nodiscard]] inline GatherGrouping groupGatherRuns(std::span<const GatherStep> steps) {
    GatherGrouping g;
    g.stepRun.assign(steps.size(), -1);
    std::size_t i = 0;
    while (i < steps.size()) {
        if (!steps[i].eligible) { ++i; continue; }
        std::size_t j = i + 1;                              // maximal contiguous same-stage eligible stretch
        while (j < steps.size() && steps[j].eligible && steps[j].stage == steps[i].stage) ++j;
        if (j - i >= 2) {
            const int runIdx = static_cast<int>(g.runs.size());
            GatherRun run;
            run.stage = steps[i].stage;
            run.steps.reserve(j - i);
            for (std::size_t k = i; k < j; ++k) {
                run.steps.push_back(static_cast<std::uint32_t>(k));
                g.stepRun[k] = runIdx;
            }
            g.runs.push_back(std::move(run));
        }
        i = j;
    }
    return g;
}

// The stride of a gather record in float4s: the 3-float4 (48-byte) header + one float4 per 16 bytes of the
// stage's packed cbuffer. The generated gather shader bakes the SAME value as a compile-time constant
// (kGatherStride, from its reflected cbuffer size), and the CPU uploads the packer's bytes verbatim — so
// this and the shader agree by construction. `packedParamBytes` is the packer's returned size (already a
// 16-byte multiple, or 0 for a parameterless shader); the round-up guards against a non-multiple.
[[nodiscard]] constexpr std::uint32_t gatherRecordFloat4s(std::size_t packedParamBytes) noexcept {
    return 3u + static_cast<std::uint32_t>((packedParamBytes + 15u) / 16u);
}

// Build ONE gathered region's GPU record: the 48-byte header (byte-identical to the renderer's
// GpuRegionBatch / region_batch.vert's RegionBatchRecord — uvBox px→uv, spine viewport px, radius + pads)
// followed by the stage's packed cbuffer bytes, zero-padded to a 16-byte (float4) multiple. The gather
// fragment reads the header (float4 0 = uv bbox for the quick-reject, float4 1 = spine, float4 2.x =
// radius) and retroppLoadParams reads the params from float4 3 onward. The px→uv box math mirrors the
// renderer's toGpuRegionBatch exactly (the single covering-box authority is regionBatchInstance's
// box == regionScissorRect); it is pinned by unit tests. Emitted as raw bytes so the renderer uploads it
// verbatim into the run's storage buffer.
[[nodiscard]] inline std::vector<std::byte>
gatherRecordBytes(const RegionBatchInstance& in, int composeW, int composeH,
                  std::span<const std::byte> packedParams) {
    const float cw = static_cast<float>(composeW > 0 ? composeW : 1);
    const float ch = static_cast<float>(composeH > 0 ? composeH : 1);
    const float header[12] = {
        static_cast<float>(in.box.x) / cw,
        static_cast<float>(in.box.y) / ch,
        static_cast<float>(in.box.x + in.box.width)  / cw,
        static_cast<float>(in.box.y + in.box.height) / ch,
        in.p0.x, in.p0.y, in.p1.x, in.p1.y,
        in.radius, 0.0f, 0.0f, 0.0f,
    };
    const std::size_t paramPadded = ((packedParams.size() + 15u) / 16u) * 16u;
    std::vector<std::byte> bytes(sizeof(header) + paramPadded, std::byte{0});
    std::memcpy(bytes.data(), header, sizeof(header));
    if (!packedParams.empty())
        std::memcpy(bytes.data() + sizeof(header), packedParams.data(), packedParams.size());
    return bytes;
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
    float         strokeWidth = 0.0f;  // stroke band width (viewport px); 0 = filled region. Rides the
                                       // shader's free uInvRow1.w pad lane (the invert flag rides uInvRow0.w).
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
    p.strokeWidth = region.strokeWidth;
    return p;
}

// ── Built-in ColorFill gathering (the first built-in kind on the gather fast path) ─────────────────
//
// N contiguous ColorFill-confined regions — the standard way to draw UI panels, swatches, and meters —
// are the same O(N)-pass cliff the custom gather closes, so they collapse the same way: ONE fullscreen
// pass per run whose fragment walks the run's per-region records. Unlike the custom gather (last
// covering region wins), the ColorFill gather composites EVERY covering record in submission order —
// the general sequential semantics — which is what lets any region alpha and any blend mode ride the
// record: overlapping grades compound and translucent fills stack exactly as the per-region path
// produces them. Output is byte-identical to the per-region path: the shader replicates the float16
// intermediate quantization a value picks up between per-region passes.

// The reserved gather-stage id naming the built-in ColorFill pipeline in the gather key space. Custom
// stage handles are small ascending indices; the top of the uint32 range can never collide with one.
// groupGatherRuns needs no special case — the id is just another stage, so an adjacent custom-gather
// step (or any ineligible step) remains a hard run boundary, preserving composition order.
inline constexpr std::uint32_t kColorFillGatherStage = 0xFFFF'FFFFu;

// The polygon-vertex cap a ColorFill gather record carries — the same 64 the region-select cbuffer
// holds (the renderer asserts the two caps agree). A longer polygon truncates to the first 64 vertices
// in the record packer, exactly as the per-region gate's cbuffer packing does, so the two paths render
// the same (truncated) shape.
inline constexpr std::size_t kColorFillGatherMaxPoints = 64;

// Whether a confined step CAN take the ColorFill gather path. Deliberately wider than the custom
// predicates: any region alpha and any blend mode qualify (they ride the record and composite in-loop),
// as do invert, stroke, a non-identity transform (the inverse homography rides the record), and any
// vertex count (truncation matches the per-region cbuffer). The only shape holdout is a curve boundary
// — the analytic/sampled flavours are follow-up record kinds, and a baked SDF mask cannot batch at all
// (each region binds its own mask texture; records have no per-record texture) — and the only step
// holdout is shapelessness, which never reaches here (an unconfined step is skipped by the plan walk,
// and a shapeless region was already synthesized into a whole-viewport rectangle).
[[nodiscard]] inline bool colorFillGatherEligible(const ScreenSpaceEffect& eff,
                                                  const ShapePoints& shape) noexcept {
    if (eff.kind != ScreenSpaceEffectKind::ColorFill) return false;
    if (!shape.curve.empty()) return false;
    return !shape.points.empty();
}

// The stride of a ColorFill gather record in float4s: the 6-float4 header (uvBox; fill + alpha; the
// three inverse-homography rows with invert/stroke/blend-mode in their w lanes; count + radius) plus
// two vertices per float4. One stride per RUN — resolved from the run's largest (post-truncation)
// vertex count so every record in the run's storage buffer indexes uniformly; the shader receives it
// in the gather-info cbuffer.
[[nodiscard]] constexpr std::uint32_t colorFillGatherStrideFloat4s(std::size_t maxPointCount) noexcept {
    const std::size_t n = maxPointCount < kColorFillGatherMaxPoints ? maxPointCount
                                                                    : kColorFillGatherMaxPoints;
    return 6u + static_cast<std::uint32_t>((n + 1u) / 2u);
}

// Build ONE ColorFill-gathered region's GPU record, padded to the run's stride. Field layout (float4s):
//
//   0 : uvBox u0, v0, u1, v1      — the covering quad px→uv (regionBatchInstance's box, the same
//                                    single authority the custom gather header uses); the quick-reject
//   1 : fill r, g, b, regionAlpha — colorFillParams (normalized × fillIntensity) + the Region's alpha
//   2 : invRow0.xyz, invert flag  — the region transform's inverse homography rows + the
//   3 : invRow1.xyz, strokeWidth    region_select cbuffer's exact w-lane packing (one convention,
//   4 : invRow2.xyz, blend mode     no second authority)
//   5 : count, radius, 0, 0       — the effective (post-truncation) vertex count + the SDF radius
//   6+: vertices, two per float4, zero-padded to the stride
//
// regionParams supplies the inverse rows / count / radius / stroke (the single CPU authority the
// per-region cbuffer also draws from); the px→uv box math mirrors gatherRecordBytes. Emitted as raw
// bytes so the renderer uploads records verbatim into the run's pooled storage buffer.
[[nodiscard]] inline std::vector<std::byte>
colorFillGatherRecordBytes(const ShapePoints& shape, const ScreenSpaceEffect& eff, float regionAlpha,
                           BlendMode regionBlend, PixelSize viewport, const RegionBatchInstance& in,
                           int composeW, int composeH, std::uint32_t strideFloat4s) {
    const RegionParams    rp = regionParams(shape, viewport);
    const ColorFillParams cf = colorFillParams(eff);
    const float cw = static_cast<float>(composeW > 0 ? composeW : 1);
    const float ch = static_cast<float>(composeH > 0 ? composeH : 1);
    const std::size_t n = shape.points.size() < kColorFillGatherMaxPoints ? shape.points.size()
                                                                          : kColorFillGatherMaxPoints;
    std::vector<std::byte> bytes(static_cast<std::size_t>(strideFloat4s) * 16u, std::byte{0});
    const float header[24] = {
        static_cast<float>(in.box.x) / cw,
        static_cast<float>(in.box.y) / ch,
        static_cast<float>(in.box.x + in.box.width)  / cw,
        static_cast<float>(in.box.y + in.box.height) / ch,
        cf.r, cf.g, cf.b, regionAlpha,
        rp.invRow0[0], rp.invRow0[1], rp.invRow0[2], shape.invert ? 1.0f : 0.0f,
        rp.invRow1[0], rp.invRow1[1], rp.invRow1[2], rp.strokeWidth,
        rp.invRow2[0], rp.invRow2[1], rp.invRow2[2], static_cast<float>(regionBlend),
        static_cast<float>(n), rp.radius, 0.0f, 0.0f,
    };
    std::memcpy(bytes.data(), header, sizeof(header));
    for (std::size_t i = 0; i < n; ++i) {
        const float xy[2] = {shape.points[i].x, shape.points[i].y};
        std::memcpy(bytes.data() + sizeof(header) + i * sizeof(xy), xy, sizeof(xy));
    }
    return bytes;
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
[[nodiscard]] inline bool curveRegionContains(Point fragPx, const ShapePoints& region,
                                              bool snap = false) noexcept {
    if (region.curve.empty()) return regionContains(fragPx, region, snap);
    if (snap) fragPx = snapFragToCellCenter(fragPx);
    const Transform inv = region.transform.inverse();
    const Point local{inv.applyX(fragPx.x, fragPx.y), inv.applyY(fragPx.x, fragPx.y)};
    const float d = bandSignedDistance(
        sdCurveAnalytic(local, std::span<const CurveSegment>(region.curve)) - region.radius,
        region.strokeWidth);
    return (d <= 0.0f) != region.invert;  // invert flips inside/outside (region.invert true = the OUTSIDE)
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
    float         strokeWidth  = 0.0f;  // stroke band width (viewport px); 0 = filled region. Rides the
                                        // shader's free uInvRow1.w pad lane.
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
    p.strokeWidth  = region.strokeWidth;
    return p;
}

// ── Curve mask gate (the baked-SDF-texture boundary for cubic / arbitrary curves) ──────────────
//
// A cubic / Catmull-Rom boundary has no closed-form GPU distance, so its Curve::signedDistance is baked
// once into a signed-distance grid, uploaded as a texture, and sampled per fragment — the region_select_
// curve_mask.frag / region_stencil_curve_mask.frag shaders mirror these helpers. The bake is the CPU
// producer below (device-free); the GPU samples the same field with hardware bilinear filtering, which
// sampleCurveMaskField mirrors. radius / stroke / transform / invert compose on the sampled distance
// exactly as on the analytic path.

// The CPU-baked signed-distance field for a closed curve boundary — the device-free producer the renderer
// uploads and the headless tests assert against Curve::signedDistance. `distances` is a width×height grid
// (row-major, top row first) of signed distances (negative inside, positive outside) in shape-local pixels,
// sampled at each texel center across the box [bakeMin, bakeMin + bakeExtent]. A shape-local point maps to a
// grid sample by (local − bakeMin) / bakeExtent. The box is the boundary's control-point bounds inflated by
// `padding` so radius / stroke inflation up to that margin reads a valid distance; a point outside the box
// clamps to the border distance (an unambiguous outside). Empty boundary ⇒ an empty field.
struct CurveMaskField {
    std::vector<float> distances;       // width × height signed distances (shape-local px), row-major
    int                width  = 0;
    int                height = 0;
    Vec2               bakeMin{};        // box min corner (shape-local px)
    Vec2               bakeExtent{};     // box size (shape-local px); local → uv = (local − bakeMin) / extent
};

[[nodiscard]] inline CurveMaskField bakeCurveMaskField(const Curve& boundary, float padding = 8.0f,
                                                       int maxResolution = 256) {
    CurveMaskField f;
    if (boundary.segments.empty()) return f;

    // Control-point bounds (a Bézier segment lies within the convex hull of its live control points), then
    // inflate by padding on every side.
    float minX = boundary.segments.front().p0.x, maxX = minX;
    float minY = boundary.segments.front().p0.y, maxY = minY;
    const auto include = [&](Vec2 v) noexcept {
        minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
        minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
    };
    for (const CurveSegment& s : boundary.segments) {
        include(s.p0);
        include(segmentEnd(s));
        if (s.degree == CurveDegree::Quadratic || s.degree == CurveDegree::Cubic) include(s.p1);
        if (s.degree == CurveDegree::Cubic) include(s.p2);
    }
    f.bakeMin    = Vec2{minX - padding, minY - padding};
    f.bakeExtent = Vec2{(maxX - minX) + 2.0f * padding, (maxY - minY) + 2.0f * padding};
    if (f.bakeExtent.x <= 0.0f) f.bakeExtent.x = 1.0f;
    if (f.bakeExtent.y <= 0.0f) f.bakeExtent.y = 1.0f;

    // Longer axis = maxResolution; the shorter scales by aspect (floored at 1). The field is smooth, so a
    // moderate resolution plus bilinear reconstruction carries the boundary exactly enough.
    const int   res = std::max(1, maxResolution);
    const float ar  = f.bakeExtent.x / f.bakeExtent.y;
    f.width  = ar >= 1.0f ? res : std::max(1, static_cast<int>(static_cast<float>(res) * ar + 0.5f));
    f.height = ar >= 1.0f ? std::max(1, static_cast<int>(static_cast<float>(res) / ar + 0.5f)) : res;

    // Sample Curve::signedDistance (the single source of truth) at every texel center, forcing the boundary
    // closed so the field is signed (negative inside).
    Curve closed = boundary;
    closed.closed = true;
    f.distances.resize(static_cast<std::size_t>(f.width) * static_cast<std::size_t>(f.height));
    for (int y = 0; y < f.height; ++y) {
        const float v  = (static_cast<float>(y) + 0.5f) / static_cast<float>(f.height);
        const float ly = f.bakeMin.y + v * f.bakeExtent.y;
        for (int x = 0; x < f.width; ++x) {
            const float u  = (static_cast<float>(x) + 0.5f) / static_cast<float>(f.width);
            const float lx = f.bakeMin.x + u * f.bakeExtent.x;
            f.distances[static_cast<std::size_t>(y) * static_cast<std::size_t>(f.width) +
                        static_cast<std::size_t>(x)] = closed.signedDistance(Vec2{lx, ly});
        }
    }
    return f;
}

// Bilinear sample of the baked field at a shape-local point, with clamp-to-edge addressing — the CPU mirror
// of the hardware bilinear sampler the mask shaders use. Empty field ⇒ +inf (no boundary).
[[nodiscard]] inline float sampleCurveMaskField(const CurveMaskField& f, Point local) noexcept {
    if (f.width <= 0 || f.height <= 0) return std::numeric_limits<float>::infinity();
    // local → uv → texel-center grid coordinate (the −0.5 places the first texel center at 0).
    const float gx = ((local.x - f.bakeMin.x) / f.bakeExtent.x) * static_cast<float>(f.width)  - 0.5f;
    const float gy = ((local.y - f.bakeMin.y) / f.bakeExtent.y) * static_cast<float>(f.height) - 0.5f;
    const float cx = std::clamp(gx, 0.0f, static_cast<float>(f.width  - 1));
    const float cy = std::clamp(gy, 0.0f, static_cast<float>(f.height - 1));
    const int   x0 = static_cast<int>(cx), y0 = static_cast<int>(cy);
    const int   x1 = std::min(x0 + 1, f.width  - 1);
    const int   y1 = std::min(y0 + 1, f.height - 1);
    const float fx = cx - static_cast<float>(x0), fy = cy - static_cast<float>(y0);
    const auto  at = [&](int x, int y) noexcept {
        return f.distances[static_cast<std::size_t>(y) * static_cast<std::size_t>(f.width) +
                           static_cast<std::size_t>(x)];
    };
    const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
    const float bot = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
    return top + (bot - top) * fy;
}

// Whether a viewport-pixel fragment lies inside a region whose boundary is evaluated by a baked mask `field`
// — the CPU mirror of the region_select_curve_mask.frag gate. Maps the fragment into shape space via the
// region transform inverse, samples the field, then tests the radius-inflated, stroke-banded distance,
// honouring invert.
[[nodiscard]] inline bool curveMaskRegionContains(Point fragPx, const ShapePoints& region,
                                                  const CurveMaskField& field, bool snap = false) noexcept {
    if (snap) fragPx = snapFragToCellCenter(fragPx);
    const Transform inv = region.transform.inverse();
    const Point local{inv.applyX(fragPx.x, fragPx.y), inv.applyY(fragPx.x, fragPx.y)};
    const float d = bandSignedDistance(sampleCurveMaskField(field, local) - region.radius, region.strokeWidth);
    return (d <= 0.0f) != region.invert;
}

// Which evaluation path a region's boundary takes — the renderer's pure routing decision, device-free
// testable. Polygon: no curve (the straight-edged path). Analytic: a linear+quadratic curve (the exact
// closed-form SDF). Mask: a cubic / arbitrary curve WITH a baked mask attached (the SDF-mask texture).
// SampledPolygon: a cubic curve with no mask (sampled to a faceted polygon).
enum class CurveRegionPath : std::uint8_t { Polygon, Analytic, Mask, SampledPolygon };

[[nodiscard]] inline CurveRegionPath regionCurvePath(const ShapePoints& region) noexcept {
    if (region.curve.empty()) return CurveRegionPath::Polygon;
    if (curveRegionIsAnalytic(region.curve)) return CurveRegionPath::Analytic;
    return region.curveMask != CurveMaskId{} ? CurveRegionPath::Mask : CurveRegionPath::SampledPolygon;
}

// ── Stencil (region see-through) ──────────────────────────────────────────────────────────
//
// The subtractive sibling of the region gate: where regionContains confines an effect to ADD inside a
// shape, a Stencil effect makes the layer's own pixels in/around the shape SEE-THROUGH to reveal what is
// behind it (the layers below, or the backdrop). The boundary reuses the region SDF wholesale — sdPolygon for a
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
//   TransparentInside  → 1 - coverage  (deep inside coverage 1 → factor 0 = transparent; outside → 1 = kept)
//   TransparentOutside → coverage      (inside → 1 kept; outside coverage 0 → factor 0 = transparent)
// The fragment output is source * survival across all four (premultiplied) channels.
[[nodiscard]] constexpr float stencilSurvival(StencilMode mode, float coverage) noexcept {
    return mode == StencilMode::TransparentInside ? 1.0f - coverage : coverage;
}

// The stencil stage's resolved parameters — the region SDF params (reused verbatim from regionParams)
// plus the two stencil scalars (mode as a uint, feather). The CPU side of the stencil cbuffer the
// renderer fills (the regionParams discipline). A curve-free region takes this polygon path.
struct StencilParams {
    RegionParams  region;
    std::uint32_t mode    = 0;     // StencilMode as a uint (TransparentInside = 0, TransparentOutside = 1)
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

// ── Sprite effect records (the sprite fragment's inline effect + region evaluation) ───────
//
// A sprite carries an `effects` chain and a `regions` list; both flatten into a contiguous run of
// SpriteFxRecords the sprite fragment loops over per pixel (no added render passes — the pass count
// stays flat in the sprite count). These helpers are the CPU side the renderer drives (buildSpriteFxRecords
// packs a sprite's run) and the device-free oracle the sprite fragment mirrors (evalSpriteFxRecords for the
// colour transform; spriteDisplacedReadUv for the displacing re-read).
//
// A whole-silhouette chain step is EITHER a colour transform (ColorFill / Gleam / ColorSaturation /
// Transparency — realized by
// evalSpriteFxRecords) OR a displacing re-read (RowDisplacement / Ripple — realized by spriteDisplacedReadUv,
// which moves WHERE the art is sampled before the colour transform runs). A displacing effect's params on a
// sprite are in the sprite's OWN art pixels (the re-read space), not viewport pixels as on a layer. Region
// shapes use the polygon path (circle / capsule / polygon + radius / stroke / invert / transform); a
// curve-boundary sprite region is unsupported here and skipped by the packer, as is a displacing kind placed
// inside a region (chain-only).

// Whether a sprite carries any realized effect (a non-None chain effect, or a region holding one). A sprite
// with none takes the fragment's no-effect early-out — the byte-identity path.
[[nodiscard]] inline bool spriteHasEffects(const Sprite& s) noexcept {
    for (const ScreenSpaceEffect& e : s.effects)
        if (e.kind != ScreenSpaceEffectKind::None) return true;
    for (const Region& r : s.regions)
        for (const ScreenSpaceEffect& e : r.effects)
            if (e.kind != ScreenSpaceEffectKind::None) return true;
    return false;
}

// Whether a sprite-region shape is realizable inline (the polygon path). A curve boundary has no inline
// evaluation on the sprite path; the packer skips such a region and the renderer warns.
[[nodiscard]] inline bool spriteRegionShapeSupported(const ShapePoints& shape) noexcept {
    return shape.curve.empty();
}

// Pack one effect step. `isRegion` marks a confined region step (carrying `shape` / `alpha` / `blend`);
// otherwise a whole-silhouette chain step (identity gate, full alpha, Normal blend). The kind params are
// RESOLVED here (ColorFill normalized × fillIntensity; Gleam field copy; ColorSaturation normalized;
// Transparency mode+feather) so the
// fragment reads them directly. A region polygon longer than kSpriteRegionMaxPoints is truncated (the
// renderer warns before calling for a shape it cannot represent).
[[nodiscard]] inline SpriteFxRecord packSpriteFxRecord(const ScreenSpaceEffect& e, bool isRegion,
                                                       const ShapePoints& shape, float alpha,
                                                       BlendMode blend) noexcept {
    SpriteFxRecord r{};
    r.kind  = static_cast<std::uint32_t>(e.kind);
    r.flags = isRegion ? kSpriteFxIsRegion : 0u;
    r.blend = static_cast<std::uint32_t>(isRegion ? blend : BlendMode::Normal);
    r.alpha = isRegion ? alpha : 1.0f;
    switch (e.kind) {
        case ScreenSpaceEffectKind::ColorFill: {
            const ColorFillParams p = colorFillParams(e);
            r.params[0] = p.r; r.params[1] = p.g; r.params[2] = p.b; r.params[3] = 0.0f;
            break;
        }
        case ScreenSpaceEffectKind::Gleam: {
            const GleamParams p = gleamParams(e);
            r.params[0] = p.sweep; r.params[1] = p.width; r.params[2] = p.gain; r.params[3] = p.slant;
            break;
        }
        case ScreenSpaceEffectKind::ColorSaturation:
            r.params[0] = saturationParams(e).saturation;  // normalized 0..1
            break;
        case ScreenSpaceEffectKind::Transparency:
            r.params[0] = static_cast<float>(static_cast<std::uint32_t>(e.stencil));
            r.params[1] = e.feather;
            break;
        case ScreenSpaceEffectKind::RowDisplacement:
            // A whole-silhouette re-read of the sprite's own art. params = (amplitude, frequency, phase, axis);
            // amplitude/center are the sprite's OWN art pixels here (the re-read space), not viewport px. edge
            // rides the flags bit so the fragment reads it beside kind.
            r.params[0] = e.amplitude; r.params[1] = e.frequency; r.params[2] = e.phase;
            r.params[3] = static_cast<float>(static_cast<std::uint32_t>(e.axis));
            if (e.edge == DisplacementEdge::Stretch) r.flags |= kSpriteFxEdgeStretch;
            break;
        case ScreenSpaceEffectKind::Ripple:
            // Radial art re-read. params = (amplitude, frequency, phase, _); the centre (art px) and decay ride
            // the otherwise-idle chain-step gate lanes (radius/strokeWidth/pad0 carry no shape for a chain step).
            r.params[0] = e.amplitude; r.params[1] = e.frequency; r.params[2] = e.phase;
            r.radius = e.center.x; r.strokeWidth = e.center.y; r.pad0 = e.decay;
            if (e.edge == DisplacementEdge::Stretch) r.flags |= kSpriteFxEdgeStretch;
            break;
        case ScreenSpaceEffectKind::Custom:
            // A whole-silhouette custom step: the shader body runs inline (through the sprite-inline variant),
            // and its cbuffer params ride the idle chain lanes (writeSpriteFxCustomParams, filled by the
            // renderer from the shader's packer). Only the edge rides the record here — the wrapper reads it.
            if (e.edge == DisplacementEdge::Stretch) r.flags |= kSpriteFxEdgeStretch;
            break;
        default: break;  // None carries no params
    }
    // Identity gate for a chain step; the region's shape otherwise.
    r.invRow0[0] = 1.0f; r.invRow1[1] = 1.0f; r.invRow2[2] = 1.0f;
    if (isRegion) {
        if (shape.invert) r.flags |= kSpriteFxInvert;
        r.radius      = shape.radius;
        r.strokeWidth = shape.strokeWidth;
        const Transform inv = shape.transform.inverse();
        r.invRow0[0] = inv.m00; r.invRow0[1] = inv.m01; r.invRow0[2] = inv.m02;
        r.invRow1[0] = inv.m10; r.invRow1[1] = inv.m11; r.invRow1[2] = inv.m12;
        r.invRow2[0] = inv.m20; r.invRow2[1] = inv.m21; r.invRow2[2] = inv.m22;
        const std::size_t n = std::min(shape.points.size(), kSpriteRegionMaxPoints);
        r.pointCount = static_cast<std::uint32_t>(n);
        for (std::size_t i = 0; i < n; ++i) {
            r.points[2 * i]     = shape.points[i].x;
            r.points[2 * i + 1] = shape.points[i].y;
        }
    }
    return r;
}

// Flatten a sprite's effects chain (whole-silhouette steps) followed by its regions (each region's effects,
// sharing the region's shape / alpha / blend) into the contiguous record run the fragment reads. None-kind
// effects and curve-boundary regions are skipped (the latter is unsupported inline). Empty when the sprite
// carries no realized effect.
[[nodiscard]] inline std::vector<SpriteFxRecord> buildSpriteFxRecords(const Sprite& s) {
    std::vector<SpriteFxRecord> recs;
    static const ShapePoints kNoShape{};
    for (const ScreenSpaceEffect& e : s.effects) {
        if (e.kind == ScreenSpaceEffectKind::None) continue;
        if (effectIsBelowScope(e)) continue;  // Below-scope steps distort the SCENE — the below-sprite pass, not inline
        recs.push_back(packSpriteFxRecord(e, /*isRegion=*/false, kNoShape, 1.0f, BlendMode::Normal));
    }
    for (const Region& r : s.regions) {
        if (!spriteRegionShapeSupported(r.shape)) continue;
        for (const ScreenSpaceEffect& e : r.effects) {
            if (e.kind == ScreenSpaceEffectKind::None) continue;
            if (effectIsBelowScope(e)) continue;  // a region-confined Below step is packed by the below pass, not inline
            // A displacing kind re-reads the whole silhouette; it has no confined-region meaning (and its packed
            // centre/decay would collide with the region's shape lanes) — skip it inside a region. A Custom kind
            // runs whole-silhouette (its cbuffer params fill the same lanes the region shape needs) — skip it
            // inside a region too (the renderer warns).
            if (e.kind == ScreenSpaceEffectKind::RowDisplacement || e.kind == ScreenSpaceEffectKind::Ripple ||
                e.kind == ScreenSpaceEffectKind::Custom)
                continue;
            recs.push_back(packSpriteFxRecord(e, /*isRegion=*/true, r.shape, clampAlpha(r.alpha), r.blend));
        }
    }
    return recs;
}

// ── Below-scope sprite effects — the scene-facing path ──────────────────────────────────────
//
// A Below-scope chain effect (scope == Below) does not transform the sprite's OWN pixels — it distorts /
// grades the accumulator (the composited scene beneath this sprite's layer), confined to the sprite's
// silhouette (its art alpha coverage). The sprite's art then composites on top via the Layer path. So a
// sprite can carry both: Layer effects shape the art, Below effects shape the scene showing through it.
// The below-sprite pass realizes these (the sprite draws through the scene-reading spriteBelow_ pipeline,
// its rasterized art alpha the silhouette mask). Records pack identically (packSpriteFxRecord) — the
// below-sprite fragment interprets displacement amplitude / centre as VIEWPORT px (it distorts the scene),
// where the Layer path reads them as the sprite's own ART px (it re-reads the art). A Below-scope Custom
// effect routes through a generated scene-read variant (its own pipeline — spriteBelowInlineCustomShader
// selects it, the renderer builds it), so a game's custom shader distorts / grades the scene through the
// silhouette exactly like a frame post-process, its output confined to the coverage. A Below-scope
// Transparency scales the lens strength (blending the grade back toward the untouched scene); a Below-scope
// region grades the scene over its shape ∩ the silhouette. Every effect kind is first-class at Below scope.

// Whether a sprite carries any realized Layer-scope effect — the inline (art-facing) path's gate. A sprite
// with only Below-scope effects has no inline records and takes the byte-identical art draw.
[[nodiscard]] inline bool spriteHasLayerEffects(const Sprite& s) noexcept {
    for (const ScreenSpaceEffect& e : s.effects)
        if (e.kind != ScreenSpaceEffectKind::None && !effectIsBelowScope(e)) return true;
    for (const Region& r : s.regions)
        for (const ScreenSpaceEffect& e : r.effects)
            if (e.kind != ScreenSpaceEffectKind::None && !effectIsBelowScope(e)) return true;
    return false;
}

// Whether a sprite carries any realized Below-scope effect — the below-sprite pass's gate. Counts both
// whole-silhouette chain steps and Below-scope region effects (a region grades the scene over shape ∩
// silhouette).
[[nodiscard]] inline bool spriteHasBelowEffects(const Sprite& s) noexcept {
    for (const ScreenSpaceEffect& e : s.effects)
        if (e.kind != ScreenSpaceEffectKind::None && effectIsBelowScope(e)) return true;
    for (const Region& r : s.regions)
        for (const ScreenSpaceEffect& e : r.effects)
            if (e.kind != ScreenSpaceEffectKind::None && effectIsBelowScope(e)) return true;
    return false;
}

// Whether a sprite carries a Below-scope effect inside a region — a region-confined scene grade (shape ∩
// silhouette). buildSpriteBelowRecords packs these after the whole-silhouette chain steps.
[[nodiscard]] inline bool spriteHasBelowRegionEffects(const Sprite& s) noexcept {
    for (const Region& r : s.regions)
        for (const ScreenSpaceEffect& e : r.effects)
            if (e.kind != ScreenSpaceEffectKind::None && effectIsBelowScope(e)) return true;
    return false;
}

// Whether a Below-scope kind is realized by the BUILT-IN below-sprite fragment (the spriteBelow_ pipeline).
// ColorFill / Gleam / ColorSaturation grade the scene sample; RowDisplacement / Ripple re-read the scene at a
// displaced screen position; Transparency scales the lens's output alpha (its strength), blending the grade
// back toward the untouched scene. Custom routes through a generated scene-read variant (a distinct pipeline —
// spriteBelowInlineCustomShader selects it), NOT this built-in path.
[[nodiscard]] inline bool belowSpriteKindSupported(ScreenSpaceEffectKind kind) noexcept {
    return kind == ScreenSpaceEffectKind::ColorFill || kind == ScreenSpaceEffectKind::Gleam ||
           kind == ScreenSpaceEffectKind::ColorSaturation ||
           kind == ScreenSpaceEffectKind::RowDisplacement || kind == ScreenSpaceEffectKind::Ripple ||
           kind == ScreenSpaceEffectKind::Transparency;
}

// The custom shader a sprite's BELOW pass runs through — the FIRST Below-scope Custom effect in its `effects`
// chain (the scene-facing counterpart to spriteInlineCustomShader). nullopt when the sprite carries no
// Below-scope Custom effect. Routability (does a below-custom variant exist for the handle) is the renderer's
// call, exactly as on the Layer path.
[[nodiscard]] inline std::optional<PostProcessStageId> spriteBelowInlineCustomShader(const Sprite& s) noexcept {
    for (const ScreenSpaceEffect& e : s.effects)
        if (e.kind == ScreenSpaceEffectKind::Custom && effectIsBelowScope(e)) return e.customShader;
    return std::nullopt;
}

// Flatten a sprite's Below-scope effects into the record run the below-sprite fragment reads (the scene-facing
// counterpart to buildSpriteFxRecords): the whole-silhouette chain steps first (built-in kinds — a Custom
// routes through its own pipeline, not here), then each Below-scope region's effects sharing that region's
// shape / alpha / blend. None-kind effects, curve-boundary regions, and a region's displacing / Custom kinds
// are excluded (a displacing kind re-reads the whole silhouette; a Custom runs whole-silhouette). Empty when
// the sprite carries no realized Below-scope effect.
[[nodiscard]] inline std::vector<SpriteFxRecord> buildSpriteBelowRecords(const Sprite& s) {
    std::vector<SpriteFxRecord> recs;
    static const ShapePoints kNoShape{};
    for (const ScreenSpaceEffect& e : s.effects) {
        if (e.kind == ScreenSpaceEffectKind::None) continue;
        if (!effectIsBelowScope(e)) continue;
        if (!belowSpriteKindSupported(e.kind)) continue;  // a Custom routes through the below-custom pipeline
        recs.push_back(packSpriteFxRecord(e, /*isRegion=*/false, kNoShape, 1.0f, BlendMode::Normal));
    }
    for (const Region& r : s.regions) {
        if (!spriteRegionShapeSupported(r.shape)) continue;
        for (const ScreenSpaceEffect& e : r.effects) {
            if (e.kind == ScreenSpaceEffectKind::None) continue;
            if (!effectIsBelowScope(e)) continue;
            // A displacing kind re-reads the whole silhouette (no confined-region meaning, and its packed
            // centre / decay would collide with the region's shape lanes); a Custom runs whole-silhouette (its
            // cbuffer params fill the same lanes the region shape needs) — neither is a region-confined below step.
            if (e.kind == ScreenSpaceEffectKind::RowDisplacement || e.kind == ScreenSpaceEffectKind::Ripple ||
                e.kind == ScreenSpaceEffectKind::Custom)
                continue;
            recs.push_back(packSpriteFxRecord(e, /*isRegion=*/true, r.shape, clampAlpha(r.alpha), r.blend));
        }
    }
    return recs;
}

// A contiguous run of below-sprite lenses that draw through the SAME pipeline — the unit the renderer draws in
// one instanced pass. `first` indexes the layer's below-sprite sequence (draw order), `count` its length, and
// `pipelineKey` the shared pipeline (0 = the built-in scene-reading fragment; handle + 1 = a scene-read custom
// variant). The below pass count tracks the number of runs (the authored pipeline mix), never the sprite
// count: N lenses on one pipeline coalesce into one run.
struct SpriteBelowRun {
    int first = 0;
    int count = 0;
    int pipelineKey = 0;
};

// Split a layer's below-sprite pipeline-key sequence (in draw order) into contiguous same-key runs. A change
// in pipeline key opens a new run; equal adjacent keys coalesce. Empty input yields no runs.
[[nodiscard]] inline std::vector<SpriteBelowRun> groupSpriteBelowRuns(std::span<const int> pipelineKeys) {
    std::vector<SpriteBelowRun> runs;
    for (int i = 0; i < static_cast<int>(pipelineKeys.size()); ++i) {
        if (runs.empty() || runs.back().pipelineKey != pipelineKeys[static_cast<std::size_t>(i)])
            runs.push_back(SpriteBelowRun{i, 0, pipelineKeys[static_cast<std::size_t>(i)]});
        ++runs.back().count;
    }
    return runs;
}

// ── Custom sprite-effect params (the record's idle chain lanes) ─────────────────────────────
//
// A whole-silhouette Custom step carries no shape, so a chain record's shape lanes are idle: params[4] then
// invRow0/1/2 then points — 32 contiguous floats (record texels 2..9) = 128 bytes. The renderer fills these
// with the shader's packed cbuffer bytes (from the shader's generated packer), and the generated sprite loader
// reads them back by texel (2 + register). A cbuffer wider than this budget is an escalation, not a silent
// clamp. Float params only (the sprite-effect store is a float texture — float values round-trip bit-exact).
inline constexpr std::size_t kSpriteFxCustomParamBytes =
    sizeof(float) * (4 + 4 + 4 + 4 + kSpriteRegionMaxPoints * 2);
static_assert(kSpriteFxCustomParamBytes == 128);

// Copy a custom shader's packed cbuffer bytes into a Custom chain record's idle lanes. `bytes` is the
// shader's generated packer output; `n` past the budget is dropped (the renderer validates the size). Writes
// into the record's storage at the params offset — well-defined for the trivially-copyable record.
inline void writeSpriteFxCustomParams(SpriteFxRecord& r, std::span<const std::byte> bytes) noexcept {
    const std::size_t n = std::min(bytes.size(), kSpriteFxCustomParamBytes);
    std::memcpy(reinterpret_cast<std::byte*>(&r) + offsetof(SpriteFxRecord, params), bytes.data(), n);
}

// The float4 the generated sprite loader reads for custom param register `reg` (0-based) — the CPU mirror of
// uFxStore.Load(int3(2 + reg, ri, 0)). reg 0 = params, 1..3 = invRow0/1/2, 4..7 = the points pairs. reg past
// the 8-register (128-byte) budget is out of range.
[[nodiscard]] inline Vec4 spriteFxCustomParamFloat4(const SpriteFxRecord& r, std::size_t reg) noexcept {
    Vec4 v{};
    std::memcpy(&v, reinterpret_cast<const std::byte*>(&r) + offsetof(SpriteFxRecord, params) + reg * sizeof(Vec4),
                sizeof(Vec4));
    return v;
}

// The custom shader a sprite renders through on the inline path — the FIRST Custom effect in its `effects`
// chain (region-confined customs are not inline; a chain custom is the pipeline the whole sprite draws
// through). nullopt when the sprite carries no inline custom effect. A second, DIFFERENT custom in the chain
// is neutralized by the renderer (a visible skip), so the sprite's one pipeline is this first shader's.
[[nodiscard]] inline std::optional<PostProcessStageId> spriteInlineCustomShader(const Sprite& s) noexcept {
    for (const ScreenSpaceEffect& e : s.effects)
        if (e.kind == ScreenSpaceEffectKind::Custom && !effectIsBelowScope(e)) return e.customShader;
    return std::nullopt;
}

// Signed distance from a quad-space point to a record's inline polygon (the sdPolygon degenerations: 1 pt =
// circle, 2 pts = capsule, ≥3 = polygon). pointCount == 0 is "no shape" ⇒ inside everywhere (−inf).
[[nodiscard]] inline float spriteRegionSignedDistance(Point local, const SpriteFxRecord& r) noexcept {
    if (r.pointCount == 0) return -std::numeric_limits<float>::infinity();
    std::array<Point, kSpriteRegionMaxPoints> pts{};
    const std::size_t n = std::min<std::size_t>(r.pointCount, kSpriteRegionMaxPoints);
    for (std::size_t i = 0; i < n; ++i) pts[i] = Point{r.points[2 * i], r.points[2 * i + 1]};
    return bandSignedDistance(sdPolygon(local, std::span<const Point>(pts.data(), n)) - r.radius, r.strokeWidth);
}

// Evaluate a sprite's flattened effect run at one covered pixel — the device-free oracle the sprite fragment
// reproduces. `base` is the sprite's own straight-RGBA pixel (palette colour, before the layer/sprite alpha
// multiply); `u`/`v` are its within-sprite coordinates in [0,1]; `w`/`h` the sprite pixel size. Chain steps
// transform the running colour in order (ColorFill replaces rgb, Gleam adds a keyed sheen, ColorSaturation
// desaturates, Transparency makes
// the whole silhouette see-through); region steps gate on the quad-space shape and grade over the pixel by
// the region's alpha + blend (Transparency instead scales alpha by the stencil survival). Returns nullopt when
// the pixel is fully discarded (a whole-silhouette Transparency, or a region Transparency that punches it out).
[[nodiscard]] inline std::optional<Vec4>
evalSpriteFxRecords(Vec4 base, float u, float v, int w, int h,
                    std::span<const SpriteFxRecord> recs) noexcept {
    Vec4 c = base;  // straight rgba
    for (const SpriteFxRecord& r : recs) {
        const auto kind = static_cast<ScreenSpaceEffectKind>(r.kind);
        const bool isRegion = (r.flags & kSpriteFxIsRegion) != 0u;
        if (!isRegion) {
            switch (kind) {  // whole-silhouette chain step — a direct transform
                case ScreenSpaceEffectKind::ColorFill:
                    c.x = r.params[0]; c.y = r.params[1]; c.z = r.params[2];
                    break;
                case ScreenSpaceEffectKind::Gleam: {
                    const ColorFillRgb g = applyGleam(ColorFillRgb{c.x, c.y, c.z}, u, v,
                                                      GleamParams{r.params[0], r.params[1], r.params[2], r.params[3]});
                    c.x = g.r; c.y = g.g; c.z = g.b;
                    break;
                }
                case ScreenSpaceEffectKind::ColorSaturation: {
                    const ColorFillRgb s = applySaturation(ColorFillRgb{c.x, c.y, c.z}, SaturationParams{r.params[0]});
                    c.x = s.r; c.y = s.g; c.z = s.b;
                    break;
                }
                case ScreenSpaceEffectKind::Transparency: {
                    const float surv = stencilSurvival(static_cast<StencilMode>(static_cast<int>(r.params[0])), 1.0f);
                    if (surv <= 0.0f) return std::nullopt;
                    c.w *= surv;
                    break;
                }
                default: break;  // None / displacing kinds pass through on the v1 sprite path
            }
            continue;
        }
        // Region step: gate in quad space (perspective-correct inverse), ∩ silhouette is implicit (we run only
        // on covered pixels).
        const float qx = u * static_cast<float>(w), qy = v * static_cast<float>(h);
        const float wgt = r.invRow2[0] * qx + r.invRow2[1] * qy + r.invRow2[2];
        const Point local{(r.invRow0[0] * qx + r.invRow0[1] * qy + r.invRow0[2]) / wgt,
                          (r.invRow1[0] * qx + r.invRow1[1] * qy + r.invRow1[2]) / wgt};
        const float d = spriteRegionSignedDistance(local, r);
        if (kind == ScreenSpaceEffectKind::Transparency) {  // survival everywhere from the shape SDF (the stencil rule)
            const float cov  = stencilCoverage(d, r.params[1]);
            const float surv = stencilSurvival(static_cast<StencilMode>(static_cast<int>(r.params[0])), cov);
            c.w *= surv;
            continue;
        }
        const bool inside = (d <= 0.0f) != ((r.flags & kSpriteFxInvert) != 0u);
        if (!inside) continue;
        Vec4 src{r.params[0], r.params[1], r.params[2], r.alpha};  // ColorFill: the fill
        if (kind == ScreenSpaceEffectKind::Gleam) {
            const ColorFillRgb g = applyGleam(ColorFillRgb{c.x, c.y, c.z}, u, v,
                                              GleamParams{r.params[0], r.params[1], r.params[2], r.params[3]});
            src = Vec4{g.r, g.g, g.b, r.alpha};
        } else if (kind == ScreenSpaceEffectKind::ColorSaturation) {
            const ColorFillRgb s = applySaturation(ColorFillRgb{c.x, c.y, c.z}, SaturationParams{r.params[0]});
            src = Vec4{s.r, s.g, s.b, r.alpha};
        }
        c = applyBlendMode(c, src, static_cast<BlendMode>(r.blend));
    }
    return c;
}

// Whether a sprite carries any displacing chain effect (RowDisplacement / Ripple) — the fragment's
// displacement pre-pass runs only for such a sprite; a pure-colour sprite reads its art at the plain
// coordinate.
[[nodiscard]] inline bool spriteHasDisplacement(const Sprite& s) noexcept {
    for (const ScreenSpaceEffect& e : s.effects)
        if ((e.kind == ScreenSpaceEffectKind::RowDisplacement || e.kind == ScreenSpaceEffectKind::Ripple) &&
            !effectIsBelowScope(e))  // a Below-scope displace re-reads the SCENE (the below-sprite pass), not the art
            return true;
    return false;
}

// The source coordinate a sprite's displacing chain resolves to, plus the edge that governs an out-of-art
// read — the device-free oracle the sprite fragment's displacement pre-pass mirrors. `quadUv` is the
// within-sprite coordinate (which for a displacing sprite may lie in the inflated footprint, outside [0,1]);
// each displacing chain effect re-reads the art in the sprite's OWN pixel space, in list order, crisp
// (whole-art-px, viewport-cell-snapped — the sprite path is always the analytic grid). `src` is the composed
// read coordinate; `edge` is the last displacing effect's edge (Blank = an out-of-art read is transparent,
// Stretch = it clamps to the art border). A non-displacing sprite returns quadUv unchanged with Blank.
struct SpriteDisplacedRead {
    Uv               src{};
    DisplacementEdge edge = DisplacementEdge::Blank;
};

[[nodiscard]] inline SpriteDisplacedRead spriteDisplacedRead(Uv quadUv, const Sprite& s) noexcept {
    SpriteDisplacedRead out{quadUv, DisplacementEdge::Blank};
    const PixelSize art{s.size.width, s.size.height};  // the re-read normalization is the sprite's art size
    for (const ScreenSpaceEffect& e : s.effects) {
        if (effectIsBelowScope(e)) continue;  // Below-scope distorts the SCENE, not the art footprint
        if (e.kind == ScreenSpaceEffectKind::RowDisplacement) {
            out.src  = displaceSourceUv(out.src, e, art, /*snap=*/true);
            out.edge = e.edge;
        } else if (e.kind == ScreenSpaceEffectKind::Ripple) {
            out.src  = rippleSourceUv(out.src, e, art, /*snap=*/true);
            out.edge = e.edge;
        }
    }
    return out;
}

// Whether a resolved read coordinate lands off the sprite's art (outside [0,1]²) — the point at which the
// edge policy decides transparent (Blank) vs. border-clamp (Stretch).
[[nodiscard]] constexpr bool spriteReadOffArt(Uv src) noexcept {
    return src.u < 0.0f || src.u >= 1.0f || src.v < 0.0f || src.v >= 1.0f;
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
