#include "retropp/renderer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>

#include "retropp/asset_policy.h"    // resolveAssetPolicy
#include "retropp/asset_registry.h"  // detail::findEmbeddedAsset
#include "retropp/frame_timing.h"    // frameTiming() — the run loop's sub-tick factor + tick signal
#include "retropp/geometry.h"
#include "retropp/postprocess.h"
#include "retropp/shader_format.h"
#include "retropp/shader_registry.h"
#include "shaders/generated/blend_frag.h"
#include "shaders/generated/blit_frag.h"
#include "shaders/generated/blit_vert.h"
#include "shaders/generated/colorfill_frag.h"
#include "shaders/generated/displace_frag.h"
#include "shaders/generated/gleam_frag.h"
#include "shaders/generated/postprocess_vert.h"
#include "shaders/generated/region_batch_vert.h"
#include "shaders/generated/region_select_curve_frag.h"
#include "shaders/generated/region_select_curve_mask_frag.h"
#include "shaders/generated/region_select_frag.h"
#include "shaders/generated/region_stencil_curve_frag.h"
#include "shaders/generated/region_stencil_curve_mask_frag.h"
#include "shaders/generated/region_stencil_frag.h"
#include "shaders/generated/ripple_frag.h"
#include "shaders/generated/sprite_frag.h"
#include "shaders/generated/sprite_vert.h"
#include "shaders/generated/tile_frag.h"
#include "shaders/generated/tile_vert.h"

namespace retropp {

namespace {

// The backdrop the viewport pass clears to before compositing (opaque black, behind every
// layer) and the letterbox bars around the scaled viewport on the swapchain.
constexpr SDL_FColor kBackdropClear{0.0f, 0.0f, 0.0f, 1.0f};
constexpr SDL_FColor kLetterboxClear{0.0f, 0.0f, 0.0f, 1.0f};

// The offscreen colour intermediates — the viewport composite target, the post-process ping-pong
// scratch, and the per-layer effect scratch — and the captureViewport download all use this format.
// R16G16B16A16_FLOAT lets a colour above 1 survive the effect→blend round-trip (so a Multiply ColorFill
// with fillIntensity > 1 brightens instead of only darkening); the blit to the 8-bit swapchain is the
// single clamp back into displayable range.
constexpr SDL_GPUTextureFormat kViewportColorFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

// Decode an IEEE 754 binary16 (half) bit pattern to float. The float16 viewport texels are halves;
// captureViewport decodes them before quantizing to 8-bit.
inline float halfBitsToFloat(Uint16 h) noexcept {
    const Uint32 sign = static_cast<Uint32>(h & 0x8000u) << 16;
    Uint32       exp  = (h >> 10) & 0x1Fu;
    Uint32       mant = h & 0x03FFu;
    Uint32       bits;
    if (exp == 0u) {
        if (mant == 0u) {
            bits = sign;  // ±0
        } else {          // subnormal half → normalize into a float normal
            exp = 127u - 15u + 1u;
            while ((mant & 0x0400u) == 0u) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x03FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {  // inf / NaN
        bits = sign | 0x7F800000u | (mant << 13);
    } else {  // normal
        bits = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Encode a float to an IEEE 754 binary16 (half) bit pattern — the inverse of halfBitsToFloat, round-to-
// nearest-even. The baked curve-mask field uploads as R16_FLOAT, so each signed-distance sample is encoded to
// a half; a value past the half range saturates to ±inf (a far distance still reads as unambiguously outside).
inline Uint16 floatToHalfBits(float f) noexcept {
    Uint32 bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const Uint32 sign = (bits >> 16) & 0x8000u;
    const Uint32 exp  = (bits >> 23) & 0xFFu;
    Uint32       mant = bits & 0x7FFFFFu;
    if (exp == 0xFFu) return static_cast<Uint16>(sign | 0x7C00u | (mant ? 0x200u : 0u));  // inf / NaN
    const int e = static_cast<int>(exp) - 127 + 15;  // rebias to the half exponent
    if (e >= 0x1F) return static_cast<Uint16>(sign | 0x7C00u);  // overflow → ±inf
    if (e <= 0) {                                    // subnormal or zero
        if (e < -10) return static_cast<Uint16>(sign);  // too small → ±0
        mant |= 0x800000u;                           // restore the implicit leading 1
        const int    shift   = 14 - e;
        Uint32       half    = mant >> shift;
        const Uint32 rem     = mant & ((1u << shift) - 1u);
        const Uint32 halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (half & 1u))) ++half;  // round to nearest even
        return static_cast<Uint16>(sign | half);
    }
    Uint16       half = static_cast<Uint16>(sign | (static_cast<Uint32>(e) << 10) | (mant >> 13));
    const Uint32 rem  = mant & 0x1FFFu;              // the dropped low 13 bits
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;  // round to nearest even (carries into exp)
    return half;
}

// Quantize a colour channel to 8-bit the way the swapchain blit's UNORM write does: round(clamp(v,0,1)·255).
// A float16 headroom colour above 1 clamps to 255 — the single clamp into displayable range.
inline std::uint8_t quantizeChannel(float v) noexcept {
    return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// The Game Boy tile edge length. The atlas grid and tilemap addressing are in these units.
constexpr int kTilePx = 8;

// The palette store texture's row width, in colours. The store is a FLAT array of palette colours
// wrapped into a 2-D texture this many wide; a palette's flat offset + a colour index address the
// texel at (flat % W, flat / W). Palettes pack contiguously (no per-palette padding) and may
// straddle rows; only the final row is padded out to W. The store's height grows with each
// uploadPalette, so palette capacity is W × maxTextureHeight — arbitrary for any real use (no
// per-palette colour cap). 16384 keeps the height minimal for typical palettes, and
// W×4 = 65536 B/row is 256-aligned for backend upload-pitch requirements.
constexpr int kPaletteStoreWidth = 16384;

// Per-layer uniform block — must match tile.frag.hlsl's TileUniforms cbuffer exactly
// (std140-style 16-byte-register packing; no member straddles a 16-byte boundary). Each cell carries
// its own atlas + palette handle directly, so there is no per-layer palette-set or atlas-set table:
// the palette handle IS the flat offset, and the atlas handle indexes the global atlas-region store
// texture both frag stages bind.
struct TileUniforms {
    float scrollX, scrollY;      // register 0
    float layerW, layerH;
    float tilemapW, tilemapH;    // register 1
    float tilePx, alpha;
    float paletteStoreW;         // register 2: palette-store row width (colours); flat offset → (f%W, f/W)
    float composeScale;          //             compose grid ÷ viewport (1 = faithful); output pixel → viewport
    float snap;                  //             1 = snap the transform's destination pixel to the viewport grid
    float pad2;
    float invRow0[4];            // inverse transform homography, rows 0..2 (registers 3..5)
    float invRow1[4];
    float invRow2[4];
    std::uint32_t hasTransform;  // register 6: x = hasTransform (0/1)
    std::uint32_t transformEdge; //              y = footprint edge (0 Blank / 1 Stretch)
    std::uint32_t wrap;          //              z = tilemap wrap mode (0 Repeat / 1 Clamp / 2 Blank)
    std::uint32_t pad3;          //              w pad
};
static_assert(sizeof(TileUniforms) == 112, "TileUniforms must match the HLSL cbuffer layout");

// The sprite vertex stage carries NO uniform buffer: the screen→clip transform is baked CPU-side
// into each GpuSprite (retropp::makeGpuSprite), so the vertex stage is a pure storage-buffer read.
// This sidesteps a Metal [[buffer]]-namespace collision a storage+uniform vertex stage would hit
// under the single-pass shader toolchain.

// Sprite fragment uniform — must match sprite.frag.hlsl's SpriteFragUniforms cbuffer (one 16-byte
// register). Each sprite names its own atlas, so the sheet's store region (storeY, cols) is looked up
// per-sprite from the global atlas-region store the frag binds — not carried here.
struct SpriteFragUniforms {
    float tilePx;        // register 0: tile edge length, pixels
    float alpha;         // layer alpha, [0,1]
    float paletteStoreW; // palette-store row width (colours); flat offset → (f%W, f/W)
    float composeScale;  // compose grid ÷ viewport (1 = faithful) — the analytic branch's output→viewport map
};
static_assert(sizeof(SpriteFragUniforms) == 16, "SpriteFragUniforms must match the HLSL cbuffer");

// Row-displacement stage uniform — must match displace.frag.hlsl's DisplaceUniforms
// cbuffer exactly (two 16-byte registers). Filled from retropp::displaceParams(effect, viewport);
// the layout mirrors DisplaceParams's fields, with the axis carried as a uint.
struct DisplaceFragUniforms {
    float         amplitude, frequency, phase;  // register 0
    std::uint32_t axis;                         //   (0 = Horizontal, 1 = Vertical)
    float         invViewportW, invViewportH;
    std::uint32_t edge;                         //   (0 = Blank, 1 = Stretch)
    std::uint32_t blankTransparent;             //   (0 = opaque backdrop, 1 = transparent) — register 1
    float         snap;                         //   (1 = snap to the viewport grid, crisp) — register 2
    float         pad0, pad1, pad2;
};
static_assert(sizeof(DisplaceFragUniforms) == 48, "DisplaceFragUniforms must match the displace.frag cbuffer");

// Built-in radial-ripple stage uniform — must match ripple.frag.hlsl's RippleUniforms
// cbuffer exactly (two 16-byte registers). Filled from retropp::rippleParams(effect, viewport);
// the layout mirrors RippleParams's fields (centre normalized px→UV, the inverse-viewport amplitude
// scale, the radial decay).
struct RippleFragUniforms {
    float centerU, centerV, amplitude, frequency;  // register 0
    float phase, invViewportW, invViewportH, decay; // register 1
    float snap;                                     // (1 = snap to the viewport grid, crisp) — register 2
    float pad0, pad1, pad2;
};
static_assert(sizeof(RippleFragUniforms) == 48, "RippleFragUniforms must match the ripple.frag cbuffer");

// Built-in colour-fill stage uniform — must match colorfill.frag.hlsl's ColorFillUniforms cbuffer exactly:
// one 16-byte register holding the fill colour (rgb, normalized) + a pad lane. Filled from
// retropp::colorFillParams(effect). The ColorFill stage replaces the pixel rgb with this colour; opacity is
// the layer alpha.
struct ColorFillFragUniforms {
    float r, g, b, pad;   // register 0 — fill colour (normalized) + pad
};
static_assert(sizeof(ColorFillFragUniforms) == 16, "ColorFillFragUniforms must match the colorfill.frag cbuffer");

// Built-in gleam stage uniform — must match gleam.frag.hlsl's GleamUniforms cbuffer exactly (one 16-byte
// register: the sweep/width/gain/slant scalars). Filled from retropp::gleamParams(effect). The stage
// multiplies each pixel by a luminance-keyed diagonal band; gain 0 is identity.
struct GleamFragUniforms {
    float sweep, width, gain, slant;   // register 0
};
static_assert(sizeof(GleamFragUniforms) == 16, "GleamFragUniforms must match the gleam.frag cbuffer");

// Scratch buffer size for a custom effect's cbuffer. A custom shader declares its OWN cbuffer
// (its own named params); the build reflects it and generates a packer (custom_effect_packers.h) that
// writes those params' bytes from the effect's inline fields. The renderer hands the packer a buffer this
// big, then pushes the size the packer reports — it never reads the param fields itself, so its view of
// ScreenSpaceEffect is independent of which params any consumer shader declares. 256 B covers a generous
// cbuffer (16 float4 registers); the packer's size is validated against it.
inline constexpr std::uint32_t kMaxCustomEffectUniformBytes = 256;

// One batched region's GPU instance record — must match region_batch.vert.hlsl's RegionBatchRecord (48 B)
// exactly. uvBox is the covering quad in normalized frame uv (u0,v0,u1,v1 — regionScissorRect converted
// px→uv); spine is the shape's SDF spine (p0.xy, p1.xy) in viewport px; radiusPad.x is the SDF radius
// (yzw padding). Built from a retropp::RegionBatchInstance + the compose dimensions.
struct GpuRegionBatch {
    float uvBox[4];
    float spine[4];
    float radiusPad[4];
};
static_assert(sizeof(GpuRegionBatch) == 48, "GpuRegionBatch must match region_batch.vert's 48-byte record");

// The gather pass's run header — must match the generated GATHER shader's RetroppGatherInfo cbuffer
// (b1, space3) exactly (one 16-byte register). `regionCount` is how many per-region records the run's
// storage buffer holds (the gather entry point's loop bound); the pads round to a full register. Pushed
// at fragment uniform slot 1 (the shader's own params register was rewritten to statics, freeing it).
struct GpuGatherInfo {
    std::uint32_t regionCount;
    std::uint32_t pad0, pad1, pad2;
};
static_assert(sizeof(GpuGatherInfo) == 16, "GpuGatherInfo must match the GATHER shader's RetroppGatherInfo cbuffer");

// The engine-controlled custom-effect cbuffer — must match retropp_effect.hlsli's
// RetroppEngineEffect (b0, space3) exactly. Carries the edge mode sampleSource() obeys (from the effect's
// `edge`: 0 = Blank, transparent outside the frame, the default; 1 = Stretch, clamp / smear), this
// effect's per-row data-table location in the row-data store (rowTableY + rowTableRows; rowTableRows == 0
// ⇒ no table), and the evaluation grid (snap + the viewport dims the generated wrapper's snap math needs;
// see the preamble's evaluation-grid note). The engine fills + pushes this for EVERY custom stage
// (slot 0), so the layer governs the edge and the engine forwards the table + grid, not the shader itself.
struct EngineEffectFragUniforms {
    std::uint32_t edgeClamp;      // 0 = blank, 1 = clamp
    std::uint32_t rowTableY;      // this effect's row-table offset (rows) into the row-data store
    std::uint32_t rowTableRows;   // table row count (0 = no table)
    std::uint32_t snap;           // 1 = Viewport grid (crisp), 0 = Output grid (smooth)
    float viewportW;              // logical viewport dimensions for the snap math
    float viewportH;
    float enginePad0;             // → 32 bytes (two cbuffer registers)
    float enginePad1;
};
static_assert(sizeof(EngineEffectFragUniforms) == 32,
              "EngineEffectFragUniforms must match retropp_effect.hlsli's RetroppEngineEffect cbuffer");

// The polygon-vertex cap the region cbuffer carries (packed two-per-register → uPoints[32] in the
// shader). The ShapePoints API stays unbounded (std::vector); a longer polygon is truncated here and
// warned. True-unbounded counts via a fragment storage buffer are a follow-up (needs on-device bring-up).
inline constexpr int kRegionCbufferMaxPoints = 64;

// Region-select gate uniform — must match region_select.frag.hlsl's RegionUniforms cbuffer
// exactly (37 × 16-byte registers). The ≤64 polygon vertices pack two-per-register (a cbuffer
// array would 16-byte-pad each float2), so points[128] lays out as the shader's `float4 uPoints[32]`.
// The inverse homography + misc register mirror retropp::regionParams; count is a float (uMisc.z), the
// EFFECTIVE (possibly truncated) vertex count, rounded back to a uint in the shader; the trailing
// register carries the region's blend mode (the gate grades the effect over the scene before the alpha mix).
struct RegionSelectFragUniforms {
    float points[2 * kRegionCbufferMaxPoints];  // registers 0..31 : ≤64 vertices, xy packed 2-per-register
    float invRow0[4];                            // register 32
    float invRow1[4];                            // register 33
    float invRow2[4];                            // register 34
    float invViewportW, invViewportH;            // register 35
    float count;                                 //   (the effective vertex count, rounded to uint in the shader)
    float radius;
    float blend;                                 // register 36 : blend mode (BlendMode as float, rounded to uint)
    float snap;                                  //   (uBlend.y) 1 = snap the gate to the viewport grid (crisp)
    float pad1, pad2;
};
static_assert(sizeof(RegionSelectFragUniforms) == 592, "RegionSelectFragUniforms must match the region_select.frag cbuffer");

// Resolve a region + viewport + blend mode into the region_select cbuffer bytes. Mirrors retropp::regionParams
// + packs the vertices two-per-register, truncating past kRegionCbufferMaxPoints (with a warning) and carrying
// the EFFECTIVE count so the shader never reads an unfilled slot. `blend` is the owning Region's blend mode.
RegionSelectFragUniforms makeRegionUniforms(const ShapePoints& region, ViewportResolution viewport,
                                            float alpha, BlendMode blend, float snap) {
    const RegionParams p = regionParams(region, PixelSize{viewport.width, viewport.height});
    RegionSelectFragUniforms u{};
    const std::size_t cap = static_cast<std::size_t>(kRegionCbufferMaxPoints);
    const std::size_t n   = std::min(region.points.size(), cap);
    if (region.points.size() > cap) {
        SDL_Log("retropp: region polygon has %zu vertices; truncated to %d (cbuffer cap)",
                region.points.size(), kRegionCbufferMaxPoints);
    }
    for (std::size_t i = 0; i < n; ++i) {
        u.points[2 * i]     = region.points[i].x;
        u.points[2 * i + 1] = region.points[i].y;
    }
    u.invRow0[0] = p.invRow0[0]; u.invRow0[1] = p.invRow0[1]; u.invRow0[2] = p.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.invRow1[0]; u.invRow1[1] = p.invRow1[1]; u.invRow1[2] = p.invRow1[2]; u.invRow1[3] = p.strokeWidth;
    u.invRow2[0] = p.invRow2[0]; u.invRow2[1] = p.invRow2[1]; u.invRow2[2] = p.invRow2[2]; u.invRow2[3] = alpha;
    u.invViewportW = p.invViewportW;
    u.invViewportH = p.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) vertex count
    u.radius       = p.radius;
    u.blend        = static_cast<float>(blend);
    u.snap         = snap;                    // uBlend.y — 1 = snap the gate to the viewport grid
    return u;
}

// The curve-boundary segment cap the curve region cbuffer carries (two registers per segment). The
// ShapePoints::curve API stays unbounded; a longer boundary is truncated here and warned, mirroring the
// polygon vertex cap. A genuinely longer boundary would move to a fragment storage buffer (its own
// on-device bring-up).
inline constexpr int kCurveRegionMaxSegments = 32;

// Curve region-select gate uniform — must match region_select_curve.frag.hlsl's CurveRegionUniforms
// cbuffer exactly (69 × 16-byte registers). Each segment packs two registers: register A {start.xy,
// control.xy}, register B {end.xy, degree, pad}. The inverse homography + misc tail mirror
// retropp::curveRegionParams; count is the EFFECTIVE (post-truncation) segment count; the trailing
// register carries the region's blend mode (the gate grades the effect over the scene before the alpha mix).
struct CurveRegionSelectFragUniforms {
    float segs[8 * kCurveRegionMaxSegments];  // registers 0..63 : 2 regs/segment (8 floats)
    float invRow0[4];                          // register 64
    float invRow1[4];                          // register 65
    float invRow2[4];                          // register 66
    float invViewportW, invViewportH;          // register 67
    float count;                               //   (the effective segment count, rounded to uint in the shader)
    float radius;
    float blend;                               // register 68 : blend mode (BlendMode as float, rounded to uint)
    float snap;                                //   (uBlend.y) 1 = snap the gate to the viewport grid (crisp)
    float pad1, pad2;
};
static_assert(sizeof(CurveRegionSelectFragUniforms) == 1104,
              "CurveRegionSelectFragUniforms must match the region_select_curve.frag cbuffer (69 registers)");

// Resolve a curve region + viewport + blend mode into the curve region-select cbuffer bytes. Mirrors
// retropp::curveRegionParams + packs the per-segment control points two registers each, truncating past
// kCurveRegionMaxSegments (with a warning) and carrying the EFFECTIVE count so the shader never reads an
// unfilled slot. The boundary is assumed analytic (linear + quadratic); a cubic boundary is sampled to a
// polygon by sampleCurveRegionToPolygon before this path. `blend` is the owning Region's blend mode.
CurveRegionSelectFragUniforms makeCurveRegionUniforms(const ShapePoints& region,
                                                      ViewportResolution viewport, float alpha,
                                                      BlendMode blend, float snap) {
    const CurveRegionParams p = curveRegionParams(region, PixelSize{viewport.width, viewport.height});
    CurveRegionSelectFragUniforms u{};
    const std::size_t cap = static_cast<std::size_t>(kCurveRegionMaxSegments);
    const std::size_t n   = std::min(region.curve.size(), cap);
    if (region.curve.size() > cap) {
        SDL_Log("retropp: curve region has %zu segments; truncated to %d (cbuffer cap)",
                region.curve.size(), kCurveRegionMaxSegments);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const CurveSegment& s = region.curve[i];
        const Vec2 start = s.p0;
        const Vec2 ctrl  = s.degree == CurveDegree::Quadratic ? s.p1 : s.p0;
        const Vec2 end   = segmentEnd(s);
        float* a = &u.segs[8 * i];
        a[0] = start.x; a[1] = start.y; a[2] = ctrl.x; a[3] = ctrl.y;
        a[4] = end.x;   a[5] = end.y;   a[6] = static_cast<float>(static_cast<int>(s.degree)); a[7] = 0.0f;
    }
    u.invRow0[0] = p.invRow0[0]; u.invRow0[1] = p.invRow0[1]; u.invRow0[2] = p.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.invRow1[0]; u.invRow1[1] = p.invRow1[1]; u.invRow1[2] = p.invRow1[2]; u.invRow1[3] = p.strokeWidth;
    u.invRow2[0] = p.invRow2[0]; u.invRow2[1] = p.invRow2[1]; u.invRow2[2] = p.invRow2[2]; u.invRow2[3] = alpha;
    u.invViewportW = p.invViewportW;
    u.invViewportH = p.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) segment count
    u.radius       = p.radius;
    u.blend        = static_cast<float>(blend);
    u.snap         = snap;                    // uBlend.y — 1 = snap the gate to the viewport grid
    return u;
}

// Sample a curve boundary that carries a cubic segment into a faceted closed polygon (the points path):
// the analytic gate handles linear + quadratic exactly, so a cubic (an explicit cubic or a Catmull-Rom
// throughPoints) renders faceted via region_select.frag until the mask-texture path. radius/transform
// ride along; the curve is emptied so the polygon gate is taken. Logged once.
ShapePoints sampleCurveRegionToPolygon(const ShapePoints& region) {
    static bool warned = false;
    if (!warned) {
        SDL_Log("retropp: curve region contains a cubic segment; sampling to a faceted polygon "
                "(linear + quadratic boundaries are exact)");
        warned = true;
    }
    const Curve c{region.curve, /*closed=*/true};
    ShapePoints out;
    out.radius    = region.radius;
    out.transform = region.transform;
    const int n = kRegionCbufferMaxPoints;  // sample at the polygon cap for the smoothest faceting
    out.points.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Vec2 v = c.at(static_cast<float>(i) / static_cast<float>(n));
        out.points.push_back(Point{v.x, v.y});
    }
    return out;
}

// Curve-mask region-select gate uniform — must match region_select_curve_mask.frag.hlsl's CurveMaskUniforms
// cbuffer exactly (5 × 16-byte registers). No per-segment data: the boundary's signed distance lives in the
// baked mask texture; the cbuffer carries the inverse homography (invert in row0.w, stroke in row1.w, alpha in
// row2.w), invViewport/radius/blend, and the shape-local bake box (min + 1/extent) the shader maps fragments
// into. The bake box comes from the renderer's CurveMaskEntry, not the per-frame region.
struct CurveMaskSelectFragUniforms {
    float invRow0[4];                                   // register 0
    float invRow1[4];                                   // register 1
    float invRow2[4];                                   // register 2
    float invViewportW, invViewportH, radius, blend;    // register 3
    float bakeMinX, bakeMinY, invBakeExtentX, invBakeExtentY;  // register 4
    float snap, snapPad0, snapPad1, snapPad2;           // register 5 : uSnap.x = 1 → snap gate to viewport grid
};
static_assert(sizeof(CurveMaskSelectFragUniforms) == 96,
              "CurveMaskSelectFragUniforms must match the region_select_curve_mask.frag cbuffer (6 registers)");

CurveMaskSelectFragUniforms makeCurveMaskSelectUniforms(const ShapePoints& region, Vec2 bakeMin, Vec2 bakeExtent,
                                                        ViewportResolution viewport, float alpha, BlendMode blend,
                                                        float snap) {
    const CurveRegionParams p = curveRegionParams(region, PixelSize{viewport.width, viewport.height});
    CurveMaskSelectFragUniforms u{};
    u.invRow0[0] = p.invRow0[0]; u.invRow0[1] = p.invRow0[1]; u.invRow0[2] = p.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.invRow1[0]; u.invRow1[1] = p.invRow1[1]; u.invRow1[2] = p.invRow1[2]; u.invRow1[3] = p.strokeWidth;
    u.invRow2[0] = p.invRow2[0]; u.invRow2[1] = p.invRow2[1]; u.invRow2[2] = p.invRow2[2]; u.invRow2[3] = alpha;
    u.invViewportW = p.invViewportW;
    u.invViewportH = p.invViewportH;
    u.radius       = p.radius;
    u.blend        = static_cast<float>(blend);
    u.bakeMinX = bakeMin.x; u.bakeMinY = bakeMin.y;
    u.invBakeExtentX = bakeExtent.x > 0.0f ? 1.0f / bakeExtent.x : 0.0f;
    u.invBakeExtentY = bakeExtent.y > 0.0f ? 1.0f / bakeExtent.y : 0.0f;
    u.snap = snap;                            // uSnap.x — 1 = snap the gate to the viewport grid
    return u;
}

// Curve-mask stencil gate uniform — must match region_stencil_curve_mask.frag.hlsl's CurveMaskStencilUniforms
// cbuffer exactly (6 × 16-byte registers). The first 5 registers mirror CurveMaskSelectFragUniforms (row2.w is
// unused for stencil; register 3's .w is the stencil mode, not blend); register 5 appends feather.
struct CurveMaskStencilFragUniforms {
    float invRow0[4];                                   // register 0
    float invRow1[4];                                   // register 1
    float invRow2[4];                                   // register 2
    float invViewportW, invViewportH, radius, mode;     // register 3
    float bakeMinX, bakeMinY, invBakeExtentX, invBakeExtentY;  // register 4
    float feather, snap, pad1, pad2;                    // register 5 : uStencil.y (snap) 1 = viewport grid
};
static_assert(sizeof(CurveMaskStencilFragUniforms) == 96,
              "CurveMaskStencilFragUniforms must match the region_stencil_curve_mask.frag cbuffer (6 registers)");

CurveMaskStencilFragUniforms makeCurveMaskStencilUniforms(const ShapePoints& region, Vec2 bakeMin, Vec2 bakeExtent,
                                                          StencilMode mode, float feather, ViewportResolution viewport,
                                                          float snap) {
    const CurveRegionParams p = curveRegionParams(region, PixelSize{viewport.width, viewport.height});
    CurveMaskStencilFragUniforms u{};
    u.invRow0[0] = p.invRow0[0]; u.invRow0[1] = p.invRow0[1]; u.invRow0[2] = p.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.invRow1[0]; u.invRow1[1] = p.invRow1[1]; u.invRow1[2] = p.invRow1[2]; u.invRow1[3] = p.strokeWidth;
    u.invRow2[0] = p.invRow2[0]; u.invRow2[1] = p.invRow2[1]; u.invRow2[2] = p.invRow2[2]; u.invRow2[3] = 0.0f;
    u.invViewportW = p.invViewportW;
    u.invViewportH = p.invViewportH;
    u.radius       = p.radius;
    u.mode         = static_cast<float>(static_cast<std::uint32_t>(mode));
    u.bakeMinX = bakeMin.x; u.bakeMinY = bakeMin.y;
    u.invBakeExtentX = bakeExtent.x > 0.0f ? 1.0f / bakeExtent.x : 0.0f;
    u.invBakeExtentY = bakeExtent.y > 0.0f ? 1.0f / bakeExtent.y : 0.0f;
    u.feather = feather;
    u.snap    = snap;                         // uStencil.y — 1 = snap the gate to the viewport grid
    return u;
}

// Stencil gate uniform — must match region_stencil.frag.hlsl's StencilUniforms cbuffer exactly (37 ×
// 16-byte registers). The first 36 registers are byte-identical to RegionSelectFragUniforms (the same
// polygon SDF + inverse homography + misc tail); register 36 appends the two stencil scalars (mode as a
// float rounded to uint in the shader, feather in shape-local px). New struct — the region_select cbuffer
// is untouched.
struct StencilFragUniforms {
    float points[2 * kRegionCbufferMaxPoints];  // registers 0..31 : ≤64 vertices, xy packed 2-per-register
    float invRow0[4];                            // register 32
    float invRow1[4];                            // register 33
    float invRow2[4];                            // register 34
    float invViewportW, invViewportH;            // register 35
    float count;                                 //   (the effective vertex count, rounded to uint in the shader)
    float radius;
    float mode;                                  // register 36 : 0 TransparentInside, 1 TransparentOutside (rounded to uint)
    float feather;                               //   shape-local px; 0 = hard edge
    float snap;                                  //   (uStencil.z) 1 = snap the stencil to the viewport grid (crisp)
    float pad1;
};
static_assert(sizeof(StencilFragUniforms) == 592, "StencilFragUniforms must match the region_stencil.frag cbuffer");

// Resolve a region + stencil scalars + viewport into the stencil cbuffer bytes. Mirrors retropp::stencilParams
// + packs the vertices two-per-register, truncating past kRegionCbufferMaxPoints (with a warning) and
// carrying the EFFECTIVE count so the shader never reads an unfilled slot.
StencilFragUniforms makeStencilUniforms(const ShapePoints& region, StencilMode mode, float feather,
                                        ViewportResolution viewport, float snap) {
    const StencilParams p = stencilParams(region, mode, feather, PixelSize{viewport.width, viewport.height});
    StencilFragUniforms u{};
    const std::size_t cap = static_cast<std::size_t>(kRegionCbufferMaxPoints);
    const std::size_t n   = std::min(region.points.size(), cap);
    if (region.points.size() > cap) {
        SDL_Log("retropp: stencil region polygon has %zu vertices; truncated to %d (cbuffer cap)",
                region.points.size(), kRegionCbufferMaxPoints);
    }
    for (std::size_t i = 0; i < n; ++i) {
        u.points[2 * i]     = region.points[i].x;
        u.points[2 * i + 1] = region.points[i].y;
    }
    u.invRow0[0] = p.region.invRow0[0]; u.invRow0[1] = p.region.invRow0[1]; u.invRow0[2] = p.region.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.region.invRow1[0]; u.invRow1[1] = p.region.invRow1[1]; u.invRow1[2] = p.region.invRow1[2]; u.invRow1[3] = p.region.strokeWidth;
    u.invRow2[0] = p.region.invRow2[0]; u.invRow2[1] = p.region.invRow2[1]; u.invRow2[2] = p.region.invRow2[2]; u.invRow2[3] = 0.0f;
    u.invViewportW = p.region.invViewportW;
    u.invViewportH = p.region.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) vertex count
    u.radius       = p.region.radius;
    u.mode         = static_cast<float>(p.mode);
    u.feather      = p.feather;
    u.snap         = snap;                    // uStencil.z — 1 = snap the stencil to the viewport grid
    return u;
}

// Curve stencil gate uniform — must match region_stencil_curve.frag.hlsl's CurveStencilUniforms cbuffer
// exactly (69 × 16-byte registers). The first 68 registers are byte-identical to CurveRegionSelectFragUniforms
// (the per-segment control points + inverse homography + misc tail); register 68 appends the two stencil
// scalars. New struct — the curve region_select cbuffer is untouched.
struct CurveStencilFragUniforms {
    float segs[8 * kCurveRegionMaxSegments];  // registers 0..63 : 2 regs/segment (8 floats)
    float invRow0[4];                          // register 64
    float invRow1[4];                          // register 65
    float invRow2[4];                          // register 66
    float invViewportW, invViewportH;          // register 67
    float count;                               //   (the effective segment count, rounded to uint in the shader)
    float radius;
    float mode;                                // register 68 : 0 TransparentInside, 1 TransparentOutside (rounded to uint)
    float feather;                             //   shape-local px; 0 = hard edge
    float snap;                                //   (uStencil.z) 1 = snap the stencil to the viewport grid (crisp)
    float pad1;
};
static_assert(sizeof(CurveStencilFragUniforms) == 1104,
              "CurveStencilFragUniforms must match the region_stencil_curve.frag cbuffer (69 registers)");

// Resolve a curve region + stencil scalars + viewport into the curve stencil cbuffer bytes. Mirrors
// retropp::curveStencilParams + packs the per-segment control points two registers each, truncating past
// kCurveRegionMaxSegments (with a warning). The boundary is assumed analytic (linear + quadratic); a cubic
// boundary is sampled to a polygon by sampleCurveRegionToPolygon before this path.
CurveStencilFragUniforms makeCurveStencilUniforms(const ShapePoints& region, StencilMode mode, float feather,
                                                  ViewportResolution viewport, float snap) {
    const CurveStencilParams p =
        curveStencilParams(region, mode, feather, PixelSize{viewport.width, viewport.height});
    CurveStencilFragUniforms u{};
    const std::size_t cap = static_cast<std::size_t>(kCurveRegionMaxSegments);
    const std::size_t n   = std::min(region.curve.size(), cap);
    if (region.curve.size() > cap) {
        SDL_Log("retropp: stencil curve region has %zu segments; truncated to %d (cbuffer cap)",
                region.curve.size(), kCurveRegionMaxSegments);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const CurveSegment& s = region.curve[i];
        const Vec2 start = s.p0;
        const Vec2 ctrl  = s.degree == CurveDegree::Quadratic ? s.p1 : s.p0;
        const Vec2 end   = segmentEnd(s);
        float* a = &u.segs[8 * i];
        a[0] = start.x; a[1] = start.y; a[2] = ctrl.x; a[3] = ctrl.y;
        a[4] = end.x;   a[5] = end.y;   a[6] = static_cast<float>(static_cast<int>(s.degree)); a[7] = 0.0f;
    }
    u.invRow0[0] = p.region.invRow0[0]; u.invRow0[1] = p.region.invRow0[1]; u.invRow0[2] = p.region.invRow0[2]; u.invRow0[3] = region.invert ? 1.0f : 0.0f;
    u.invRow1[0] = p.region.invRow1[0]; u.invRow1[1] = p.region.invRow1[1]; u.invRow1[2] = p.region.invRow1[2]; u.invRow1[3] = p.region.strokeWidth;
    u.invRow2[0] = p.region.invRow2[0]; u.invRow2[1] = p.region.invRow2[1]; u.invRow2[2] = p.region.invRow2[2]; u.invRow2[3] = 0.0f;
    u.invViewportW = p.region.invViewportW;
    u.invViewportH = p.region.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) segment count
    u.radius       = p.region.radius;
    u.mode         = static_cast<float>(p.mode);
    u.feather      = p.feather;
    u.snap         = snap;                    // uStencil.z — 1 = snap the stencil to the viewport grid
    return u;
}

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string{what} + ": " + SDL_GetError());
}

// Each generated shader header now exposes a ready-made `retropp::shaders::<stem>` ShaderVariants
// constant (the generator does the assembly), so the renderer binds them directly — no per-stem
// Variants() helpers. createShader still selects the live device's format from the variant.

// numStorageBuffers is the last (additive) parameter so existing call sites — which pass
// (numSamplers, numStorageTextures, numUniformBuffers) positionally — are unaffected; the sprite
// vertex stage is the only consumer (its t0 space0 sprite record buffer).
SDL_GPUShader* createShader(SDL_GPUDevice* device, SDL_GPUShaderStage stage,
                            const ShaderVariants& variants, Uint32 numSamplers,
                            Uint32 numStorageTextures = 0, Uint32 numUniformBuffers = 0,
                            Uint32 numStorageBuffers = 0) {
    const auto chosen = selectShader(SDL_GetGPUShaderFormats(device), variants);
    if (!chosen) fail("no compatible shader format for this GPU device");

    SDL_GPUShaderCreateInfo info{};
    info.code_size            = chosen->first.size;
    info.code                 = chosen->first.data;
    info.entrypoint           = chosen->first.entrypoint;
    info.format               = chosen->second;
    info.stage                = stage;
    info.num_samplers         = numSamplers;
    info.num_storage_textures = numStorageTextures;
    info.num_storage_buffers  = numStorageBuffers;
    info.num_uniform_buffers  = numUniformBuffers;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (!shader) fail("SDL_CreateGPUShader failed");
    return shader;
}

}  // namespace

Renderer::Renderer(SDL_GPUDevice* device, SDL_Window* window, ViewportResolution viewport)
    : device_(device), window_(window), viewport_(viewport) {
    // Detect the Metal backend once: only there does the blocking swapchain acquire busy-wait (see the
    // acquireNonBlocking_ comment in renderer.h). On Metal we acquire non-blocking and let the host
    // loop's frame deadline pace; every other backend keeps the blocking acquire.
    if (const char* driver = SDL_GetGPUDeviceDriver(device_); driver && SDL_strcmp(driver, "metal") == 0) {
        acquireNonBlocking_ = true;
    }

    // Resolve the compose grid — the raster resolution of the offscreen targets and the content pass —
    // and create the four offscreen targets at it. composeScale_ starts at 1 (== the viewport); it is
    // re-resolved per frame once rendering begins (renderFrame → resolveComposeScale → resizeComposeTargets).
    resizeComposeTargets(1);

    // The per-frame row-data store starts as a 1×1 RGBA32F texture so the custom-effect pipeline (which
    // declares one fragment storage texture) always has something to bind, even on a frame with no tables.
    // renderFrame grows + refills it when an effect carries a paramTable.
    {
        SDL_GPUTextureCreateInfo ri{};
        ri.type                 = SDL_GPU_TEXTURETYPE_2D;
        ri.format               = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        ri.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
        ri.width                = 1;
        ri.height               = 1;
        ri.layer_count_or_depth = 1;
        ri.num_levels           = 1;
        ri.sample_count         = SDL_GPU_SAMPLECOUNT_1;
        rowDataStore_ = SDL_CreateGPUTexture(device_, &ri);
        if (!rowDataStore_) fail("SDL_CreateGPUTexture (row-data store) failed");
        rowDataStoreH_ = 1;
    }

    // The batched-region fast path binds this 1×1 transparent-black texture as its SourceTexture: with a
    // zero source, sampleSource() returns 0 everywhere, so an additive custom shader's body returns exactly
    // its source-independent delta D — which the pass's additive blend accumulates. Cleared to (0,0,0,0)
    // once here via a one-shot command buffer (COLOR_TARGET so it can be a clear pass's target; SAMPLER so
    // the batched fragment can sample it). Persists for the renderer's lifetime.
    {
        SDL_GPUTextureCreateInfo zi{};
        zi.type                 = SDL_GPU_TEXTURETYPE_2D;
        zi.format               = kViewportColorFormat;
        zi.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        zi.width                = 1;
        zi.height               = 1;
        zi.layer_count_or_depth = 1;
        zi.num_levels           = 1;
        zi.sample_count         = SDL_GPU_SAMPLECOUNT_1;
        batchZeroSource_ = SDL_CreateGPUTexture(device_, &zi);
        if (!batchZeroSource_) fail("SDL_CreateGPUTexture (batch zero source) failed");
        if (SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_)) {
            SDL_GPUColorTargetInfo t{};
            t.texture     = batchZeroSource_;
            t.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};  // transparent black
            t.load_op     = SDL_GPU_LOADOP_CLEAR;
            t.store_op    = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
            SDL_EndGPURenderPass(p);
            SDL_SubmitGPUCommandBuffer(cmd);
        }
    }

    // Nearest filtering, clamped — the faithful default (bilinear is a runtime SamplingMode;
    // CRT-style filters are a post-process stage). Shared by the tile compositor (atlas sampling)
    // and the blit (viewport sampling).
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter     = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter     = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &samplerInfo);
    if (!sampler_) fail("SDL_CreateGPUSampler failed");

    // Bilinear filtering, same CLAMP_TO_EDGE so the viewport edge never bleeds the letterbox —
    // the blit-only alternate the renderer binds under SamplingMode::Bilinear. The
    // tile/atlas path keeps the nearest sampler above; only the final viewport→swapchain blit
    // swaps to this one. Sampler state is pipeline-independent, so this needs no shader change.
    SDL_GPUSamplerCreateInfo bilinearInfo = samplerInfo;
    bilinearInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    bilinearInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    bilinear_ = SDL_CreateGPUSampler(device_, &bilinearInfo);
    if (!bilinear_) fail("SDL_CreateGPUSampler (bilinear) failed");

    // Tile compositor pipeline: renders into the offscreen viewport target (RGBA8), alpha-
    // blended (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) so per-layer alpha composites back-to-front.
    // The fragment shader binds NO sampler and three read-only storage textures — the indexed
    // atlas (R8_UINT), the tilemap cells (R32_UINT), and the palette store (RGBA8) — plus one
    // uniform buffer (the per-layer block); colour is all integer Load + palette lookup.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::tile_vert, 0, 0, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::tile_frag, 0, 4, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                          = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        tile_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!tile_) fail("SDL_CreateGPUGraphicsPipeline (tile) failed");
    }

    // Sprite compositor pipeline: instanced per-sprite quads (TRIANGLELIST, 6 verts × N
    // instances) drawn into the same offscreen viewport target with the same alpha blend as the
    // tile pipeline, so sprites composite back-to-front with tiles by z. The vertex shader reads
    // ONE read-only storage buffer (the per-layer GpuSprite records, t0 space0) and no uniform
    // (the screen→clip transform is baked into each record); the fragment shader binds two
    // read-only storage textures (indexed atlas, palette store — t0/t1 space2) + one uniform
    // buffer (b0 space3) and no sampler — all integer Load, colour-index-0 discarded for OBJ
    // transparency.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::sprite_vert, 0, 0, 0, 1);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::sprite_frag, 0, 3, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        sprite_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!sprite_) fail("SDL_CreateGPUGraphicsPipeline (sprite) failed");
    }

    // Row-displacement post-process pipeline: a fullscreen-triangle pass that
    // samples the source viewport at a displaced UV and writes a viewport-sized RGBA8 scratch
    // target. Shares postprocess.vert with future stages; the fragment binds one sampled texture
    // (the source) + one uniform (the displacement params) and no storage. No blend — the stage
    // fully replaces its target. The colour target format is the viewport's (RGBA8), NOT the
    // swapchain's, since it renders into post0_/post1_.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::displace_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        displace_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!displace_) fail("SDL_CreateGPUGraphicsPipeline (displace) failed");
    }

    // Per-layer (Layer scope) composite pipeline: the SAME displace shaders, but this
    // one BLENDS its displaced output into target_ rather than replacing a scratch. The isolated
    // layer is rendered alone over a transparent-cleared scratch first (standard alpha blend → a
    // PREMULTIPLIED image), so this composite uses PREMULTIPLIED-OVER factors (ONE / ONE_MINUS_SRC_ALPHA),
    // not SRC_ALPHA/…, which would multiply by alpha a second time and double-darken translucent edges.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::displace_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        displaceBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!displaceBlend_) fail("SDL_CreateGPUGraphicsPipeline (displaceBlend) failed");
    }

    // Built-in radial-ripple post-process pipeline: the second engine effect kind, the
    // SAME shape as displace_ — a fullscreen-triangle pass over postprocess.vert, one sampled source +
    // one uniform (RippleFragUniforms), no blend (replaces its scratch). The runEffect built-in branch
    // dispatches to this by ScreenSpaceEffectKind::Ripple.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::ripple_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        ripple_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!ripple_) fail("SDL_CreateGPUGraphicsPipeline (ripple) failed");
    }

    // Per-layer (Layer scope) ripple composite pipeline: the SAME ripple shaders, premultiplied-over
    // blend onto target_ — mirroring displaceBlend_ (the isolated layer is rendered alone over a
    // transparent-cleared scratch first, so this composites the PREMULTIPLIED result).
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::ripple_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        rippleBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!rippleBlend_) fail("SDL_CreateGPUGraphicsPipeline (rippleBlend) failed");
    }

    // Built-in colour-fill post-process pipeline: the third engine effect kind, the SAME shape as
    // displace_ / ripple_ — a fullscreen-triangle pass over postprocess.vert, one sampled source + one
    // uniform (ColorFillFragUniforms), no blend (replaces its scratch). The runEffect built-in branch
    // dispatches to this by ScreenSpaceEffectKind::ColorFill. The fragment paints a colour onto the pixels
    // it covers (clamp(in*mul+add) then mix to fill); a Region confines it to a shape, so a colour fills
    // that shape — a stroked region becomes a drawn line, a filled region a solid shape.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::colorfill_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        colorFill_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!colorFill_) fail("SDL_CreateGPUGraphicsPipeline (colorFill) failed");
    }

    // Per-layer (Layer scope) colour-fill composite pipeline: the SAME colorfill shaders, premultiplied-
    // over blend onto target_ — mirroring rippleBlend_ (the isolated layer is rendered alone over a
    // transparent-cleared scratch first, so this composites the PREMULTIPLIED result).
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::colorfill_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        colorFillBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!colorFillBlend_) fail("SDL_CreateGPUGraphicsPipeline (colorFillBlend) failed");
    }

    // Built-in gleam post-process pipeline: the SAME shape as ripple_ — a fullscreen-triangle pass over
    // postprocess.vert, one sampled source + one uniform (GleamFragUniforms), no blend (replaces its
    // scratch). The runEffect built-in branch dispatches to this by ScreenSpaceEffectKind::Gleam. The
    // fragment multiplies each pixel by a luminance-keyed diagonal sheen band (the marquee "shine").
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::gleam_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        gleam_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!gleam_) fail("SDL_CreateGPUGraphicsPipeline (gleam) failed");
    }

    // Per-layer (Layer scope) gleam composite pipeline: the SAME gleam shaders, premultiplied-over blend
    // onto target_ — mirroring rippleBlend_ (the isolated layer is rendered alone over a transparent-cleared
    // scratch first, so this composites the PREMULTIPLIED result).
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::gleam_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = kViewportColorFormat;
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        gleamBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!gleamBlend_) fail("SDL_CreateGPUGraphicsPipeline (gleamBlend) failed");
    }

    // Region-select gate pipelines: a fullscreen-triangle pass that reads the effect result
    // (t0) + the original source (t1) and writes `inside(region) ? eff : src`, confining ANY effect to
    // a shape with NO change to the effect shaders. Two variants mirror displace_ / displaceBlend_:
    // regionSelect_ REPLACES its target (frame-level + Below scope); regionSelectBlend_ composites the
    // selected image PREMULTIPLIED-OVER target_ (Layer scope, where eff/src are premultiplied). Both
    // share region_select.frag (2 samplers + 1 uniform), differing only in blend state.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        // 2 samplers (eff t0, src t1) + 1 uniform (b0; carries the ≤64 packed vertices + transform + misc).
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_select_frag, 2, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionSelect_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelect_) fail("SDL_CreateGPUGraphicsPipeline (regionSelect) failed");

        // Same shaders, premultiplied-over blend (ONE / ONE_MINUS_SRC_ALPHA) — the Layer-scope composite-
        // back, mirroring displaceBlend_.
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionSelectBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelectBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionSelectBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Curve region-select gate pipelines: the curve-boundary peer of regionSelect_/regionSelectBlend_,
    // confining an effect to a CLOSED CURVE (analytic linear + quadratic) instead of a straight-edged
    // polygon, exact between control points. Same I/O (2 samplers + 1 uniform, the curve cbuffer) and
    // the same replace / premultiplied-over blend split; only region_select_curve.frag differs.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_select_curve_frag, 2, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionSelectCurve_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelectCurve_) fail("SDL_CreateGPUGraphicsPipeline (regionSelectCurve) failed");

        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionSelectCurveBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelectCurveBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionSelectCurveBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Stencil pipelines (region see-through): a fullscreen-triangle pass that reads ONE source (the layer's
    // rendered pixels, t0), computes the region SDF, and writes `source × survival` — making the layer
    // see-through in/around the shape to reveal what's behind it. The subtractive sibling of the region_select gate
    // (which selects between two textures); a separate pass, so the gate is untouched. Two variants mirror
    // regionSelect_/regionSelectBlend_: regionStencil_ REPLACES its target (frame-level + Below scope);
    // regionStencilBlend_ composites the stenciled image PREMULTIPLIED-OVER target_ (Layer scope). Both
    // share region_stencil.frag (1 sampler + 1 uniform), differing only in blend state.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_stencil_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionStencil_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencil_) fail("SDL_CreateGPUGraphicsPipeline (regionStencil) failed");

        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // src rgb is premultiplied
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionStencilBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencilBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionStencilBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Curve stencil pipelines: the curve-boundary peer of regionStencil_/regionStencilBlend_, erasing
    // along a CLOSED CURVE (analytic linear + quadratic) instead of a straight-edged polygon, exact
    // between control points. Same I/O (1 sampler + 1 uniform, the curve stencil cbuffer) and the same
    // replace / premultiplied-over split; only region_stencil_curve.frag differs.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_stencil_curve_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionStencilCurve_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencilCurve_) fail("SDL_CreateGPUGraphicsPipeline (regionStencilCurve) failed");

        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionStencilCurveBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencilCurveBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionStencilCurveBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Curve-mask region-select gate pipelines: the curve-boundary peer of regionSelectCurve_ that reads the
    // boundary's signed distance from a baked mask texture (t2, bilinear) instead of solving it per segment —
    // for cubic / Catmull-Rom / arbitrary boundaries. Three samplers (eff, src, mask) + the mask cbuffer; the
    // same replace / premultiplied-over split.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_select_curve_mask_frag, 3, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionSelectCurveMask_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelectCurveMask_) fail("SDL_CreateGPUGraphicsPipeline (regionSelectCurveMask) failed");

        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionSelectCurveMaskBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionSelectCurveMaskBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionSelectCurveMaskBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Curve-mask stencil pipelines: the curve-boundary peer of regionStencilCurve_ that reads the boundary's
    // signed distance from a baked mask texture (t1, bilinear) — for cubic / arbitrary boundaries. Two samplers
    // (src, mask) + the mask stencil cbuffer; the same replace / premultiplied-over split.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_stencil_curve_mask_frag, 2, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        regionStencilCurveMask_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencilCurveMask_) fail("SDL_CreateGPUGraphicsPipeline (regionStencilCurveMask) failed");

        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        regionStencilCurveMaskBlend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!regionStencilCurveMaskBlend_) fail("SDL_CreateGPUGraphicsPipeline (regionStencilCurveMaskBlend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Blend composite pipeline: a fullscreen-triangle pass that reads the accumulator (t0) + a container's
    // isolated render (t1) and writes applyBlendMode(dst, src, mode) — the programmable blend that a
    // non-Normal Region / DrawLayer / frame selects, where the fixed-function premultiplied-over composite
    // can only alpha-blend. It REPLACES its target (the full blended RGBA, which the caller swaps into the
    // accumulator), so there is one variant — no blend-state split.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::blend_frag, 2, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        blend_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
        if (!blend_) fail("SDL_CreateGPUGraphicsPipeline (blend) failed");

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
    }

    // Blit pipeline: the fragment shader uses one sampled texture (the viewport); the vertex
    // shader needs none. The pipeline's colour target must match the swapchain — which needs the
    // window. A compose-only renderer (window == nullptr) skips it: it composes + captures the
    // viewport offscreen but never presents, so it has no swapchain and no blit pipeline (blit_ stays
    // null; the destructor and renderFrame's blit section both guard on it).
    if (window_) {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::blit_vert, 0);
        // 1 sampler (the viewport), no uniform buffer — the blit is a plain passthrough sample+write.
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::blit_frag, 1, 0, 0);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragment;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        blit_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragment);
        if (!blit_) fail("SDL_CreateGPUGraphicsPipeline (blit) failed");
    }
}

void Renderer::resizeComposeTargets(int scale) {
    if (scale == composeScale_ && target_ != nullptr) return;  // already at this grid — no reallocation

    composeScale_ = scale;
    const PixelSize compose = composeDimensions(viewport_, composeScale_);
    composeW_ = compose.width;
    composeH_ = compose.height;

    // Release the old targets (if any) before recreating at the new compose grid. SDL_GPU defers the
    // actual free until any in-flight command buffer referencing them completes, so releasing here is
    // safe even mid-loop; a resize only fires on a window-size change, never per steady frame.
    if (layerScratch_) { SDL_ReleaseGPUTexture(device_, layerScratch_); layerScratch_ = nullptr; }
    if (post1_)        { SDL_ReleaseGPUTexture(device_, post1_);        post1_        = nullptr; }
    if (post0_)        { SDL_ReleaseGPUTexture(device_, post0_);        post0_        = nullptr; }
    if (target_)       { SDL_ReleaseGPUTexture(device_, target_);       target_       = nullptr; }

    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = kViewportColorFormat;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width                = static_cast<Uint32>(composeW_);
    texInfo.height               = static_cast<Uint32>(composeH_);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    // Offscreen compose target: the colour target the compositor renders into, and the blit's sampler source.
    target_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!target_) fail("SDL_CreateGPUTexture (viewport) failed");
    // Two scratch targets for the post-process chain: it ping-pongs between them, never writing target_,
    // so two suffice for any stage count. Both COLOR_TARGET (a stage writes one) + SAMPLER (the next stage /
    // the blit reads it). Never touched when frame.postEffects is empty (an empty chain costs nothing).
    post0_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!post0_) fail("SDL_CreateGPUTexture (post0) failed");
    post1_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!post1_) fail("SDL_CreateGPUTexture (post1) failed");
    // Per-layer effect scratch: a Layer-scope effect renders its layer alone here and composites it back
    // displaced; a Below-scope effect displaces the accumulator into here and swaps it with target_. Same
    // format/usage as target_ (the two are interchangeable for the swap).
    layerScratch_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!layerScratch_) fail("SDL_CreateGPUTexture (layerScratch) failed");
}

int Renderer::resolveComposeScale() const {
    if (!interpolation_ || window_ == nullptr) return 1;  // faithful path: compose at viewport resolution
    int w = 0, h = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &w, &h) || w <= 0 || h <= 0) return 1;
    // The window's integer-scale-to-fit factor (clamped): compose at exactly the drawn-region size so the
    // blit is a 1:1 centring copy — fill parity with the faithful path.
    return composeScaleToFit(PixelSize{w, h},
                             PixelSize{viewport_.width, viewport_.height}, kMaxComposeScale);
}

Renderer::~Renderer() {
    releaseSpriteBuffers();
    releaseTilemaps();
    releaseAtlases();
    for (CurveMaskEntry& m : curveMasks_)
        if (m.texture) SDL_ReleaseGPUTexture(device_, m.texture);
    releaseCustomStages();
    releaseBatchResources();
    if (rowDataStore_) SDL_ReleaseGPUTexture(device_, rowDataStore_);
    if (paletteStore_) SDL_ReleaseGPUTexture(device_, paletteStore_);
    if (blit_)          SDL_ReleaseGPUGraphicsPipeline(device_, blit_);
    if (blend_)         SDL_ReleaseGPUGraphicsPipeline(device_, blend_);
    if (regionStencilCurveMaskBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilCurveMaskBlend_);
    if (regionStencilCurveMask_)      SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilCurveMask_);
    if (regionSelectCurveMaskBlend_)  SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectCurveMaskBlend_);
    if (regionSelectCurveMask_)       SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectCurveMask_);
    if (regionStencilCurveBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilCurveBlend_);
    if (regionStencilCurve_)      SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilCurve_);
    if (regionStencilBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilBlend_);
    if (regionStencil_)  SDL_ReleaseGPUGraphicsPipeline(device_, regionStencil_);
    if (regionSelectCurveBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectCurveBlend_);
    if (regionSelectCurve_)      SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectCurve_);
    if (regionSelectBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectBlend_);
    if (regionSelect_)  SDL_ReleaseGPUGraphicsPipeline(device_, regionSelect_);
    if (colorFillBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, colorFillBlend_);
    if (colorFill_)     SDL_ReleaseGPUGraphicsPipeline(device_, colorFill_);
    if (rippleBlend_)   SDL_ReleaseGPUGraphicsPipeline(device_, rippleBlend_);
    if (ripple_)        SDL_ReleaseGPUGraphicsPipeline(device_, ripple_);
    if (displaceBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, displaceBlend_);
    if (displace_)      SDL_ReleaseGPUGraphicsPipeline(device_, displace_);
    if (sprite_)        SDL_ReleaseGPUGraphicsPipeline(device_, sprite_);
    if (tile_)          SDL_ReleaseGPUGraphicsPipeline(device_, tile_);
    if (bilinear_)      SDL_ReleaseGPUSampler(device_, bilinear_);
    if (sampler_)       SDL_ReleaseGPUSampler(device_, sampler_);
    if (layerScratch_)  SDL_ReleaseGPUTexture(device_, layerScratch_);
    if (post1_)         SDL_ReleaseGPUTexture(device_, post1_);
    if (post0_)         SDL_ReleaseGPUTexture(device_, post0_);
    if (target_)        SDL_ReleaseGPUTexture(device_, target_);
}

void Renderer::releaseAtlases() {
    if (atlasStore_) SDL_ReleaseGPUTexture(device_, atlasStore_);
    atlasStore_  = nullptr;
    if (atlasRegionStore_) SDL_ReleaseGPUTexture(device_, atlasRegionStore_);
    atlasRegionStore_ = nullptr;
    atlasStoreW_ = 0;
    atlasStoreH_ = 0;
    atlases_.clear();
}

void Renderer::releaseTilemaps() {
    for (TilemapTex& t : tilemaps_) {
        if (t.texture) SDL_ReleaseGPUTexture(device_, t.texture);
    }
    tilemaps_.clear();
}

void Renderer::releaseSpriteBuffers() {
    for (SpriteBuf& s : spriteBufs_) {
        if (s.buffer) SDL_ReleaseGPUBuffer(device_, s.buffer);
    }
    spriteBufs_.clear();
}

void Renderer::releaseCustomStages() {
    for (SDL_GPUGraphicsPipeline* p : customReplace_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    for (SDL_GPUGraphicsPipeline* p : customBlend_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    for (SDL_GPUGraphicsPipeline* p : customGather_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    for (SDL_GPUGraphicsPipeline* p : customGatherBlend_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    customReplace_.clear();
    customBlend_.clear();
    customGather_.clear();
    customGatherBlend_.clear();
}

void Renderer::releaseBatchResources() {
    for (SDL_GPUGraphicsPipeline* p : customBatched_) {
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    customBatched_.clear();
    for (SDL_GPUBuffer* b : batchInstanceBufs_) {
        if (b) SDL_ReleaseGPUBuffer(device_, b);
    }
    batchInstanceBufs_.clear();
    batchInstanceCaps_.clear();
    if (batchZeroSource_) SDL_ReleaseGPUTexture(device_, batchZeroSource_);
    batchZeroSource_ = nullptr;
}

PostProcessStageId Renderer::registerPostProcessStage(const ShaderVariants& fragment) {
    // Build the pipeline pair from the game's fragment + the shared fullscreen-triangle vertex stage. The
    // resource contract is fixed (the engine injects it): 1 sampled source texture + sampler, 1 read-only
    // storage texture (the row-data store, for an effect's per-row paramTable), and TWO uniform cbuffers —
    // slot 0 = the engine cbuffer (RetroppEngineEffect: the edge mode sampleSource() obeys + the effect's
    // row-table location), slot 1 = the shader's OWN reflected params, filled by its generated packer. Two
    // pipelines, differing only in blend state — the no-blend replace (frame-level / Below scope) and the
    // premultiplied-over blend (Layer scope), exactly mirroring displace_ / displaceBlend_.
    auto buildPipeline = [&](bool blend) -> SDL_GPUGraphicsPipeline* {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragShader = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, fragment, 1, 1, 2);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = kViewportColorFormat;
        if (blend) {
            colorTarget.blend_state.enable_blend          = true;
            colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // premultiplied src
            colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
            colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
        }

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader                         = vertex;
        pipeline.fragment_shader                       = fragShader;
        pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &colorTarget;
        pipeline.target_info.num_color_targets         = 1;
        SDL_GPUGraphicsPipeline* built = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

        SDL_ReleaseGPUShader(device_, vertex);
        SDL_ReleaseGPUShader(device_, fragShader);
        return built;
    };

    SDL_GPUGraphicsPipeline* replace = buildPipeline(/*blend=*/false);
    if (!replace) fail("SDL_CreateGPUGraphicsPipeline (custom replace) failed");
    SDL_GPUGraphicsPipeline* blend = buildPipeline(/*blend=*/true);
    if (!blend) {
        SDL_ReleaseGPUGraphicsPipeline(device_, replace);
        fail("SDL_CreateGPUGraphicsPipeline (custom blend) failed");
    }

    const auto id = static_cast<PostProcessStageId>(customReplace_.size());
    customReplace_.push_back(replace);
    customBlend_.push_back(blend);
    customPackers_.push_back(nullptr);  // parallel to the pipeline pair; set by the path overload
    customBatched_.push_back(nullptr);  // set by the path overload iff the shader is additive-declared
    customGather_.push_back(nullptr);       // set by the path overload iff the shader has a gather variant
    customGatherBlend_.push_back(nullptr);  //   (parallel pair — the premultiplied-over peer)
    return id;
}

PostProcessStageId Renderer::registerPostProcessStage(LiteralPath shaderPath) {
    // The path is a compile-time string literal (LiteralPath enforces this — a non-literal is a compile
    // error, not a runtime miss). Resolve it against the build-time-compiled shader registry (populated by
    // the generated per-target registry TU's static initializers — see retropp_autocompile_shaders /
    // shader_registry.h).
    const std::string_view path = shaderPath.view();
    const ShaderVariants* fragment = detail::findShaderVariants(path);
    if (!fragment) {
        throw std::runtime_error(
            "registerPostProcessStage: no shader compiled for path \"" + std::string(path) +
            "\" — the literal was not referenced in a SCANNED source (the build-time scan reads the "
            "target's source files for .hlsl path literals; a literal sitting only in a header is not "
            "seen), or its spelling differs from the registered literal");
    }
    // Build the pipeline pair, then attach this shader's generated cbuffer packer (reflected from its own
    // cbuffer; null for a parameterless shader). The packer fills the uniform from the effect's inline fields.
    const PostProcessStageId id = registerPostProcessStage(*fragment);
    customPackers_[static_cast<std::size_t>(id)] = detail::findEffectPacker(path);
    // If the shader carries a `// @retropp:additive` declaration, the build compiled a BATCHED variant too;
    // build its instanced-additive pipeline so the renderer can route eligible same-shader regions through
    // ONE pass. Absent the declaration this is null and the stage stays on the per-region path.
    if (const ShaderVariants* batched = detail::findBatchedShaderVariants(path)) {
        customBatched_[static_cast<std::size_t>(id)] = buildBatchedStagePipeline(*batched);
    }
    // If the shader has a GATHER variant (every custom shader EXCEPT additive- / no-gather-declared ones),
    // build its replace + premultiplied-over gather pipeline pair so the renderer can collapse eligible
    // same-stage replace regions into ONE union-shape pass. Absent the variant both are null and the stage
    // stays on the per-region path (the additive and gather paths are disjoint by stage class — a stage has a batched
    // pipeline XOR a gather pipeline XOR neither).
    if (const ShaderVariants* gather = detail::findGatherShaderVariants(path)) {
        const auto sid = static_cast<std::size_t>(id);
        customGather_[sid]      = buildGatherStagePipeline(*gather, /*blend=*/false);
        customGatherBlend_[sid] = buildGatherStagePipeline(*gather, /*blend=*/true);
    }
    return id;
}

SDL_GPUGraphicsPipeline* Renderer::buildBatchedStagePipeline(const ShaderVariants& batchedFragment) {
    // The engine's region_batch vertex stage (instanced covering quads; one storage buffer, no uniform —
    // the sprite-path idiom that dodges Metal's vertex storage+uniform [[buffer]] collision) + the shader's
    // BATCHED fragment variant (same 1 sampler + 1 storage texture + 2 uniforms as the normal variant).
    SDL_GPUShader* vertex = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::region_batch_vert,
                                         /*samplers=*/0, /*storageTex=*/0, /*uniforms=*/0, /*storageBuf=*/1);
    SDL_GPUShader* fragShader = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, batchedFragment, 1, 1, 2);

    // ADDITIVE colour blend (ONE / ONE): each region's delta D accumulates onto the destination — the
    // composite IS the blend, so there is no gate pass. Alpha ZERO / ONE preserves the destination alpha
    // (the per-region path's gate output alpha was the source pixel's own; here dst == source at the site).
    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format                            = kViewportColorFormat;
    colorTarget.blend_state.enable_blend          = true;
    colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo pipeline{};
    pipeline.vertex_shader                         = vertex;
    pipeline.fragment_shader                       = fragShader;
    pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
    pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
    pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
    pipeline.target_info.color_target_descriptions = &colorTarget;
    pipeline.target_info.num_color_targets         = 1;
    SDL_GPUGraphicsPipeline* built = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragShader);
    if (!built) fail("SDL_CreateGPUGraphicsPipeline (batched region) failed");
    return built;
}

SDL_GPUGraphicsPipeline* Renderer::buildGatherStagePipeline(const ShaderVariants& gatherFragment, bool blend) {
    // The shared fullscreen-triangle vertex stage (no per-instance geometry — one triangle covers the frame)
    // + the shader's GATHER fragment variant. Resource contract: 1 sampled source + sampler, 1 storage
    // texture (the row-data store, bound for layout parity — a gather-eligible step carries no paramTable,
    // so it is unread), 1 fragment STORAGE BUFFER (the run's per-region records, t2 space2), and TWO uniforms
    // (b0 = the engine cbuffer, b1 = RetroppGatherInfo — the freed params register carries the run's region
    // count). The replace pipeline (frame-level / Below / mid-chain) writes the gathered image; the blend
    // pipeline (premultiplied-over — ONE / ONE_MINUS_SRC_ALPHA) composites a Normal layer's last gather step
    // onto target_, mirroring runRegionSelect(blend=true) and the customBlend_ state.
    SDL_GPUShader* vertex     = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
    SDL_GPUShader* fragShader = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, gatherFragment,
                                             /*samplers=*/1, /*storageTex=*/1, /*uniforms=*/2, /*storageBuf=*/1);

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = kViewportColorFormat;
    if (blend) {
        colorTarget.blend_state.enable_blend          = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // premultiplied src
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUGraphicsPipelineCreateInfo pipeline{};
    pipeline.vertex_shader                         = vertex;
    pipeline.fragment_shader                       = fragShader;
    pipeline.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline.rasterizer_state.fill_mode            = SDL_GPU_FILLMODE_FILL;
    pipeline.rasterizer_state.cull_mode            = SDL_GPU_CULLMODE_NONE;
    pipeline.rasterizer_state.front_face           = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline.multisample_state.sample_count        = SDL_GPU_SAMPLECOUNT_1;
    pipeline.target_info.color_target_descriptions = &colorTarget;
    pipeline.target_info.num_color_targets         = 1;
    SDL_GPUGraphicsPipeline* built = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragShader);
    if (!built) fail("SDL_CreateGPUGraphicsPipeline (gather region) failed");
    return built;
}

// Core indexed-atlas upload: one palette INDEX per pixel as R32_UINT, so a pixel can address an
// arbitrary palette. Read in-shader by integer Load — no sampler; colour is resolved from
// a palette at render time, not stored here. The public overloads widen 8-/16-bit source indices
// into the 32-bit texel (Texture2D<uint> reads any width identically).
AtlasId Renderer::uploadAtlas32(const std::uint32_t* indices, int width, int height, TransparentIndices transparent) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");

    // Append this atlas to the flat atlas store (mirroring uploadPalette). The store stacks
    // every atlas vertically so a SINGLE map layer can mix tiles from several sheets — TileCell::
    // atlasSelect picks the region. Keep a CPU mirror of each atlas's pixels so the store can be
    // recreated + re-uploaded whole when a new atlas grows it. Uploads are amortized (load time).
    AtlasEntry entry;
    entry.data.assign(indices, indices + static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    entry.width       = width;
    entry.height      = height;
    entry.transparent = transparent;
    atlases_.push_back(std::move(entry));
    const AtlasId id = static_cast<AtlasId>(atlases_.size() - 1);

    rebuildAtlasStore();
    return id;
}

void Renderer::rebuildAtlasStore() {
    if (atlases_.empty()) return;

    // Stack vertically: store width = the widest atlas, height = Σ heights; assign each atlas its top row.
    int W = 0, H = 0;
    for (AtlasEntry& a : atlases_) {
        W = std::max(W, a.width);
        a.storeY = H;
        H += a.height;
    }
    atlasStoreW_ = W;
    atlasStoreH_ = H;

    if (atlasStore_) SDL_ReleaseGPUTexture(device_, atlasStore_);
    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = SDL_GPU_TEXTUREFORMAT_R32_UINT;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    texInfo.width                = static_cast<Uint32>(W);
    texInfo.height               = static_cast<Uint32>(H);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    atlasStore_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!atlasStore_) fail("SDL_CreateGPUTexture (atlas store) failed");

    // Build a W×H buffer: each atlas copied left-aligned into its rows, the rest zero (index 0).
    std::vector<std::uint32_t> upload(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), 0u);
    for (const AtlasEntry& a : atlases_) {
        for (int y = 0; y < a.height; ++y) {
            std::copy_n(a.data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width),
                        static_cast<std::size_t>(a.width),
                        upload.data() + static_cast<std::size_t>(a.storeY + y) * static_cast<std::size_t>(W));
        }
    }

    const Uint32 bytes = static_cast<Uint32>(upload.size()) * static_cast<Uint32>(sizeof(std::uint32_t));
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
    if (!transfer) fail("SDL_CreateGPUTransferBuffer (atlas store) failed");

    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) fail("SDL_MapGPUTransferBuffer (atlas store) failed");
    std::memcpy(mapped, upload.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) fail("SDL_AcquireGPUCommandBuffer (atlas store) failed");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = transfer;
    src.offset          = 0;
    src.pixels_per_row  = static_cast<Uint32>(W);
    src.rows_per_layer  = static_cast<Uint32>(H);
    SDL_GPUTextureRegion dst{};
    dst.texture = atlasStore_;
    dst.w       = static_cast<Uint32>(W);
    dst.h       = static_cast<Uint32>(H);
    dst.d       = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);

    // Global atlas-region table: one R32G32B32A32_UINT texel per AtlasId = (storeY, cols,
    // transparentMaskLo, transparentMaskHi). Both frag stages Load it by a cell's / sprite's atlas
    // handle to resolve that sheet's placement (storeY) and stride (cols) in the flat store, and to
    // test structural transparency: the sheet's transparent-index set is a 64-bit bitmask split across
    // .z (indices 0–31) and .w (indices 32–63), bit i set => index i is a hole. The empty set (None)
    // is mask 0 → both words 0 → nothing discarded. Rebuilt here because each entry's storeY is
    // assigned just above.
    {
        const int N = static_cast<int>(atlases_.size());
        std::vector<std::uint32_t> regions(static_cast<std::size_t>(N) * 4u, 0u);
        for (int i = 0; i < N; ++i) {
            const AtlasEntry& a = atlases_[static_cast<std::size_t>(i)];
            const std::size_t base = static_cast<std::size_t>(i) * 4u;
            regions[base + 0] = static_cast<std::uint32_t>(a.storeY);
            regions[base + 1] = static_cast<std::uint32_t>(a.width / kTilePx);
            regions[base + 2] = static_cast<std::uint32_t>(a.transparent.mask & 0xFFFFFFFFu);
            regions[base + 3] = static_cast<std::uint32_t>(a.transparent.mask >> 32);
        }

        if (atlasRegionStore_) SDL_ReleaseGPUTexture(device_, atlasRegionStore_);
        SDL_GPUTextureCreateInfo rInfo{};
        rInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
        rInfo.format               = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT;
        rInfo.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
        rInfo.width                = static_cast<Uint32>(N);
        rInfo.height               = 1;
        rInfo.layer_count_or_depth = 1;
        rInfo.num_levels           = 1;
        rInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
        atlasRegionStore_ = SDL_CreateGPUTexture(device_, &rInfo);
        if (!atlasRegionStore_) fail("SDL_CreateGPUTexture (atlas region store) failed");

        const Uint32 rBytes = static_cast<Uint32>(regions.size()) * static_cast<Uint32>(sizeof(std::uint32_t));
        SDL_GPUTransferBufferCreateInfo rtbInfo{};
        rtbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        rtbInfo.size  = rBytes;
        SDL_GPUTransferBuffer* rTransfer = SDL_CreateGPUTransferBuffer(device_, &rtbInfo);
        if (!rTransfer) fail("SDL_CreateGPUTransferBuffer (atlas region store) failed");
        void* rMapped = SDL_MapGPUTransferBuffer(device_, rTransfer, false);
        if (!rMapped) fail("SDL_MapGPUTransferBuffer (atlas region store) failed");
        std::memcpy(rMapped, regions.data(), rBytes);
        SDL_UnmapGPUTransferBuffer(device_, rTransfer);

        SDL_GPUCommandBuffer* rCmd = SDL_AcquireGPUCommandBuffer(device_);
        if (!rCmd) fail("SDL_AcquireGPUCommandBuffer (atlas region store) failed");
        SDL_GPUCopyPass* rCopy = SDL_BeginGPUCopyPass(rCmd);
        SDL_GPUTextureTransferInfo rSrc{};
        rSrc.transfer_buffer = rTransfer;
        rSrc.offset          = 0;
        rSrc.pixels_per_row  = static_cast<Uint32>(N);
        rSrc.rows_per_layer  = 1;
        SDL_GPUTextureRegion rDst{};
        rDst.texture = atlasRegionStore_;
        rDst.w       = static_cast<Uint32>(N);
        rDst.h       = 1;
        rDst.d       = 1;
        SDL_UploadToGPUTexture(rCopy, &rSrc, &rDst, false);
        SDL_EndGPUCopyPass(rCopy);
        SDL_SubmitGPUCommandBuffer(rCmd);
        SDL_ReleaseGPUTransferBuffer(device_, rTransfer);
    }
}

CurveMaskId Renderer::bakeCurveMask(const Curve& boundary, float padding, int maxResolution) {
    // Bake Curve::signedDistance over the boundary's box on the CPU (the device-free producer), then upload it
    // as an R16_FLOAT field the gate samples with the bilinear sampler. Bake is amortized (setup), like an atlas.
    const CurveMaskField field = bakeCurveMaskField(boundary, padding, maxResolution);
    if (field.width <= 0 || field.height <= 0) fail("bakeCurveMask: empty boundary");

    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width                = static_cast<Uint32>(field.width);
    texInfo.height               = static_cast<Uint32>(field.height);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(device_, &texInfo);
    if (!tex) fail("SDL_CreateGPUTexture (curve mask) failed");

    std::vector<Uint16> halfData(field.distances.size());
    for (std::size_t i = 0; i < field.distances.size(); ++i) halfData[i] = floatToHalfBits(field.distances[i]);

    const Uint32 bytes = static_cast<Uint32>(halfData.size()) * static_cast<Uint32>(sizeof(Uint16));
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
    if (!transfer) fail("SDL_CreateGPUTransferBuffer (curve mask) failed");
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) fail("SDL_MapGPUTransferBuffer (curve mask) failed");
    std::memcpy(mapped, halfData.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) fail("SDL_AcquireGPUCommandBuffer (curve mask) failed");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = transfer;
    src.offset          = 0;
    src.pixels_per_row  = static_cast<Uint32>(field.width);
    src.rows_per_layer  = static_cast<Uint32>(field.height);
    SDL_GPUTextureRegion dst{};
    dst.texture = tex;
    dst.w       = static_cast<Uint32>(field.width);
    dst.h       = static_cast<Uint32>(field.height);
    dst.d       = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);

    curveMasks_.push_back(CurveMaskEntry{tex, field.bakeMin, field.bakeExtent, field.width, field.height});
    return static_cast<CurveMaskId>(curveMasks_.size());  // 1-based; 0 = none
}

ShapePoints Renderer::bakeCurveRegion(const Curve& boundary, float radius, Transform t, float padding,
                                      int maxResolution) {
    ShapePoints shape = ShapePoints::fromCurve(boundary, radius, t);
    shape.curveMask   = bakeCurveMask(boundary, padding, maxResolution);
    return shape;
}

AtlasId Renderer::uploadAtlas(const std::uint8_t* indices, int width, int height, TransparentIndices transparent) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");
    const std::vector<std::uint32_t> widened(indices,
                                             indices + static_cast<std::size_t>(width) * height);
    return uploadAtlas32(widened.data(), width, height, transparent);
}

AtlasId Renderer::uploadAtlas(const std::uint16_t* indices, int width, int height, TransparentIndices transparent) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");
    const std::vector<std::uint32_t> widened(indices,
                                             indices + static_cast<std::size_t>(width) * height);
    return uploadAtlas32(widened.data(), width, height, transparent);
}

AtlasId Renderer::uploadAtlas(const std::uint32_t* indices, int width, int height, TransparentIndices transparent) {
    return uploadAtlas32(indices, width, height, transparent);
}

AtlasId Renderer::uploadAtlas(const LoadedImage&) {
    // The image → uploadAtlas route is forbidden: it bypasses the slicing system. Load a PNG with
    // loadAtlas() (it slices the image into an addressable AtlasManifest); uploadAtlas is only for raw
    // index arrays you author yourself.
    throw std::logic_error(
        "uploadAtlas does not take images — load a PNG with loadAtlas() (it slices the image into an "
        "AtlasManifest). uploadAtlas is only for raw index arrays you specify yourself.");
}

// Grouping is a manifest concern, not a carve concern: framesPerAnimation is recorded on the manifest
// only for an AnimationSeries sheet (every grid kind carves identically). For other kinds it is left 0
// (ungrouped) regardless of what the caller passed.
static int seriesFrameGroup(ContentKind kind, int framesPerAnimation) noexcept {
    return kind == ContentKind::AnimationSeries ? framesPerAnimation : 0;
}

AtlasManifest Renderer::loadAtlas(LiteralPath path, AssetDimensions assetSize,
                                 ContentKind kind, ReadOrder order, int count, TransparentIndices transparent,
                                 int framesPerAnimation, std::optional<AssetPolicy> policy) {
    // Resolve embed-vs-load: per-call > loadAtlas's per-type default (LoadFromPath). An Embed atlas
    // decodes from the bytes the build baked in, keyed by its logical path; if none were baked we fall
    // through to the disk read.
    if (resolveAssetPolicy(policy, AssetPolicy::LoadFromPath) == AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> bytes = detail::findEmbeddedAsset(path.view());
            !bytes.empty()) {
            return loadAtlasFromMemory(bytes, assetSize, kind, order, count, transparent,
                                       framesPerAnimation);
        }
    }
    // LoadFromPath (or an un-baked Embed): resolve the logical path against the runtime asset root.
    const LoadedImage img = loadPng(assetRoot() / path.c_str());  // throws on missing / decode / RGBA
    const AtlasId atlas =
        uploadAtlas(img.indices.data(), img.width, img.height, transparent);  // uploads ONCE
    return AtlasManifest{atlas,
                         sliceLayout(PixelSize{img.width, img.height}, assetSize, kind, order, count),
                         seriesFrameGroup(kind, framesPerAnimation)};
}

AtlasManifest Renderer::loadAtlasFromMemory(std::span<const std::uint8_t> bytes, AssetDimensions assetSize,
                                           ContentKind kind, ReadOrder order, int count,
                                           TransparentIndices transparent, int framesPerAnimation) {
    const LoadedImage img = loadPngFromMemory(bytes);
    const AtlasId atlas =
        uploadAtlas(img.indices.data(), img.width, img.height, transparent);
    return AtlasManifest{atlas,
                         sliceLayout(PixelSize{img.width, img.height}, assetSize, kind, order, count),
                         seriesFrameGroup(kind, framesPerAnimation)};
}

PaletteId Renderer::loadPaletteImage(LiteralPath path, ReadOrder order, int count,
                                     std::optional<AssetPolicy> policy) {
    // Resolve embed-vs-load: per-call > loadPaletteImage's per-type default (Embed — a palette image is
    // bespoke build-time colour data, like a map PNG). An Embed image decodes from the bytes the build
    // baked in, keyed by its logical path; if none were baked we fall through to the disk read.
    if (resolveAssetPolicy(policy, AssetPolicy::Embed) == AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> bytes = detail::findEmbeddedAsset(path.view());
            !bytes.empty()) {
            return uploadPalette(slicePaletteImage(loadPngFromMemory(bytes), order, count));
        }
    }
    // LoadFromPath (or an un-baked Embed): resolve the logical path against the runtime asset root.
    const LoadedImage img = loadPng(assetRoot() / path.c_str());  // throws on missing / decode
    return uploadPalette(slicePaletteImage(img, order, count));   // slice throws on a non-RGBA source
}

PaletteId Renderer::uploadPalette(std::span<const Rgba8> colors) {
    // The store keeps 16-bit channels; an 8-bit entry widens losslessly (×257) on the way in.
    const PaletteId id = static_cast<PaletteId>(paletteData_.size());
    paletteData_.reserve(paletteData_.size() + colors.size());
    for (const Rgba8 c : colors) paletteData_.push_back(widen(c));
    rebuildPaletteStore();
    return id;
}

PaletteId Renderer::uploadPalette(std::span<const Rgba16> colors) {
    // A 16-bit colour source appends direct — no widening.
    const PaletteId id = static_cast<PaletteId>(paletteData_.size());
    paletteData_.insert(paletteData_.end(), colors.begin(), colors.end());
    rebuildPaletteStore();
    return id;
}

void Renderer::rebuildPaletteStore() {
    // paletteData_ is the FLAT, contiguous CPU mirror; a PaletteId IS an entry's flat offset into it.
    // The store texture is that flat array wrapped kPaletteStoreWidth colours wide, its height grown to
    // fit; palettes pack contiguously (no per-palette padding) and may straddle rows. Uploads are
    // amortized (load time / on change), so recreating + re-uploading the whole store each time is cheap.
    const int W    = kPaletteStoreWidth;
    const int rows = std::max(1, static_cast<int>((paletteData_.size() + static_cast<std::size_t>(W) - 1)
                                                  / static_cast<std::size_t>(W)));

    if (paletteStore_) SDL_ReleaseGPUTexture(device_, paletteStore_);
    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    texInfo.width                = static_cast<Uint32>(W);
    texInfo.height               = static_cast<Uint32>(rows);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    paletteStore_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!paletteStore_) fail("SDL_CreateGPUTexture (palette store) failed");

    // Upload a W×rows buffer: the flat colours followed by opaque-black padding out to the last row.
    std::vector<Rgba16> upload(static_cast<std::size_t>(W) * static_cast<std::size_t>(rows));
    std::copy(paletteData_.begin(), paletteData_.end(), upload.begin());

    const Uint32 bytes = static_cast<Uint32>(upload.size()) * static_cast<Uint32>(sizeof(Rgba16));
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
    if (!transfer) fail("SDL_CreateGPUTransferBuffer (palette store) failed");

    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) fail("SDL_MapGPUTransferBuffer (palette store) failed");
    std::memcpy(mapped, upload.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) fail("SDL_AcquireGPUCommandBuffer (palette store) failed");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = transfer;
    src.offset          = 0;
    src.pixels_per_row  = static_cast<Uint32>(W);
    src.rows_per_layer  = static_cast<Uint32>(rows);
    SDL_GPUTextureRegion dst{};
    dst.texture = paletteStore_;
    dst.w       = static_cast<Uint32>(W);
    dst.h       = static_cast<Uint32>(rows);
    dst.d       = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
}

SDL_GPUTexture* Renderer::composeViewport(SDL_GPUCommandBuffer* cmd, const FrameDrawState& frame,
                                          std::vector<SDL_GPUTransferBuffer*>& scratch,
                                          float alpha, bool interpolate) {
    // Validate + order the layers (throws or warns per the collision policy).
    const std::vector<std::size_t> order = layerDrawOrder(frame.layers, collisionPolicy_);
    // Validate sprite keys frame-wide under the same policy: the interpolator holds ONE sprite map across
    // every layer, so a sprite key must be present and unique frame-wide (a duplicate would reconcile two
    // sprites to one slot).
    validateSpriteKeys(frame.layers, collisionPolicy_);

    // Crisp evaluation: on the Viewport grid the analytic paths (transformed tiles, effect regions, the
    // sampling effects) snap their spatial math to the viewport grid so the upscaled image is pixel-identical
    // to the viewport-resolution rasterization. A mathematical no-op at compose scale 1. Threaded into the
    // tile uniform, the displace/ripple uniforms, and the region/stencil packers below.
    const bool  snap  = evaluationGrid_ == EvaluationGrid::Viewport;
    const float snapF = snap ? 1.0f : 0.0f;

    // Per-effect row-data table locations in the row-data store (built in the copy pass below, read in
    // runEffect). Keyed by the effect's address — stable across this compose, since the effect steps
    // reference the same frame effects by pointer.
    std::unordered_map<const ScreenSpaceEffect*, RowTableLoc> rowTableLocs;

    // ── Copy pass: (re)create + upload each TILES layer's tilemap, each SPRITES layer's buffer. ──
    tilemaps_.resize(frame.layers.size());
    spriteBufs_.resize(frame.layers.size());
    SDL_GPUCopyPass* copy = nullptr;
    for (const std::size_t idx : order) {
        const DrawLayer& layer = frame.layers[idx];
        if (contentKind(layer.content) != LayerContentKind::Tiles) continue;
        const TileContent& tc = std::get<TileContent>(layer.content);
        if (tc.widthInTiles <= 0 || tc.heightInTiles <= 0) continue;

        TilemapTex& slot = tilemaps_[idx];
        if (!slot.texture || slot.widthInTiles != tc.widthInTiles ||
            slot.heightInTiles != tc.heightInTiles) {
            if (slot.texture) SDL_ReleaseGPUTexture(device_, slot.texture);
            SDL_GPUTextureCreateInfo ti{};
            ti.type                 = SDL_GPU_TEXTURETYPE_2D;
            ti.format               = SDL_GPU_TEXTUREFORMAT_R32G32_UINT;  // 2 words/cell: packTileCell
            ti.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
            ti.width                = static_cast<Uint32>(tc.widthInTiles);
            ti.height               = static_cast<Uint32>(tc.heightInTiles);
            ti.layer_count_or_depth = 1;
            ti.num_levels           = 1;
            ti.sample_count         = SDL_GPU_SAMPLECOUNT_1;
            slot.texture = SDL_CreateGPUTexture(device_, &ti);
            if (!slot.texture) fail("SDL_CreateGPUTexture (tilemap) failed");
            slot.widthInTiles  = tc.widthInTiles;
            slot.heightInTiles = tc.heightInTiles;
        }

        const Uint32 count = static_cast<Uint32>(tc.widthInTiles) * static_cast<Uint32>(tc.heightInTiles);
        SDL_GPUTransferBufferCreateInfo tbInfo{};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbInfo.size  = count * static_cast<Uint32>(sizeof(PackedTileCell));  // R32G32_UINT: 2 words/cell
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
        if (!transfer) fail("SDL_CreateGPUTransferBuffer (tilemap) failed");

        auto* dst = static_cast<PackedTileCell*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
        if (!dst) fail("SDL_MapGPUTransferBuffer (tilemap) failed");
        const std::size_t have = std::min<std::size_t>(count, tc.cells.size());
        for (std::size_t k = 0; k < have; ++k) dst[k] = packTileCell(tc.cells[k]);
        for (std::size_t k = have; k < count; ++k) dst[k] = PackedTileCell{};  // pad short maps with cell 0
        SDL_UnmapGPUTransferBuffer(device_, transfer);

        if (!copy) copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = transfer;
        src.offset          = 0;
        src.pixels_per_row  = static_cast<Uint32>(tc.widthInTiles);
        src.rows_per_layer  = static_cast<Uint32>(tc.heightInTiles);
        SDL_GPUTextureRegion region{};
        region.texture = slot.texture;
        region.w       = static_cast<Uint32>(tc.widthInTiles);
        region.h       = static_cast<Uint32>(tc.heightInTiles);
        region.d       = 1;
        SDL_UploadToGPUTexture(copy, &src, &region, false);
        scratch.push_back(transfer);
    }

    // Build + upload each SPRITES layer's GpuSprite storage buffer (each sprite carries its own atlas +
    // palette handle, baked into the record). Grow-only: the buffer is recreated only when this frame's
    // sprite count exceeds the slot's capacity; otherwise it is reused and overwritten in place.
    for (const std::size_t idx : order) {
        const DrawLayer& layer = frame.layers[idx];
        if (contentKind(layer.content) != LayerContentKind::Sprites) continue;
        const SpriteContent& sc = std::get<SpriteContent>(layer.content);
        const int spriteCount = static_cast<int>(sc.sprites.size());
        if (spriteCount <= 0) continue;

        // Placement is the eased float on the interpolation path (sub-pixel on the output grid), falling
        // back to the submission's integer position for an id with no history. The layer scroll eases by
        // the layer id, each sprite by its own id. The sprite clip is baked against the VIEWPORT (clip
        // space is resolution-independent); the output-res target rasterizes the same quad finer, and a
        // fractional position lands it on a different output pixel — smooth motion, crisp texels.
        float lscrollX = static_cast<float>(layer.scroll.x);
        float lscrollY = static_cast<float>(layer.scroll.y);
        if (interpolate) {
            if (const auto ls = interp_.interpolatedLayerScroll(layer.key, alpha)) {
                lscrollX = ls->x;
                lscrollY = ls->y;
            }
        }
        std::vector<GpuSprite> records;
        records.reserve(static_cast<std::size_t>(spriteCount));
        for (const Sprite& s : sc.sprites) {
            float px = static_cast<float>(s.x);
            float py = static_cast<float>(s.y);
            if (interpolate) {
                if (const auto p = interp_.interpolatedSpritePos(s.key, alpha)) {
                    px = p->x;
                    py = p->y;
                }
            }
            records.push_back(makeGpuSprite(s, viewport_.width, viewport_.height,
                                            px, py, lscrollX, lscrollY, layer.transform,
                                            evaluationGrid_));
        }

        SpriteBuf& slot = spriteBufs_[idx];
        if (!slot.buffer || slot.capacity < spriteCount) {
            if (slot.buffer) SDL_ReleaseGPUBuffer(device_, slot.buffer);
            SDL_GPUBufferCreateInfo bi{};
            bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
            bi.size  = static_cast<Uint32>(spriteCount) * static_cast<Uint32>(sizeof(GpuSprite));
            slot.buffer = SDL_CreateGPUBuffer(device_, &bi);
            if (!slot.buffer) fail("SDL_CreateGPUBuffer (sprite) failed");
            slot.capacity = spriteCount;
        }

        const Uint32 bytes = static_cast<Uint32>(spriteCount) * static_cast<Uint32>(sizeof(GpuSprite));
        SDL_GPUTransferBufferCreateInfo tbInfo{};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbInfo.size  = bytes;
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
        if (!transfer) fail("SDL_CreateGPUTransferBuffer (sprite) failed");

        void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
        if (!mapped) fail("SDL_MapGPUTransferBuffer (sprite) failed");
        std::memcpy(mapped, records.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer);

        if (!copy) copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation srcLoc{};
        srcLoc.transfer_buffer = transfer;
        srcLoc.offset          = 0;
        SDL_GPUBufferRegion dstRegion{};
        dstRegion.buffer = slot.buffer;
        dstRegion.offset = 0;
        dstRegion.size   = bytes;
        SDL_UploadToGPUBuffer(copy, &srcLoc, &dstRegion, false);
        scratch.push_back(transfer);
    }
    // ── Row-data store: stack every effect's paramTable into the flat RGBA32F store ──
    // Walk every effect site (frame post-effects, per-layer effects, region effects). Each Custom effect
    // carrying a paramTable gets a vertical region at a storeY; record it by address so runEffect can
    // forward (storeY, rows) to the shader. No tables this frame ⇒ the 1×1 default store stays bound and
    // nothing uploads.
    rowData_.clear();
    auto recordRowTable = [&](const ScreenSpaceEffect& e) {
        if (e.kind != ScreenSpaceEffectKind::Custom || e.paramTable.empty()) return;
        const auto storeY = static_cast<std::uint32_t>(rowData_.size());
        rowData_.insert(rowData_.end(), e.paramTable.begin(), e.paramTable.end());
        rowTableLocs.emplace(&e, RowTableLoc{storeY, static_cast<std::uint32_t>(e.paramTable.size())});
    };
    for (const ScreenSpaceEffect& e : frame.postEffects) recordRowTable(e);
    for (const DrawLayer& layer : frame.layers) {
        for (const ScreenSpaceEffect& e : layer.effects) recordRowTable(e);
        for (const Region& rg : layer.regions)
            for (const ScreenSpaceEffect& e : rg.effects) recordRowTable(e);
    }
    for (const Region& rg : frame.regions)
        for (const ScreenSpaceEffect& e : rg.effects) recordRowTable(e);

    if (!rowData_.empty()) {
        const int rows = static_cast<int>(rowData_.size());
        if (!rowDataStore_ || rows > rowDataStoreH_) {  // grow-only recreate
            if (rowDataStore_) SDL_ReleaseGPUTexture(device_, rowDataStore_);
            SDL_GPUTextureCreateInfo ri{};
            ri.type                 = SDL_GPU_TEXTURETYPE_2D;
            ri.format               = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
            ri.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
            ri.width                = 1;
            ri.height               = static_cast<Uint32>(rows);
            ri.layer_count_or_depth = 1;
            ri.num_levels           = 1;
            ri.sample_count         = SDL_GPU_SAMPLECOUNT_1;
            rowDataStore_ = SDL_CreateGPUTexture(device_, &ri);
            if (!rowDataStore_) fail("SDL_CreateGPUTexture (row-data store) failed");
            rowDataStoreH_ = rows;
        }

        const Uint32 bytes = static_cast<Uint32>(rowData_.size()) * static_cast<Uint32>(sizeof(Vec4));
        SDL_GPUTransferBufferCreateInfo tbInfo{};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbInfo.size  = bytes;
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
        if (!transfer) fail("SDL_CreateGPUTransferBuffer (row-data store) failed");
        void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
        if (!mapped) fail("SDL_MapGPUTransferBuffer (row-data store) failed");
        std::memcpy(mapped, rowData_.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer);

        if (!copy) copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = transfer;
        src.offset          = 0;
        src.pixels_per_row  = 1;
        src.rows_per_layer  = static_cast<Uint32>(rows);
        SDL_GPUTextureRegion dst{};
        dst.texture = rowDataStore_;
        dst.w       = 1;
        dst.h       = static_cast<Uint32>(rows);
        dst.d       = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        scratch.push_back(transfer);
    }

    if (copy) SDL_EndGPUCopyPass(copy);

    // ── Viewport composite (segmented for per-layer screen-space effects) ───────────────────────
    // Layers composite back-to-front into target_. A layer with NO effect draws straight into the
    // accumulator (consecutive such layers batch into one render pass — CLEAR on the first, LOAD
    // after). A per-layer effect splits the loop: a Below-scope effect draws its content into the
    // accumulator then displaces the WHOLE accumulator (this layer + everything beneath) and swaps it
    // in; a Layer-scope effect renders ITS layer alone into layerScratch_ and composites it back
    // displaced. A frame with NO effect layers never splits → a single CLEAR pass over all layers. ──

    // One tile/sprite layer drawn into the given pass — shared by the batched path, the Below content
    // draw, and the isolated-Layer offscreen render. (The per-layer screen-space effect is realized by
    // the caller, not here; this just composites the layer's content.)
    auto drawLayer = [&](SDL_GPURenderPass* pass, std::size_t idx) {
        const DrawLayer& layer = frame.layers[idx];

        if (contentKind(layer.content) == LayerContentKind::Tiles) {
            const TileContent& tc = std::get<TileContent>(layer.content);
            if (tc.widthInTiles <= 0 || tc.heightInTiles <= 0) return;
            const TilemapTex& slot = tilemaps_[idx];
            if (!slot.texture) return;
            if (!atlasStore_ || !paletteStore_ || !atlasRegionStore_) return;  // nothing uploaded → nothing to draw

            TileUniforms u{};
            // Scroll places at the eased float on the interpolation path (a fractional scroll shifts the
            // sampled tile/pixel boundary by whole output pixels between refreshes → smooth), falling back
            // to the submission's integer scroll for a layer with no history.
            float scrollX = static_cast<float>(layer.scroll.x);
            float scrollY = static_cast<float>(layer.scroll.y);
            if (interpolate) {
                if (const auto ls = interp_.interpolatedLayerScroll(layer.key, alpha)) {
                    scrollX = ls->x;
                    scrollY = ls->y;
                }
            }
            u.scrollX   = scrollX;
            u.scrollY   = scrollY;
            u.layerW    = static_cast<float>(composeW_);   // compose grid (output res on the interp path)
            u.layerH    = static_cast<float>(composeH_);
            u.composeScale = static_cast<float>(composeScale_);
            u.snap      = snapF;   // 1 = snap the transform's destination pixel to the viewport grid (crisp)
            u.tilemapW  = static_cast<float>(tc.widthInTiles);
            u.tilemapH  = static_cast<float>(tc.heightInTiles);
            u.tilePx    = static_cast<float>(kTilePx);
            u.alpha     = clampAlpha(layer.alpha);
            u.paletteStoreW = static_cast<float>(kPaletteStoreWidth);

            // Per-layer transform: upload the INVERSE homography (the fragment maps a destination
            // pixel back to content space, perspective divide included) + the footprint edge mode.
            // Identity ⇒ hasTransform 0 ⇒ the fragment takes the faithful untransformed path.
            u.hasTransform  = layer.transform.isIdentity() ? 0u : 1u;
            u.transformEdge = static_cast<std::uint32_t>(layer.transformEdge);
            // Per-layer tilemap wrap mode (Repeat default ⇒ faithful toroidal output).
            u.wrap          = static_cast<std::uint32_t>(tc.wrap);
            const Transform inv = layer.transform.inverse();
            u.invRow0[0] = inv.m00; u.invRow0[1] = inv.m01; u.invRow0[2] = inv.m02; u.invRow0[3] = 0.0f;
            u.invRow1[0] = inv.m10; u.invRow1[1] = inv.m11; u.invRow1[2] = inv.m12; u.invRow1[3] = 0.0f;
            u.invRow2[0] = inv.m20; u.invRow2[1] = inv.m21; u.invRow2[2] = inv.m22; u.invRow2[3] = 0.0f;

            // The tile path is all integer Load — bind four read-only storage textures at t0..t3 (the flat
            // atlas store, this layer's tilemap cells, the palette store, and the global atlas-region table);
            // each cell's atlas + palette handle indexes the stores directly. No sampler.
            SDL_GPUTexture* storageTextures[4] = {atlasStore_, slot.texture, paletteStore_, atlasRegionStore_};
            SDL_BindGPUGraphicsPipeline(pass, tile_);
            SDL_BindGPUFragmentStorageTextures(pass, 0, storageTextures, 4);
            SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle
        } else {  // LayerContentKind::Sprites
            const SpriteContent& sc = std::get<SpriteContent>(layer.content);
            const int spriteCount = static_cast<int>(sc.sprites.size());
            if (spriteCount <= 0) return;
            const SpriteBuf& slot = spriteBufs_[idx];
            if (!slot.buffer) return;
            if (!atlasStore_ || !paletteStore_ || !atlasRegionStore_) return;

            SpriteFragUniforms fu{};
            fu.tilePx        = static_cast<float>(kTilePx);
            fu.alpha         = clampAlpha(layer.alpha);
            fu.paletteStoreW = static_cast<float>(kPaletteStoreWidth);
            fu.composeScale  = static_cast<float>(composeScale_);  // the analytic branch's output→viewport map

            // Instanced per-sprite quads: the vertex stage reads the sprite records (already in clip
            // space) from a storage buffer (t0 space0) — no vertex uniform; the fragment stage reads the
            // flat atlas store, palette store, and the global atlas-region table (t0/t1/t2 space2) + its
            // uniform. Each sprite's atlas + palette handle indexes the stores. 6 verts × spriteCount.
            SDL_GPUTexture* fragStorage[3] = {atlasStore_, paletteStore_, atlasRegionStore_};
            SDL_BindGPUGraphicsPipeline(pass, sprite_);
            SDL_BindGPUVertexStorageBuffers(pass, 0, &slot.buffer, 1);
            SDL_BindGPUFragmentStorageTextures(pass, 0, fragStorage, 3);
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(spriteCount), 0, 0);
        }
    };

    // Whether a screen-space effect can be rendered this frame. A built-in (RowDisplacement / Ripple /
    // ColorFill) always can; a Custom effect is renderable iff its handle indexes a registered stage (its parameters
    // are the standard inline fields, so there is nothing else to validate). An invalid Custom pass throws
    // under the Throw collision policy (the debug default — surface a bad handle immediately) and is
    // skipped-with-warning under WarnAndResolve (keep a shipped game up). Shared by the per-layer +
    // frame-level realizations below.
    auto effectRenderable = [&](const ScreenSpaceEffect& effect) -> bool {
        if (!effectUsesCustomShader(effect)) return true;
        if (customStagePassValid(effect, customReplace_.size())) return true;
        if (collisionPolicy_ == LayerKeyCollisionPolicy::Throw) {
            throw std::invalid_argument(
                "renderFrame: invalid custom shader stage pass (handle out of range)");
        }
        SDL_Log("retropp: skipping invalid custom shader stage pass (handle out of range)");
        return false;
    };

    // Run one effect pass: read `source`, write `dest`. `blend` picks the replace pipeline (the opaque
    // accumulator displace for a Below effect / the frame-level chain) or the premultiplied-over
    // composite pipeline (an isolated Layer's effected image back onto target_). The pass is shader-
    // agnostic: a built-in effect dispatches BY KIND to its pipeline pair + resolved uniform —
    // RowDisplacement → displace_/displaceBlend_ + DisplaceParams (`blankTransparent` controlling the
    // Blank-edge colour), Ripple → ripple_/rippleBlend_ + RippleParams, ColorFill → colorFill_/
    // colorFillBlend_ + ColorFillParams; a Custom effect binds the registered pipeline pair + pushes the
    // game's own uniform bytes. Same scope/compositing/ping-pong plumbing for every kind.
    auto runEffect = [&](SDL_GPUTexture* dest, SDL_GPUTexture* source, const ScreenSpaceEffect& effect,
                         bool blankTransparent, bool blend, SDL_GPULoadOp loadOp,
                         const SDL_Rect* scissor = nullptr) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        // Region-confined effect: shade only the shape's bounding box. The region gate that follows discards
        // everything outside the shape, so the effect result matters only inside — byte-identical output, far
        // fewer shaded pixels. nullptr (every whole-frame caller) leaves the pass at full frame.
        if (scissor) SDL_SetGPUScissor(pass, scissor);

        const SDL_GPUTextureSamplerBinding binding{source, sampler_};  // nearest, CLAMP_TO_EDGE
        if (effectUsesCustomShader(effect)) {
            const auto id = static_cast<std::size_t>(effect.customShader);
            SDL_BindGPUGraphicsPipeline(pass, blend ? customBlend_[id] : customReplace_[id]);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            // The row-data store — always bound (the pipeline declares one fragment storage texture; the
            // 1×1 default covers an effect with no table). This effect's table location rides the engine
            // cbuffer so paramRow / paramRowAtUv read the right rows.
            SDL_BindGPUFragmentStorageTextures(pass, 0, &rowDataStore_, 1);
            const auto locIt = rowTableLocs.find(&effect);
            const RowTableLoc loc = locIt != rowTableLocs.end() ? locIt->second : RowTableLoc{};
            // Slot 0 — the engine cbuffer: the edge mode sampleSource() obeys, from the effect's `edge`
            // (Blank ⇒ transparent outside the frame, the default; Stretch ⇒ clamp; the layer decides it),
            // this effect's row-table location (storeY, rows; rows == 0 ⇒ no table), and the evaluation
            // grid (snap + viewport dims) the generated wrapper snaps by.
            const EngineEffectFragUniforms eng{
                effect.edge == DisplacementEdge::Stretch ? 1u : 0u, loc.storeY, loc.rows,
                snap ? 1u : 0u,
                static_cast<float>(viewport_.width), static_cast<float>(viewport_.height),
                0.0f, 0.0f};
            SDL_PushGPUFragmentUniformData(cmd, 0, &eng, sizeof(eng));
            // Slot 1 — the shader's OWN cbuffer, filled by its generated packer from the effect's inline
            // param fields (custom_effect_packers.h). A parameterless shader (null packer) pushes nothing.
            const EffectPacker packer = id < customPackers_.size() ? customPackers_[id] : nullptr;
            if (packer) {
                std::byte ubuf[kMaxCustomEffectUniformBytes];
                const std::uint32_t usize = packer(effect, ubuf);
                if (usize > 0) SDL_PushGPUFragmentUniformData(cmd, 1, ubuf, usize);
            }
        } else if (effect.kind == ScreenSpaceEffectKind::Ripple) {
            const RippleParams p = rippleParams(effect, PixelSize{viewport_.width, viewport_.height});
            const RippleFragUniforms ru{p.centerU, p.centerV, p.amplitude, p.frequency,
                                        p.phase, p.invViewportW, p.invViewportH, p.decay,
                                        snapF, 0.0f, 0.0f, 0.0f};
            SDL_BindGPUGraphicsPipeline(pass, blend ? rippleBlend_ : ripple_);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &ru, sizeof(ru));
        } else if (effect.kind == ScreenSpaceEffectKind::ColorFill) {
            const ColorFillParams p = colorFillParams(effect);
            const ColorFillFragUniforms cu{p.r, p.g, p.b, 0.0f};
            SDL_BindGPUGraphicsPipeline(pass, blend ? colorFillBlend_ : colorFill_);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
        } else if (effect.kind == ScreenSpaceEffectKind::Gleam) {
            const GleamParams p = gleamParams(effect);
            const GleamFragUniforms gu{p.sweep, p.width, p.gain, p.slant};
            SDL_BindGPUGraphicsPipeline(pass, blend ? gleamBlend_ : gleam_);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &gu, sizeof(gu));
        } else {
            const DisplaceParams p =
                displaceParams(effect, PixelSize{viewport_.width, viewport_.height}, blankTransparent);
            const DisplaceFragUniforms du{p.amplitude, p.frequency, p.phase, p.axis,
                                          p.invViewportW, p.invViewportH, p.edge, p.blankTransparent,
                                          snapF, 0.0f, 0.0f, 0.0f};
            SDL_BindGPUGraphicsPipeline(pass, blend ? displaceBlend_ : displace_);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &du, sizeof(du));
        }
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle
        SDL_EndGPURenderPass(pass);
    };

    // The region gate: read `eff` (the full-frame effect result, t0) + `source` (the
    // original, t1) and write `inside(region) ? eff : src`. `blend` picks regionSelectBlend_ (the
    // premultiplied-over Layer-scope composite onto target_) vs regionSelect_ (replace, for frame-level
    // + Below). Only invoked when region.hasRegion(); the eff buffer is produced by a prior runEffect.
    // No effect shader is touched — the gate is uniform across built-in and Custom effect kinds.
    auto runRegionSelect = [&](SDL_GPUTexture* dest, SDL_GPUTexture* eff, SDL_GPUTexture* source,
                               const ShapePoints& region, float alpha, BlendMode mode, bool blend,
                               SDL_GPULoadOp loadOp) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);

        const SDL_GPUTextureSamplerBinding binds[2] = {{eff, sampler_}, {source, sampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, binds, 2);

        // The boundary's evaluation path (regionCurvePath): an analytic linear+quadratic curve takes the curve
        // pipelines + cbuffer (exact, no facets); a cubic / arbitrary curve WITH a baked mask samples that mask
        // texture (t2, bilinear); a cubic curve with no mask, or a curve-free region, takes the polygon
        // pipelines (the latter byte-identical to the shipped path). `mode` is the owning Region's blend mode.
        switch (regionCurvePath(region)) {
            case CurveRegionPath::Analytic: {
                const CurveRegionSelectFragUniforms cu = makeCurveRegionUniforms(region, viewport_, alpha, mode, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionSelectCurveBlend_ : regionSelectCurve_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
                break;
            }
            case CurveRegionPath::Mask: {
                const CurveMaskEntry& m = curveMasks_[static_cast<std::uint32_t>(region.curveMask) - 1];
                const SDL_GPUTextureSamplerBinding maskBind{m.texture, bilinear_};  // linear, CLAMP_TO_EDGE
                SDL_BindGPUFragmentSamplers(pass, 2, &maskBind, 1);
                const CurveMaskSelectFragUniforms cu =
                    makeCurveMaskSelectUniforms(region, m.bakeMin, m.bakeExtent, viewport_, alpha, mode, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionSelectCurveMaskBlend_ : regionSelectCurveMask_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
                break;
            }
            default: {  // Polygon or SampledPolygon (a cubic curve with no mask is sampled to a faceted polygon)
                const RegionSelectFragUniforms ru = region.curve.empty()
                    ? makeRegionUniforms(region, viewport_, alpha, mode, snapF)
                    : makeRegionUniforms(sampleCurveRegionToPolygon(region), viewport_, alpha, mode, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionSelectBlend_ : regionSelect_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &ru, sizeof(ru));
                break;
            }
        }
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // The blend composite: read the accumulator `dst` (t0) + a container's isolated render `src` (t1) and
    // write applyBlendMode(dst, src, mode) into `dest` (a REPLACE pass — the full blended RGBA, which the
    // caller swaps into the accumulator). The programmable peer of the fixed-function premultiplied-over
    // composite, used where a container's blend mode is not Normal. Mirrors retropp::applyBlendMode.
    auto runBlendComposite = [&](SDL_GPUTexture* dest, SDL_GPUTexture* dst, SDL_GPUTexture* src,
                                 BlendMode mode, SDL_GPULoadOp loadOp) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);

        const SDL_GPUTextureSamplerBinding binds[2] = {{dst, sampler_}, {src, sampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, binds, 2);
        const float bu[4] = {static_cast<float>(mode), 0.0f, 0.0f, 0.0f};  // BlendUniforms: x = mode
        SDL_BindGPUGraphicsPipeline(pass, blend_);
        SDL_PushGPUFragmentUniformData(cmd, 0, bu, sizeof(bu));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // The stencil pass: read ONE `source` (t0) and write `source × survival` — erasing the source's own
    // pixels in/around `region` per `mode`/`feather` (TransparentInside punches a hole, TransparentOutside keeps the
    // inside). `blend` picks regionStencilBlend_ (premultiplied-over composite onto target_, Layer scope)
    // vs regionStencil_ (replace, frame-level + Below). An analytic curve boundary takes the curve
    // pipelines + cbuffer; a curve-free region OR a cubic boundary (sampled to a polygon here) takes the
    // polygon pipelines. A single-texture pass distinct from runRegionSelect (which selects between two
    // textures) — Stencil produces transparency, so there is no effect result to gate.
    auto runStencil = [&](SDL_GPUTexture* dest, SDL_GPUTexture* source, const ShapePoints& region,
                          StencilMode mode, float feather, bool blend, SDL_GPULoadOp loadOp) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);

        const SDL_GPUTextureSamplerBinding binding{source, sampler_};  // nearest, CLAMP_TO_EDGE
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

        switch (regionCurvePath(region)) {
            case CurveRegionPath::Analytic: {
                const CurveStencilFragUniforms cu = makeCurveStencilUniforms(region, mode, feather, viewport_, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionStencilCurveBlend_ : regionStencilCurve_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
                break;
            }
            case CurveRegionPath::Mask: {
                const CurveMaskEntry& m = curveMasks_[static_cast<std::uint32_t>(region.curveMask) - 1];
                const SDL_GPUTextureSamplerBinding maskBind{m.texture, bilinear_};  // linear, CLAMP_TO_EDGE
                SDL_BindGPUFragmentSamplers(pass, 1, &maskBind, 1);
                const CurveMaskStencilFragUniforms cu =
                    makeCurveMaskStencilUniforms(region, m.bakeMin, m.bakeExtent, mode, feather, viewport_, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionStencilCurveMaskBlend_ : regionStencilCurveMask_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
                break;
            }
            default: {  // Polygon or SampledPolygon (a cubic curve with no mask is sampled to a faceted polygon)
                const StencilFragUniforms su = region.curve.empty()
                    ? makeStencilUniforms(region, mode, feather, viewport_, snapF)
                    : makeStencilUniforms(sampleCurveRegionToPolygon(region), mode, feather, viewport_, snapF);
                SDL_BindGPUGraphicsPipeline(pass, blend ? regionStencilBlend_ : regionStencil_);
                SDL_PushGPUFragmentUniformData(cmd, 0, &su, sizeof(su));
                break;
            }
        }
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // One confined-effect application: an effect plus the shape it is confined to. `confined == false`
    // is the whole-reach case (a DrawLayer::effects-chain effect / FrameDrawState::postEffects — no shape).
    // A Region's effects become confined steps (shape = the region's shape). The shape is held by value so
    // an inverted() temporary outlives the step.
    struct ConfinedStep {
        const ScreenSpaceEffect* eff;
        bool                     confined;
        ShapePoints              shape;
        float                    alpha;     // the owning Region's alpha (opacity of its effects); 1 = full
        BlendMode                blend;     // the owning Region's blend mode (Normal for a whole-reach effect)
    };

    // One batched additive run, resolved for issue: the pipeline stage, the pooled instance-buffer slot
    // holding its per-region records, the record count, the packed shader-uniform bytes (all steps in a
    // run share them), and the CPU records (uploaded in the second copy pass). Built in the pre-pass.
    struct BatchRun {
        std::uint32_t               stage = 0;
        int                         slot  = 0;
        int                         count = 0;
        std::vector<std::byte>      params;
        std::vector<GpuRegionBatch> records;
    };
    // A site's batching plan: the run grouping (stepRun disposition parallel to the site's step list) and
    // the resolved runs (parallel to grouping.runs). At step i, stepRun[i] < 0 ⇒ the existing per-step
    // path; else the step belongs to a run, issued once at the run's first step.
    struct SiteBatch {
        RegionBatchGrouping   grouping;
        std::vector<BatchRun> runs;
    };

    // One resolved gather run: the pipeline stage, its edge mode (for the engine cbuffer — a run's
    // regions are same-stage confined effects, expected to share an edge; the first step's is used), the
    // pooled buffer slot holding the concatenated per-region records, the region count (the gather entry
    // point's loop bound / RetroppGatherInfo), and the record bytes (uploaded in the copy pass). Built in the
    // pre-pass. Named distinctly from postprocess::GatherRun (the grouping's run) — this is the GPU-side peer.
    struct GatherRunGpu {
        std::uint32_t          stage = 0;
        std::uint32_t          edge  = 0;   // ScreenSpaceEffect::edge as a uint (0 = Blank, 1 = Stretch)
        int                    slot  = 0;
        int                    count = 0;   // per-region record count == uRegionCount
        std::vector<std::byte> bytes;       // count records concatenated (48-byte header + padded params each)
    };
    // A site's gather plan: the run grouping (stepRun parallel to the site's step list) + the resolved runs.
    // Disjoint from SiteBatch by stage class — a step belongs to at most one of the two dispositions.
    struct SiteGather {
        GatherGrouping            grouping;
        std::vector<GatherRunGpu> runs;
    };

    // Append the confined step for ONE effect. Every effect is region-agnostic: it confines to `defaultShape`
    // (the owning Region's shape) when it has one, else it is whole-reach. `regionAlpha` is the owning
    // Region's opacity (1 for a whole-reach effect). A Transparency is no exception — its shape comes from
    // its Region just like a colour effect's. None / invalid-Custom effects are dropped.
    auto appendEffectSteps = [&](std::vector<ConfinedStep>& steps, const ScreenSpaceEffect& e,
                                 bool hasDefaultShape, const ShapePoints& defaultShape, float regionAlpha,
                                 BlendMode regionBlend) {
        if (e.kind == ScreenSpaceEffectKind::None || !effectRenderable(e)) return;
        if (hasDefaultShape) {
            // A region with no shape is a WHOLE-VIEWPORT region: synthesize a viewport-covering rectangle so
            // it flows through the same gate as a shaped region and honours its blend + alpha across the whole
            // frame (a shaped region keeps its own shape). Inflated a few px so every viewport pixel is
            // strictly inside the gate. This is what makes a whole-frame colour grade / flash / fade — a
            // ColorFill region with no shape — composite correctly.
            ShapePoints shape = defaultShape.hasRegion()
                ? defaultShape
                : ShapePoints::rectangle(Point{-4.0f, -4.0f},
                                         static_cast<float>(viewport_.width) + 8.0f,
                                         static_cast<float>(viewport_.height) + 8.0f);
            steps.push_back({&e, true, std::move(shape), regionAlpha, regionBlend});
        } else {
            steps.push_back({&e, false, {}, 1.0f, BlendMode::Normal});  // whole-reach (no shape; uses frame/layer blend)
        }
    };

    // A layer's confined-step list: each whole-reach effect in the layer's effects chain (in order), then
    // each region's effects (confined to that region's shape, at that region's alpha). The per-layer loop
    // partitions the result by scope (Layer vs Below) — so a Below-scope step (e.g. a transparency side
    // effect) runs on the accumulator after the layer composites, reaching the layers showing through.
    auto buildSteps = [&](const DrawLayer& layer) {
        std::vector<ConfinedStep> steps;
        for (const ScreenSpaceEffect& e : layer.effects)
            appendEffectSteps(steps, e, /*hasDefaultShape=*/false, {}, 1.0f, BlendMode::Normal);
        for (const Region& region : layer.regions)
            for (const ScreenSpaceEffect& e : region.effects)
                appendEffectSteps(steps, e, /*hasDefaultShape=*/true, region.shape, region.alpha, region.blend);
        return steps;
    };

    // ── Region batching (the instanced-additive fast path) ─────────────────────────────────────
    //
    // A running count of per-run instance-buffer slots claimed this frame (buildBatchPlan assigns one per
    // run; the second copy pass uploads each run's records into batchInstanceBufs_[slot]).
    int batchSlotCount = 0;

    // Pack a shape's covering quad + spine + radius into the GPU record (px→uv for the box, viewport-px
    // spine/radius). The single px→uv authority is regionBatchInstance's box == regionScissorRect.
    auto toGpuRegionBatch = [&](const RegionBatchInstance& in) {
        const float cw = static_cast<float>(composeW_ > 0 ? composeW_ : 1);
        const float ch = static_cast<float>(composeH_ > 0 ? composeH_ : 1);
        GpuRegionBatch g{};
        g.uvBox[0] = static_cast<float>(in.box.x) / cw;
        g.uvBox[1] = static_cast<float>(in.box.y) / ch;
        g.uvBox[2] = static_cast<float>(in.box.x + in.box.width)  / cw;
        g.uvBox[3] = static_cast<float>(in.box.y + in.box.height) / ch;
        g.spine[0] = in.p0.x; g.spine[1] = in.p0.y; g.spine[2] = in.p1.x; g.spine[3] = in.p1.y;
        g.radiusPad[0] = in.radius;
        return g;
    };

    // Resolve a site's confined steps into a SiteBatch: per-step batch keys (the postprocess eligibility
    // predicate AND a batched pipeline for the stage), grouped by (stage, packed params) within contiguous
    // eligible stretches (postprocess::groupRegionBatches), then each ≥2 group turned into a BatchRun with
    // its instance records + a claimed buffer slot. Pure CPU; records upload in the second copy pass.
    auto buildBatchPlan = [&](const std::vector<ConfinedStep>& steps) -> SiteBatch {
        SiteBatch sb;
        if (steps.empty()) return sb;
        std::vector<std::array<std::byte, kMaxCustomEffectUniformBytes>> paramBufs(steps.size());
        std::vector<std::uint32_t> paramLens(steps.size(), 0);
        std::vector<BatchStep>     keys(steps.size());
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const ConfinedStep& s = steps[i];
            if (!s.confined || !s.shape.hasRegion()) continue;
            if (!regionBatchEligible(*s.eff, s.shape, s.alpha, s.blend)) continue;
            const auto id = static_cast<std::size_t>(s.eff->customShader);
            if (id >= customBatched_.size() || customBatched_[id] == nullptr) continue;  // stage not additive
            keys[i].eligible = true;
            keys[i].stage    = static_cast<std::uint32_t>(id);
            const EffectPacker packer = id < customPackers_.size() ? customPackers_[id] : nullptr;
            if (packer) paramLens[i] = packer(*s.eff, paramBufs[i].data());
            keys[i].params = std::span<const std::byte>(paramBufs[i].data(), paramLens[i]);
        }
        sb.grouping = groupRegionBatches(keys);
        sb.runs.reserve(sb.grouping.runs.size());
        for (const RegionBatchRun& run : sb.grouping.runs) {
            BatchRun br;
            br.stage = run.stage;
            br.slot  = batchSlotCount++;
            br.count = static_cast<int>(run.steps.size());
            const std::uint32_t first = run.steps.front();  // all steps share (stage, params) by construction
            br.params.assign(paramBufs[first].begin(), paramBufs[first].begin() + paramLens[first]);
            br.records.reserve(run.steps.size());
            for (const std::uint32_t si : run.steps)
                br.records.push_back(toGpuRegionBatch(
                    regionBatchInstance(steps[si].shape, composeScale_, composeW_, composeH_)));
            sb.runs.push_back(std::move(br));
        }
        return sb;
    };

    // Resolve a site's confined steps into a SiteGather: per-step gather keys (the gatherEligible
    // predicate AND a gather pipeline for the stage), grouped into maximal contiguous same-stage stretches
    // (postprocess::groupGatherRuns), then each ≥2 run turned into a GatherRunGpu with its concatenated
    // per-region records (each = the 48-byte header + the step's OWN packed params — per-region params are
    // the point, so they ride the records) + a claimed buffer slot. Pure CPU; records upload in the copy pass.
    auto buildGatherPlan = [&](const std::vector<ConfinedStep>& steps) -> SiteGather {
        SiteGather sg;
        if (steps.empty()) return sg;
        std::vector<std::array<std::byte, kMaxCustomEffectUniformBytes>> paramBufs(steps.size());
        std::vector<std::uint32_t> paramLens(steps.size(), 0);
        std::vector<GatherStep>    keys(steps.size());
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const ConfinedStep& s = steps[i];
            if (!s.confined || !s.shape.hasRegion()) continue;
            if (!gatherEligible(*s.eff, s.shape, s.alpha, s.blend)) continue;
            const auto id = static_cast<std::size_t>(s.eff->customShader);
            if (id >= customGather_.size() || customGather_[id] == nullptr) continue;  // stage does not gather
            keys[i].eligible = true;
            keys[i].stage    = static_cast<std::uint32_t>(id);
            const EffectPacker packer = id < customPackers_.size() ? customPackers_[id] : nullptr;
            if (packer) paramLens[i] = packer(*s.eff, paramBufs[i].data());
        }
        sg.grouping = groupGatherRuns(keys);
        sg.runs.reserve(sg.grouping.runs.size());
        for (const GatherRun& run : sg.grouping.runs) {
            GatherRunGpu gr;
            gr.stage = run.stage;
            gr.slot  = batchSlotCount++;
            gr.count = static_cast<int>(run.steps.size());
            const std::uint32_t first = run.steps.front();
            gr.edge = static_cast<std::uint32_t>(steps[first].eff->edge);
            for (const std::uint32_t si : run.steps) {
                const RegionBatchInstance in =
                    regionBatchInstance(steps[si].shape, composeScale_, composeW_, composeH_);
                const std::vector<std::byte> rec = gatherRecordBytes(
                    in, composeW_, composeH_,
                    std::span<const std::byte>(paramBufs[si].data(), paramLens[si]));
                gr.bytes.insert(gr.bytes.end(), rec.begin(), rec.end());
            }
            sg.runs.push_back(std::move(gr));
        }
        return sg;
    };

    // Issue ONE batched additive pass: bind the stage's batched pipeline, the run's instance buffer (vertex
    // storage t0 space0), the zero source + row-data store (fragment t0/t1 space2) + the engine cbuffer
    // (rows 0) and the shader's own params, then draw 6 × count instances onto `dest`. `loadOp` LOADs the
    // destination so the ADDITIVE blend accumulates each region's delta in place. No gate pass, no scissor.
    auto runBatch = [&](SDL_GPUTexture* dest, SDL_GPULoadOp loadOp, const BatchRun& run) {
        if (run.count <= 0 || run.stage >= customBatched_.size() || customBatched_[run.stage] == nullptr) return;
        SDL_GPUBuffer* buf = run.slot < static_cast<int>(batchInstanceBufs_.size())
                                 ? batchInstanceBufs_[run.slot] : nullptr;
        if (!buf) return;
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, customBatched_[run.stage]);
        SDL_BindGPUVertexStorageBuffers(pass, 0, &buf, 1);
        const SDL_GPUTextureSamplerBinding binding{batchZeroSource_, sampler_};  // zero source ⇒ delta only
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_BindGPUFragmentStorageTextures(pass, 0, &rowDataStore_, 1);  // bound but unread (rows = 0)
        // The engine cbuffer: no row table (rows 0; eligibility forbids a paramTable), the evaluation grid
        // + viewport dims the batched entry point's gate + snap use. Edge is irrelevant — the zero source
        // samples 0 in and out of bounds.
        const EngineEffectFragUniforms eng{
            0u, 0u, 0u, snap ? 1u : 0u,
            static_cast<float>(viewport_.width), static_cast<float>(viewport_.height), 0.0f, 0.0f};
        SDL_PushGPUFragmentUniformData(cmd, 0, &eng, sizeof(eng));
        if (!run.params.empty())
            SDL_PushGPUFragmentUniformData(cmd, 1, run.params.data(),
                                           static_cast<Uint32>(run.params.size()));
        SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(run.count), 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // Issue ONE gather pass: a fullscreen triangle that reads `source` (the previous image, bound as
    // the REAL SourceTexture — unlike the additive zero-source pass) + the run's per-region records (fragment storage
    // buffer t0) and writes the union-shape result to `dest`. Per fragment the generated entry tests every
    // record's gate (uv-bbox reject → n≤2 SDF, LAST region wins) and either passes the source through
    // (outside every shape, byte-exact) or evaluates the shader with the winning region's params loaded.
    // `blend`: false = replace (frame-level / Below / mid-chain); true = premultiplied-over composite onto
    // target_ (a Normal layer's last step), with `loadOp` LOADing the accumulator beneath.
    auto runGather = [&](SDL_GPUTexture* dest, SDL_GPUTexture* source, const GatherRunGpu& run, bool blend,
                         SDL_GPULoadOp loadOp) {
        if (run.count <= 0 || run.stage >= customGather_.size() || customGather_[run.stage] == nullptr) return;
        SDL_GPUGraphicsPipeline* pipe = blend ? customGatherBlend_[run.stage] : customGather_[run.stage];
        if (!pipe) return;
        SDL_GPUBuffer* buf = run.slot < static_cast<int>(batchInstanceBufs_.size())
                                 ? batchInstanceBufs_[run.slot] : nullptr;
        if (!buf) return;
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, pipe);
        const SDL_GPUTextureSamplerBinding binding{source, sampler_};  // the real previous image (nearest CLAMP)
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_BindGPUFragmentStorageTextures(pass, 0, &rowDataStore_, 1);  // bound but unread (no paramTable)
        SDL_BindGPUFragmentStorageBuffers(pass, 0, &buf, 1);            // the run's per-region records (t2 space2)
        // The engine cbuffer (b0): the run's edge for sampleSource, no row table (eligibility forbids one),
        // the evaluation grid + viewport dims the entry point's gate + snap use.
        const EngineEffectFragUniforms eng{
            run.edge, 0u, 0u, snap ? 1u : 0u,
            static_cast<float>(viewport_.width), static_cast<float>(viewport_.height), 0.0f, 0.0f};
        SDL_PushGPUFragmentUniformData(cmd, 0, &eng, sizeof(eng));
        const GpuGatherInfo info{static_cast<std::uint32_t>(run.count), 0u, 0u, 0u};  // b1 = region count
        SDL_PushGPUFragmentUniformData(cmd, 1, &info, sizeof(info));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle
        SDL_EndGPURenderPass(pass);
    };

    // Apply one Below-scope confined step to the WHOLE accumulator (target_): transform it (or make it
    // see-through) confined to the step's shape into layerScratch_, then swap it in — this layer's content
    // (already composited
    // into target_) AND everything beneath it. The single-step case is the plain whole-layer Below composite.
    auto applyBelowStep = [&](const ConfinedStep& s) {
        if (s.eff->kind == ScreenSpaceEffectKind::Transparency) {
            runStencil(layerScratch_, target_, s.shape, s.eff->stencil, s.eff->feather,
                       /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
        } else if (s.confined && s.shape.hasRegion()) {
            const IntRect  r = regionScissorRect(s.shape, composeScale_, composeW_, composeH_);
            const SDL_Rect sc{r.x, r.y, r.width, r.height};
            runEffect(post0_, target_, *s.eff, /*blankTransparent=*/false, /*blend=*/false,
                      SDL_GPU_LOADOP_DONT_CARE, &sc);
            runRegionSelect(layerScratch_, post0_, target_, s.shape, s.alpha, s.blend, /*blend=*/false,
                            SDL_GPU_LOADOP_DONT_CARE);
        } else {
            runEffect(layerScratch_, target_, *s.eff, /*blankTransparent=*/false, /*blend=*/false,
                      SDL_GPU_LOADOP_DONT_CARE);
        }
        std::swap(target_, layerScratch_);
    };

    // Apply an isolated layer's confined-step chain on its own premultiplied scratch (starting at
    // layerScratch_, where the layer content was rendered): each step replaces into a ping-pong scratch
    // (so step n+1 sees step n's output). When the layer's blend is Normal, the LAST step composites
    // premultiplied-over target_ (the byte-identical alpha-over path). When the layer's blend is NOT
    // Normal, every step replaces into a scratch and the finished isolated image is composited onto the
    // accumulator with `layerBlend` (the programmable blend composite) — the accumulator must already hold
    // the layers beneath (the caller initializes target_ first). A single step is the plain per-layer composite.
    auto applyLayerChain = [&](const std::vector<ConfinedStep>& steps, const SiteBatch& batch,
                               const SiteGather& gather, BlendMode layerBlend, SDL_GPULoadOp compositeLoad) {
        const bool blendComposite = (layerBlend != BlendMode::Normal);
        SDL_GPUTexture* pool[3] = {layerScratch_, post0_, post1_};
        auto other = [&](SDL_GPUTexture* a, SDL_GPUTexture* b) -> SDL_GPUTexture* {
            for (SDL_GPUTexture* t : pool)
                if (t != a && t != b) return t;
            return pool[0];
        };
        const std::size_t n = steps.size();
        // A Normal layer's LAST step composites premultiplied-over target_ — but ONLY when that last step is
        // a solo (per-region) step OR a GATHER run (both write a fresh image, so the blend pipeline can
        // composite it straight onto target_). When the last step belongs to a additive batched run (which
        // blends in place onto `cur`, never to target_), no per-step composite runs, so a final
        // premultiplied-over composite of `cur` follows the loop (see below). `lastIsAdditiveRun` — the
        // additive grouping's stepRun[n-1] ≥ 0 — is that one case; every other last step composites in-loop.
        const bool lastIsAdditiveRun = (n > 0) && (batch.grouping.stepRun[n - 1] >= 0);
        SDL_GPUTexture* cur = layerScratch_;
        for (std::size_t i = 0; i < n; ++i) {
            const int rn = batch.grouping.stepRun[i];
            if (rn >= 0) {  // additive batched run: additive in place onto cur (no ping-pong advance), once per run
                if (i == batch.grouping.runs[rn].steps.front())
                    runBatch(cur, SDL_GPU_LOADOP_LOAD, batch.runs[rn]);
                continue;
            }
            const int rnG = gather.grouping.stepRun[i];
            if (rnG >= 0) {  // gather run: a replace step (reads cur, writes a fresh image), once per run
                const std::vector<std::uint32_t>& runSteps = gather.grouping.runs[rnG].steps;
                if (i == runSteps.front()) {
                    // The run occupies its member positions; it is the layer's last content iff its LAST
                    // member is the last step. If so (and Normal), it composites premultiplied-over target_.
                    const bool          runIsLast = (runSteps.back() == n - 1);
                    const bool          toTarget  = runIsLast && !blendComposite;
                    const SDL_GPULoadOp lop       = toTarget ? compositeLoad : SDL_GPU_LOADOP_DONT_CARE;
                    SDL_GPUTexture*     dest       = toTarget ? target_ : other(cur, cur);
                    runGather(dest, cur, gather.runs[rnG], /*blend=*/toTarget, lop);
                    if (!toTarget) cur = dest;
                }
                continue;
            }
            const ConfinedStep& s    = steps[i];
            const bool          last = (i + 1 == n);
            // Composite-over target_ only on the last step of a Normal layer (a solo step; a last gather run
            // is handled in its own branch above, a last additive run in the post-loop composite below).
            const bool          toTarget = last && !lastIsAdditiveRun && !blendComposite;
            const SDL_GPULoadOp lop      = toTarget ? compositeLoad : SDL_GPU_LOADOP_DONT_CARE;
            if (s.eff->kind == ScreenSpaceEffectKind::Transparency) {
                SDL_GPUTexture* dest = toTarget ? target_ : other(cur, cur);
                runStencil(dest, cur, s.shape, s.eff->stencil, s.eff->feather, /*blend=*/toTarget, lop);
                if (!toTarget) cur = dest;
            } else if (s.confined && s.shape.hasRegion()) {
                SDL_GPUTexture* tmp  = other(cur, cur);
                SDL_GPUTexture* dest = toTarget ? target_ : other(cur, tmp);
                const IntRect  r = regionScissorRect(s.shape, composeScale_, composeW_, composeH_);
                const SDL_Rect sc{r.x, r.y, r.width, r.height};
                runEffect(tmp, cur, *s.eff, /*blankTransparent=*/true, /*blend=*/false,
                          SDL_GPU_LOADOP_DONT_CARE, &sc);
                runRegionSelect(dest, tmp, cur, s.shape, s.alpha, s.blend, /*blend=*/toTarget, lop);
                if (!toTarget) cur = dest;
            } else {
                SDL_GPUTexture* dest = toTarget ? target_ : other(cur, cur);
                runEffect(dest, cur, *s.eff, /*blankTransparent=*/true, /*blend=*/toTarget, lop);
                if (!toTarget) cur = dest;
            }
        }
        if (blendComposite) {
            // `cur` holds the layer's finished isolated image; composite it onto the accumulator with the mode.
            SDL_GPUTexture* dest = other(cur, target_);  // a free scratch (target_ is not in the pool)
            runBlendComposite(dest, target_, cur, layerBlend, SDL_GPU_LOADOP_DONT_CARE);
            std::swap(target_, dest);
        } else if (lastIsAdditiveRun) {
            // Normal layer whose LAST step is a additive batched run: nothing composited `cur` to target_, so do
            // it now. The isolated content is PREMULTIPLIED, so this must be the premultiplied-over composite
            // (ONE / ONE_MINUS_SRC_ALPHA), NOT runBlendComposite's straight alpha-over (which would double-
            // darken translucent edges). An empty-region gate (count 0 ⇒ pass-through) on the blend pipeline
            // IS that premultiplied-over composite. A gather last step does NOT reach here — it
            // composited onto target_ within the loop (its blend branch). One O(1) pass, run-path-only.
            runRegionSelect(target_, cur, cur, ShapePoints{}, 1.0f, BlendMode::Normal, /*blend=*/true,
                            compositeLoad);
        }
    };

    bool               targetInitialized = false;  // has target_ been cleared this frame?
    SDL_GPURenderPass* batch             = nullptr; // open target_ pass batching consecutive direct draws
    auto openBatch = [&]() {
        SDL_GPUColorTargetInfo t{};
        t.texture     = target_;
        t.clear_color = kBackdropClear;
        t.load_op     = targetInitialized ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        batch = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        targetInitialized = true;
    };
    auto closeBatch = [&]() {
        if (batch) { SDL_EndGPURenderPass(batch); batch = nullptr; }
    };
    // Clear target_ to the backdrop if no layer has yet initialized it. A blended layer composite reads the
    // accumulator as a texture, so it must hold the backdrop (the layers beneath) before the blend runs.
    auto ensureTargetInitialized = [&]() {
        if (targetInitialized) return;
        closeBatch();
        SDL_GPUColorTargetInfo t{};
        t.texture     = target_;
        t.clear_color = kBackdropClear;
        t.load_op     = SDL_GPU_LOADOP_CLEAR;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        SDL_EndGPURenderPass(p);
        targetInitialized = true;
    };
    // Render one layer alone into layerScratch_ (transparent-cleared) — the isolated content for a blended
    // or effected layer.
    auto renderLayerIsolated = [&](std::size_t idx) {
        SDL_GPUColorTargetInfo lt{};
        lt.texture     = layerScratch_;
        lt.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};  // transparent
        lt.load_op     = SDL_GPU_LOADOP_CLEAR;
        lt.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* lp = SDL_BeginGPURenderPass(cmd, &lt, 1, nullptr);
        drawLayer(lp, idx);
        SDL_EndGPURenderPass(lp);
    };

    // ── Pre-pass: build every site's confined-step list + batch plan, then upload all runs' instance
    //    records in ONE copy pass — BEFORE any composite render pass begins. The composite loop consumes
    //    the prebuilt lists + plans (instead of calling buildSteps mid-loop), so each run's records are on
    //    the GPU before its batched pass draws them. Each layer partitions into its Layer-scope chain and
    //    its Below-scope steps (two of the three confined sites); the frame chain is the third. ──────────
    struct LayerSitePlan {
        std::vector<ConfinedStep> layerSteps, belowSteps;
        SiteBatch                 layerBatch, belowBatch;    // additive-batching dispositions
        SiteGather                layerGather, belowGather;  // gather dispositions
    };
    std::vector<LayerSitePlan> layerPlans(frame.layers.size());
    for (const std::size_t idx : order) {
        std::vector<ConfinedStep> steps = buildSteps(frame.layers[idx]);
        LayerSitePlan lp;
        for (const ConfinedStep& s : steps)
            (effectIsBelowScope(*s.eff) ? lp.belowSteps : lp.layerSteps).push_back(s);
        lp.layerBatch  = buildBatchPlan(lp.layerSteps);
        lp.belowBatch  = buildBatchPlan(lp.belowSteps);
        lp.layerGather = buildGatherPlan(lp.layerSteps);
        lp.belowGather = buildGatherPlan(lp.belowSteps);
        layerPlans[idx] = std::move(lp);
    }
    // The frame-level steps (whole-frame postEffects, then each frame region's effects) + their batch + gather
    // plans.
    std::vector<ConfinedStep> frameSteps;
    for (const ScreenSpaceEffect& e : frame.postEffects)
        appendEffectSteps(frameSteps, e, /*hasDefaultShape=*/false, {}, 1.0f, BlendMode::Normal);
    for (const Region& region : frame.regions)
        for (const ScreenSpaceEffect& e : region.effects)
            appendEffectSteps(frameSteps, e, /*hasDefaultShape=*/true, region.shape, region.alpha, region.blend);
    SiteBatch  frameBatch  = buildBatchPlan(frameSteps);
    SiteGather frameGather = buildGatherPlan(frameSteps);

    if (batchSlotCount > 0) {
        if (static_cast<int>(batchInstanceBufs_.size()) < batchSlotCount) {
            batchInstanceBufs_.resize(static_cast<std::size_t>(batchSlotCount), nullptr);
            batchInstanceCaps_.resize(static_cast<std::size_t>(batchSlotCount), 0);
        }
        SDL_GPUCopyPass* bcopy = nullptr;
        // Generalized to byte blobs so additive instance records (GpuRegionBatch[]) and gather records
        // (48-byte header + padded params, variable stride) travel one path and share the pool
        // (batchInstanceBufs_ / batchInstanceCaps_ track BYTE capacity). Grows the slot's storage buffer
        // on demand, uploads the blob, and stages the transfer buffer in `scratch` for the frame.
        auto uploadBlob = [&](int slot, const std::byte* data, std::size_t bytesLen) {
            if (bytesLen == 0) return;
            const int need = static_cast<int>(bytesLen);
            SDL_GPUBuffer*& buf = batchInstanceBufs_[static_cast<std::size_t>(slot)];
            if (!buf || batchInstanceCaps_[static_cast<std::size_t>(slot)] < need) {  // grow-on-demand
                if (buf) SDL_ReleaseGPUBuffer(device_, buf);
                SDL_GPUBufferCreateInfo bi{};
                bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
                bi.size  = static_cast<Uint32>(need);
                buf = SDL_CreateGPUBuffer(device_, &bi);
                if (!buf) fail("SDL_CreateGPUBuffer (region records) failed");
                batchInstanceCaps_[static_cast<std::size_t>(slot)] = need;
            }
            const Uint32 bytes = static_cast<Uint32>(need);
            SDL_GPUTransferBufferCreateInfo tb{};
            tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            tb.size  = bytes;
            SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tb);
            if (!transfer) fail("SDL_CreateGPUTransferBuffer (region records) failed");
            void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
            if (!mapped) fail("SDL_MapGPUTransferBuffer (region records) failed");
            std::memcpy(mapped, data, bytes);
            SDL_UnmapGPUTransferBuffer(device_, transfer);
            if (!bcopy) bcopy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBufferLocation src{};
            src.transfer_buffer = transfer;
            src.offset          = 0;
            SDL_GPUBufferRegion dst{};
            dst.buffer = buf;
            dst.offset = 0;
            dst.size   = bytes;
            SDL_UploadToGPUBuffer(bcopy, &src, &dst, false);
            scratch.push_back(transfer);
        };
        auto uploadBatchRun = [&](const BatchRun& run) {
            uploadBlob(run.slot, reinterpret_cast<const std::byte*>(run.records.data()),
                       run.records.size() * sizeof(GpuRegionBatch));
        };
        auto uploadGatherRun = [&](const GatherRunGpu& run) {
            uploadBlob(run.slot, run.bytes.data(), run.bytes.size());
        };
        for (const std::size_t idx : order) {
            for (const BatchRun&     r : layerPlans[idx].layerBatch.runs)   uploadBatchRun(r);
            for (const BatchRun&     r : layerPlans[idx].belowBatch.runs)   uploadBatchRun(r);
            for (const GatherRunGpu& r : layerPlans[idx].layerGather.runs)  uploadGatherRun(r);
            for (const GatherRunGpu& r : layerPlans[idx].belowGather.runs)  uploadGatherRun(r);
        }
        for (const BatchRun&     r : frameBatch.runs)  uploadBatchRun(r);
        for (const GatherRunGpu& r : frameGather.runs) uploadGatherRun(r);
        if (bcopy) SDL_EndGPUCopyPass(bcopy);
    }

    for (const std::size_t idx : order) {
        const DrawLayer&     layer = frame.layers[idx];
        const LayerSitePlan& plan  = layerPlans[idx];

        // Nothing to do beyond the layer's own content. A Normal layer takes the batched faithful path; a
        // non-Normal layer renders isolated and composites onto the accumulator with its blend mode.
        if (plan.layerSteps.empty() && plan.belowSteps.empty()) {
            if (layer.blend == BlendMode::Normal) {
                if (!batch) openBatch();
                drawLayer(batch, idx);
            } else {
                closeBatch();
                ensureTargetInitialized();
                renderLayerIsolated(idx);
                runBlendComposite(post0_, target_, layerScratch_, layer.blend, SDL_GPU_LOADOP_DONT_CARE);
                std::swap(target_, post0_);
            }
            continue;
        }

        // Two-phase scope partition (done in the pre-pass). Layer-scope steps work on the layer's OWN
        // isolated content (composited over the accumulator); Below-scope steps adjust the WHOLE accumulator
        // after the layer composites. Submission order is preserved within each phase; Layer always runs
        // before Below — the only coherent order, since a Below step reads the post-composite accumulator.
        if (!plan.layerSteps.empty()) {
            // Layer (isolated) scope: render this layer alone into layerScratch_ (transparent-cleared), then
            // apply the Layer-scope chain on the layer's own scratch.
            closeBatch();
            renderLayerIsolated(idx);
            if (layer.blend != BlendMode::Normal) ensureTargetInitialized();
            const SDL_GPULoadOp compositeLoad = targetInitialized ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
            targetInitialized = true;
            applyLayerChain(plan.layerSteps, plan.layerBatch, plan.layerGather, layer.blend, compositeLoad);
        } else {
            // No Layer-scope step: composite the layer's own content straight into the accumulator (the
            // batched faithful draw); the Below-scope steps below then adjust the whole accumulator.
            if (!batch) openBatch();
            drawLayer(batch, idx);
            closeBatch();
        }

        // Below-scope phase: each step (or run) adjusts the whole accumulator at this z (this layer's content
        // AND everything beneath it), confined to its region's shape, in submission order. A additive batched
        // run blends its regions' deltas additively ONTO target_ in place (LOAD) at the run's first step; a
        // gather run reads target_ → writes the union-shape result into layerScratch_ (a replace pass —
        // its passthrough covers everything outside the shapes) and swaps it in; the per-step path handles
        // ineligible steps and eligible singletons. Disjoint by stage class, so at most one branch fires per i.
        for (std::size_t i = 0; i < plan.belowSteps.size(); ++i) {
            const int rn  = plan.belowBatch.grouping.stepRun[i];
            const int rnG = plan.belowGather.grouping.stepRun[i];
            if (rn >= 0) {
                if (i == plan.belowBatch.grouping.runs[rn].steps.front())
                    runBatch(target_, SDL_GPU_LOADOP_LOAD, plan.belowBatch.runs[rn]);
            } else if (rnG >= 0) {
                if (i == plan.belowGather.grouping.runs[rnG].steps.front()) {
                    runGather(layerScratch_, target_, plan.belowGather.runs[rnG], /*blend=*/false,
                              SDL_GPU_LOADOP_DONT_CARE);
                    std::swap(target_, layerScratch_);
                }
            } else {
                applyBelowStep(plan.belowSteps[i]);
            }
        }
    }
    closeBatch();

    // Empty frame (no layer ever cleared target_): clear it to the backdrop so the blit shows the
    // backdrop rather than stale contents.
    if (!targetInitialized) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = target_;
        t.clear_color = kBackdropClear;
        t.load_op     = SDL_GPU_LOADOP_CLEAR;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        SDL_EndGPURenderPass(pass);
    }

    // ── Post-process chain: frame-level screen-space effects, run on the finished viewport image
    //    before the blit. Each active effect is one fullscreen-triangle pass that reads the previous
    //    image and writes a scratch target; the two scratch targets strictly alternate by APPLIED-pass
    //    count, so a pass never samples the texture it writes (target_ is never written). Effects
    //    dispatch on kind — a built-in RowDisplacement binds displace_ + resolved params; a Custom
    //    effect binds its registered replace pipeline + pushes the game's uniform, composing in
    //    submission order with the built-ins. An invalid Custom pass is skipped (under WarnAndResolve)
    //    without advancing the ping-pong. An empty chain leaves blitSource == target_ → the blit
    //    samples the composited viewport directly. ──────────────────────────────────────────────────
    // The frame-level steps (frameSteps) + their batch plan (frameBatch) were built in the pre-pass above.
    // Scope plays no role here — frame-level steps are inherently whole-frame, run in submission order on
    // the composited image.
    SDL_GPUTexture* blitSource = target_;
    {
        SDL_GPUTexture* readTex    = target_;
        SDL_GPUTexture* scratch[2] = {post0_, post1_};
        std::size_t     applied    = 0;  // counts only rendered passes → preserves read≠write alternation
        for (std::size_t i = 0; i < frameSteps.size(); ++i) {
            SDL_GPUTexture* writeTex = scratch[applied % 2];

            // A batched run: blend its regions' deltas additively onto the running image. Mid-chain (readTex
            // is a scratch) it blends in place, no ping-pong advance. First in the chain (readTex == target_,
            // which stays unwritten by the compose invariant) it first copies target_ → writeTex via an
            // empty-region pass-through, then blends onto writeTex and advances. Issued once, at the run's
            // first step; the run's other steps are skipped.
            const int rn = frameBatch.grouping.stepRun[i];
            if (rn >= 0) {
                if (i != frameBatch.grouping.runs[rn].steps.front()) continue;
                const BatchRun& run = frameBatch.runs[rn];
                if (readTex == target_) {
                    runRegionSelect(writeTex, target_, target_, ShapePoints{}, 1.0f, BlendMode::Normal,
                                    /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);  // pass-through copy
                    runBatch(writeTex, SDL_GPU_LOADOP_LOAD, run);
                    blitSource = writeTex;
                    readTex    = writeTex;
                    ++applied;
                } else {
                    runBatch(readTex, SDL_GPU_LOADOP_LOAD, run);  // additive in place, no advance
                    blitSource = readTex;
                }
                continue;
            }

            // A gather run: read the running image (readTex) → write the union-shape result to a fresh
            // writeTex (replace — its passthrough covers everything outside the shapes), advancing the
            // ping-pong like any ordinary applied pass. readTex == target_ first-in-chain is fine — the pass
            // only SAMPLES target_, never writes it (the compose invariant holds). Issued once, at the run's
            // first step.
            const int rnG = frameGather.grouping.stepRun[i];
            if (rnG >= 0) {
                if (i != frameGather.grouping.runs[rnG].steps.front()) continue;
                runGather(writeTex, readTex, frameGather.runs[rnG], /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
                blitSource = writeTex;
                readTex    = writeTex;
                ++applied;
                continue;
            }

            const ConfinedStep& s = frameSteps[i];
            // runEffect dispatches the built-in (displace_) vs Custom (customReplace_) pass; blend=false
            // + blankTransparent=false = the frame-level replace. A confined step lands the effect
            // full-frame in layerScratch_ (free during the frame-level chain) and the gate selects
            // inside(shape)?eff:readTex into writeTex. A whole-frame step → the single replace pass. A
            // Transparency makes the composited frame see-through in/around the shape (replace into writeTex)
            // → reveals the backdrop — one applied pass, advancing the ping-pong like any other.
            if (s.eff->kind == ScreenSpaceEffectKind::Transparency) {
                runStencil(writeTex, readTex, s.shape, s.eff->stencil, s.eff->feather,
                           /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            } else if (s.confined && s.shape.hasRegion()) {
                const IntRect  r = regionScissorRect(s.shape, composeScale_, composeW_, composeH_);
                const SDL_Rect sc{r.x, r.y, r.width, r.height};
                runEffect(layerScratch_, readTex, *s.eff,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE, &sc);
                runRegionSelect(writeTex, layerScratch_, readTex, s.shape, s.alpha, s.blend,
                                /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            } else if (frame.blend == BlendMode::Normal) {
                runEffect(writeTex, readTex, *s.eff,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            } else {
                // The frame's whole-frame postEffect output combines over the composited image with the
                // frame's blend mode: render the effect into a scratch (layerScratch_ is free here), then
                // blend-composite it onto the running image.
                runEffect(layerScratch_, readTex, *s.eff,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
                runBlendComposite(writeTex, readTex, layerScratch_, frame.blend, SDL_GPU_LOADOP_DONT_CARE);
            }
            blitSource = writeTex;
            readTex    = writeTex;
            ++applied;
        }
    }

    return blitSource;
}

void Renderer::renderFrame(const FrameDrawState& frame) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return;

    // Automatic interpolation: read the run loop's (alpha, tickAdvanced) from the frame-timing channel. On
    // a committed tick, rotate the per-id mirror to this submission; every frame, composite each object
    // eased between its previous and current tick state by the sub-tick factor. Off (or no loop publishing
    // → the default (0, false)) composites the submission verbatim.
    const FrameDrawState* toCompose = &frame;
    float composeAlpha = 0.0f;
    if (interpolation_) {
        const FrameTiming timing = frameTiming();
        if (timing.tickAdvanced) interp_.reconcile(frame);
        toCompose    = &interp_.interpolate(frame, timing.alpha);
        composeAlpha = timing.alpha;
    }

    // Resolve the compose grid from the window and resize the offscreen targets when it changes. On the
    // interpolation path this composites at the output resolution so sub-pixel motion has a finer grid to
    // land on; off / headless it stays 1 (viewport res, blit upscales) — the faithful, byte-identical path.
    resizeComposeTargets(resolveComposeScale());

    // Compose the finished viewport image (copy pass → layer composite → post-process chain), then blit
    // it to the swapchain. The compose is shared verbatim with captureViewport — one path, no drift.
    std::vector<SDL_GPUTransferBuffer*> scratch;
    SDL_GPUTexture* blitSource = composeViewport(cmd, *toCompose, scratch, composeAlpha, interpolation_);

    // ── Blit pass: viewport → swapchain, integer-scaled + letterboxed. ──────────────────────────
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 width  = 0;
    Uint32 height = 0;
    // On Metal, the BLOCKING acquire busy-waits a core (SDL bug); use the non-blocking acquire there and
    // let the host frame deadline pace. Non-blocking returns true with a null swapchain when the frame
    // isn't ready yet → the existing `swapchain != nullptr` guard skips this frame's blit/present cleanly
    // (paced, so skips are rare). Vulkan/D3D12 keep the blocking acquire (they OS-block, no spin).
    const bool acquired = window_ != nullptr && (acquireNonBlocking_
        ? SDL_AcquireGPUSwapchainTexture(cmd, window_, &swapchain, &width, &height)
        : SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, &width, &height));
    if (acquired && swapchain != nullptr) {
        SDL_GPUColorTargetInfo scTarget{};
        scTarget.texture     = swapchain;
        scTarget.clear_color = kLetterboxClear;
        scTarget.load_op     = SDL_GPU_LOADOP_CLEAR;
        scTarget.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &scTarget, 1, nullptr);

        // The compose image always fills the window at the largest integer scale that fits, centred +
        // letterboxed. Output SIZE is the window's size (Platform owns it, via setWindowSize sized to
        // viewport × the chosen scale); the blit just fills whatever window it's given, crisply. The
        // scale is relative to the compose grid (== the viewport at composeScale_ 1).
        const IntRect dest = integerScaleToFitRect(
            PixelSize{static_cast<int>(width), static_cast<int>(height)},
            PixelSize{composeW_, composeH_});

        const SDL_GPUViewport vp{static_cast<float>(dest.x), static_cast<float>(dest.y),
                                 static_cast<float>(dest.width), static_cast<float>(dest.height),
                                 0.0f, 1.0f};
        SDL_SetGPUViewport(pass, &vp);

        const int sx = std::max(0, dest.x);
        const int sy = std::max(0, dest.y);
        const int sr = std::min(static_cast<int>(width), dest.x + dest.width);
        const int sb = std::min(static_cast<int>(height), dest.y + dest.height);
        const SDL_Rect scissor{sx, sy, std::max(0, sr - sx), std::max(0, sb - sy)};
        SDL_SetGPUScissor(pass, &scissor);

        SDL_BindGPUGraphicsPipeline(pass, blit_);
        // Select the blit sampler by the runtime mode — nearest (crisp) or bilinear (smoothed). Same blit
        // pipeline; only the bound sampler differs (sampler state is pipeline-independent, no shader change).
        SDL_GPUSampler* blitSampler = (sampling_ == SamplingMode::Bilinear) ? bilinear_ : sampler_;
        // The blit source is the post-process chain's final output, or target_ when the chain is empty.
        const SDL_GPUTextureSamplerBinding binding{blitSource, blitSampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle

        SDL_EndGPURenderPass(pass);
    }

    // Submit even with no swapchain texture (e.g. minimised) so the command buffer is never
    // leaked; then release this frame's tilemap transfer buffers.
    SDL_SubmitGPUCommandBuffer(cmd);
    for (SDL_GPUTransferBuffer* transfer : scratch) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
    }
}

std::vector<Rgba8> Renderer::captureViewport(const FrameDrawState& frame) {
    return captureViewport(frame, 1);  // scale 1 — the byte-identical golden-capture path
}

std::vector<Rgba8> Renderer::captureViewport(const FrameDrawState& frame, int composeScale) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) fail("SDL_AcquireGPUCommandBuffer (captureViewport) failed");

    // Compose at the requested scale and download it instead of presenting (the same path renderFrame blits).
    // At scale 1 the compose grid is viewport res and the download region (viewport_.width × height) matches
    // the pre-output-res pipeline byte-for-byte (the golden-capture subject, captured with no interpolation).
    // At a scale > 1 the compose grid is composeScale·viewport, so the download captures the output-resolution
    // image the current evaluation grid produces — the parity seam for the crisp harness. The snap flag comes
    // from the renderer's evaluation grid (a no-op at scale 1, so the golden path is grid-independent).
    resizeComposeTargets(std::max(1, composeScale));
    std::vector<SDL_GPUTransferBuffer*> scratch;
    SDL_GPUTexture* composed = composeViewport(cmd, frame, scratch, 0.0f, false);

    const int w = composeW_;
    const int h = composeH_;
    // The download buffer holds the SOURCE texels (8 B/px for R16G16B16A16_FLOAT, 4 B/px for R8G8B8A8_UNORM);
    // the pack below converts them to Rgba8.
    constexpr Uint32 srcTexelBytes =
        (kViewportColorFormat == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) ? 8u : 4u;
    const Uint32 bytes = static_cast<Uint32>(w) * static_cast<Uint32>(h) * srcTexelBytes;

    SDL_GPUTransferBufferCreateInfo dlInfo{};
    dlInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    dlInfo.size  = bytes;
    SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(device_, &dlInfo);
    if (!download) fail("SDL_CreateGPUTransferBuffer (captureViewport) failed");

    // Download the composed viewport tightly packed (w pixels/row, no padding) into the transfer buffer.
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region{};
    region.texture = composed;
    region.w       = static_cast<Uint32>(w);
    region.h       = static_cast<Uint32>(h);
    region.d       = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = download;
    dst.offset          = 0;
    dst.pixels_per_row  = static_cast<Uint32>(w);
    dst.rows_per_layer  = static_cast<Uint32>(h);
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);

    // One-shot capture, not the runtime loop — submit and block on the fence until the download lands.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
    }
    for (SDL_GPUTransferBuffer* transfer : scratch) SDL_ReleaseGPUTransferBuffer(device_, transfer);

    // Pack the downloaded texels into Rgba8, keyed on the offscreen colour format. R8G8B8A8_UNORM is already
    // packed Rgba8 (a straight copy). R16G16B16A16_FLOAT carries the post-process headroom (a channel may
    // exceed 1); decode each half and quantize with round(clamp(v,0,1)·255) — the same conversion the 8-bit
    // swapchain blit applies on write, so the capture matches what a present would show.
    std::vector<Rgba8> pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    const void* mapped = SDL_MapGPUTransferBuffer(device_, download, false);
    if (!mapped) fail("SDL_MapGPUTransferBuffer (captureViewport) failed");
    if constexpr (kViewportColorFormat == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) {
        std::memcpy(pixels.data(), mapped, pixels.size() * sizeof(Rgba8));
    } else {
        static_assert(kViewportColorFormat == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                      "captureViewport packs R8G8B8A8_UNORM or R16G16B16A16_FLOAT — add a branch for any "
                      "other viewport colour format");
        const auto* texels = static_cast<const Uint16*>(mapped);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = Rgba8{quantizeChannel(halfBitsToFloat(texels[i * 4 + 0])),
                              quantizeChannel(halfBitsToFloat(texels[i * 4 + 1])),
                              quantizeChannel(halfBitsToFloat(texels[i * 4 + 2])),
                              quantizeChannel(halfBitsToFloat(texels[i * 4 + 3]))};
        }
    }
    SDL_UnmapGPUTransferBuffer(device_, download);
    SDL_ReleaseGPUTransferBuffer(device_, download);

    return pixels;
}

}  // namespace retropp
