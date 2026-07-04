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
