#include "retropp/renderer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <variant>

#include "retropp/asset_policy.h"    // resolveAssetPolicy
#include "retropp/asset_registry.h"  // detail::configDefaultAssetPolicy / findEmbeddedAsset
#include "retropp/geometry.h"
#include "retropp/postprocess.h"
#include "retropp/shader_format.h"
#include "retropp/shader_registry.h"
#include "shaders/generated/blit_frag.h"
#include "shaders/generated/blit_vert.h"
#include "shaders/generated/displace_frag.h"
#include "shaders/generated/postprocess_vert.h"
#include "shaders/generated/region_select_curve_frag.h"
#include "shaders/generated/region_select_frag.h"
#include "shaders/generated/region_stencil_curve_frag.h"
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
// (std140-style 16-byte-register packing; no member straddles a 16-byte boundary). The
// trailing setOffsets[16] maps a TileCell::palette (0..15) → a palette flat offset; it lays out
// as 64 contiguous bytes, identical to the shader's `uint4 uSetOffsets[4]` (4 × 16 B registers).
struct TileUniforms {
    float scrollX, scrollY;      // register 0
    float layerW, layerH;
    float tilemapW, tilemapH;    // register 1
    float tilePx, alpha;
    float paletteStoreW;         // register 2: palette-store row width (colours); flat offset → (f%W, f/W)
    float pad0, pad1, pad2;
    std::uint32_t setOffsets[kPaletteSetSlots];  // registers 3..6 (uint4 ×4 in HLSL) — palette set
    float invRow0[4];            // inverse transform homography, rows 0..2 (registers 7..9)
    float invRow1[4];
    float invRow2[4];
    std::uint32_t hasTransform;  // register 10: x = hasTransform (0/1)
    std::uint32_t transformEdge; //              y = footprint edge (0 Blank / 1 Stretch)
    std::uint32_t wrap;          //              z = tilemap wrap mode (0 Repeat / 1 Clamp / 2 Blank)
    std::uint32_t pad3;          //              w pad
    // Atlas SET → store regions: slot i (a TileCell::atlasSelect) = (storeY, cols,
    // transparentIndex, _) of the i-th sheet in the layer's set; registers 11..26 (uint4 ×16 in HLSL).
    std::uint32_t atlasRegions[kAtlasSetSlots * 4];
};
static_assert(sizeof(TileUniforms) == 432, "TileUniforms must match the HLSL cbuffer layout");
static_assert(kPaletteSetSlots == 16, "setOffsets packs as uint4[4]; the shader assumes K=16");
static_assert(kAtlasSetSlots == 16, "atlasRegions packs as uint4[16]; the shader assumes K=16");

// The sprite vertex stage carries NO uniform buffer: the screen→clip transform is baked CPU-side
// into each GpuSprite (retropp::makeGpuSprite), so the vertex stage is a pure storage-buffer read.
// This sidesteps a Metal [[buffer]]-namespace collision a storage+uniform vertex stage would hit
// under the single-pass shader toolchain.

// Sprite fragment uniform — must match sprite.frag.hlsl's SpriteFragUniforms cbuffer (two
// 16-byte registers). A sprite layer is single-atlas; atlasStoreY is that atlas's top row in the
// flat atlas store, atlasCols its width in tiles.
struct SpriteFragUniforms {
    float atlasCols;     // register 0: atlas width in tiles
    float tilePx;        // tile edge length, pixels
    float alpha;         // layer alpha, [0,1]
    float paletteStoreW; // palette-store row width (colours); flat offset → (f%W, f/W)
    float atlasStoreY;   // register 1: this atlas's top row in the flat atlas store
    float pad0, pad1, pad2;
};
static_assert(sizeof(SpriteFragUniforms) == 32, "SpriteFragUniforms must match the HLSL cbuffer");

// Blit fragment uniform — the frame-level post-composite colour transform. Must match
// blit.frag.hlsl's BlitUniforms cbuffer exactly (three 16-byte registers: float3 + pad each).
// Filled from retropp::frameColorTransform(globalModifier, blend); the identity (mul=1, add=0,
// strength=0) reproduces the faithful blit value-for-value.
struct BlitFragUniforms {
    float mulR, mulG, mulB, pad0;                 // register 0
    float addR, addG, addB, pad1;                 // register 1
    float flashR, flashG, flashB, flashStrength;  // register 2
};
static_assert(sizeof(BlitFragUniforms) == 48, "BlitFragUniforms must match the blit.frag cbuffer");

// Row-displacement stage uniform — must match displace.frag.hlsl's DisplaceUniforms
// cbuffer exactly (two 16-byte registers). Filled from retropp::displaceParams(effect, viewport);
// the layout mirrors DisplaceParams's fields, with the axis carried as a uint.
struct DisplaceFragUniforms {
    float         amplitude, frequency, phase;  // register 0
    std::uint32_t axis;                         //   (0 = Horizontal, 1 = Vertical)
    float         invViewportW, invViewportH;
    std::uint32_t edge;                         //   (0 = Blank, 1 = Stretch)
    std::uint32_t blankTransparent;             //   (0 = opaque backdrop, 1 = transparent) — register 1
};
static_assert(sizeof(DisplaceFragUniforms) == 32, "DisplaceFragUniforms must match the displace.frag cbuffer");

// Built-in radial-ripple stage uniform — must match ripple.frag.hlsl's RippleUniforms
// cbuffer exactly (two 16-byte registers). Filled from retropp::rippleParams(effect, viewport);
// the layout mirrors RippleParams's fields (centre normalized px→UV, the inverse-viewport amplitude
// scale, the radial decay).
struct RippleFragUniforms {
    float centerU, centerV, amplitude, frequency;  // register 0
    float phase, invViewportW, invViewportH, decay; // register 1
};
static_assert(sizeof(RippleFragUniforms) == 32, "RippleFragUniforms must match the ripple.frag cbuffer");

// Scratch buffer size for a custom effect's cbuffer. A custom shader declares its OWN cbuffer
// (its own named params); the build reflects it and generates a packer (custom_effect_packers.h) that
// writes those params' bytes from the effect's inline fields. The renderer hands the packer a buffer this
// big, then pushes the size the packer reports — it never reads the param fields itself, so its view of
// ScreenSpaceEffect is independent of which params any consumer shader declares. 256 B covers a generous
// cbuffer (16 float4 registers); the packer's size is validated against it.
inline constexpr std::uint32_t kMaxCustomEffectUniformBytes = 256;

// The engine-controlled custom-effect cbuffer — must match retropp_effect.hlsli's
// RetroppEngineEffect (b0, space3) exactly. Carries the edge mode sampleSource() obeys, set from the
// effect's `edge`: 0 = Blank (transparent outside the frame — the faithful default), 1 = Stretch (clamp /
// smear). The engine fills + pushes this for EVERY custom stage (slot 0), so a layer's edge choice governs
// the custom shader, not the shader itself.
struct EngineEffectFragUniforms {
    std::uint32_t edgeClamp;         // 0 = blank, 1 = clamp
    std::uint32_t pad0, pad1, pad2;  // → 16 bytes (one cbuffer register)
};
static_assert(sizeof(EngineEffectFragUniforms) == 16,
              "EngineEffectFragUniforms must match retropp_effect.hlsli's RetroppEngineEffect cbuffer");

// The polygon-vertex cap the region cbuffer carries (packed two-per-register → uPoints[32] in the
// shader). The ShapePoints API stays unbounded (std::vector); a longer polygon is truncated here and
// warned. True-unbounded counts via a fragment storage buffer are a follow-up (needs on-device bring-up).
inline constexpr int kRegionCbufferMaxPoints = 64;

// Region-select gate uniform — must match region_select.frag.hlsl's RegionUniforms cbuffer
// exactly (36 × 16-byte registers). The ≤64 polygon vertices pack two-per-register (a cbuffer
// array would 16-byte-pad each float2), so points[128] lays out as the shader's `float4 uPoints[32]`.
// The inverse homography + misc register mirror retropp::regionParams; count is a float (uMisc.z), the
// EFFECTIVE (possibly truncated) vertex count, rounded back to a uint in the shader.
struct RegionSelectFragUniforms {
    float points[2 * kRegionCbufferMaxPoints];  // registers 0..31 : ≤64 vertices, xy packed 2-per-register
    float invRow0[4];                            // register 32
    float invRow1[4];                            // register 33
    float invRow2[4];                            // register 34
    float invViewportW, invViewportH;            // register 35
    float count;                                 //   (the effective vertex count, rounded to uint in the shader)
    float radius;
};
static_assert(sizeof(RegionSelectFragUniforms) == 576, "RegionSelectFragUniforms must match the region_select.frag cbuffer");

// Resolve a region + viewport into the region_select cbuffer bytes. Mirrors retropp::regionParams + packs
// the vertices two-per-register, truncating past kRegionCbufferMaxPoints (with a warning) and carrying
// the EFFECTIVE count so the shader never reads an unfilled slot.
RegionSelectFragUniforms makeRegionUniforms(const ShapePoints& region, ViewportResolution viewport) {
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
    u.invRow1[0] = p.invRow1[0]; u.invRow1[1] = p.invRow1[1]; u.invRow1[2] = p.invRow1[2]; u.invRow1[3] = 0.0f;
    u.invRow2[0] = p.invRow2[0]; u.invRow2[1] = p.invRow2[1]; u.invRow2[2] = p.invRow2[2]; u.invRow2[3] = 0.0f;
    u.invViewportW = p.invViewportW;
    u.invViewportH = p.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) vertex count
    u.radius       = p.radius;
    return u;
}

// The curve-boundary segment cap the curve region cbuffer carries (two registers per segment). The
// ShapePoints::curve API stays unbounded; a longer boundary is truncated here and warned, mirroring the
// polygon vertex cap. A genuinely longer boundary would move to a fragment storage buffer (its own
// on-device bring-up).
inline constexpr int kCurveRegionMaxSegments = 32;

// Curve region-select gate uniform — must match region_select_curve.frag.hlsl's CurveRegionUniforms
// cbuffer exactly (68 × 16-byte registers). Each segment packs two registers: register A {start.xy,
// control.xy}, register B {end.xy, degree, pad}. The inverse homography + misc tail mirror
// retropp::curveRegionParams; count is the EFFECTIVE (post-truncation) segment count.
struct CurveRegionSelectFragUniforms {
    float segs[8 * kCurveRegionMaxSegments];  // registers 0..63 : 2 regs/segment (8 floats)
    float invRow0[4];                          // register 64
    float invRow1[4];                          // register 65
    float invRow2[4];                          // register 66
    float invViewportW, invViewportH;          // register 67
    float count;                               //   (the effective segment count, rounded to uint in the shader)
    float radius;
};
static_assert(sizeof(CurveRegionSelectFragUniforms) == 1088,
              "CurveRegionSelectFragUniforms must match the region_select_curve.frag cbuffer (68 registers)");

// Resolve a curve region + viewport into the curve region-select cbuffer bytes. Mirrors
// retropp::curveRegionParams + packs the per-segment control points two registers each, truncating past
// kCurveRegionMaxSegments (with a warning) and carrying the EFFECTIVE count so the shader never reads an
// unfilled slot. The boundary is assumed analytic (linear + quadratic); a cubic boundary is sampled to a
// polygon by sampleCurveRegionToPolygon before this path.
CurveRegionSelectFragUniforms makeCurveRegionUniforms(const ShapePoints& region,
                                                      ViewportResolution viewport) {
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
    u.invRow1[0] = p.invRow1[0]; u.invRow1[1] = p.invRow1[1]; u.invRow1[2] = p.invRow1[2]; u.invRow1[3] = 0.0f;
    u.invRow2[0] = p.invRow2[0]; u.invRow2[1] = p.invRow2[1]; u.invRow2[2] = p.invRow2[2]; u.invRow2[3] = 0.0f;
    u.invViewportW = p.invViewportW;
    u.invViewportH = p.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) segment count
    u.radius       = p.radius;
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
    float mode;                                  // register 36 : 0 EraseInside, 1 EraseOutside (rounded to uint)
    float feather;                               //   shape-local px; 0 = hard edge
    float pad0, pad1;
};
static_assert(sizeof(StencilFragUniforms) == 592, "StencilFragUniforms must match the region_stencil.frag cbuffer");

// Resolve a region + stencil scalars + viewport into the stencil cbuffer bytes. Mirrors retropp::stencilParams
// + packs the vertices two-per-register, truncating past kRegionCbufferMaxPoints (with a warning) and
// carrying the EFFECTIVE count so the shader never reads an unfilled slot.
StencilFragUniforms makeStencilUniforms(const ShapePoints& region, StencilMode mode, float feather,
                                        ViewportResolution viewport) {
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
    u.invRow1[0] = p.region.invRow1[0]; u.invRow1[1] = p.region.invRow1[1]; u.invRow1[2] = p.region.invRow1[2]; u.invRow1[3] = 0.0f;
    u.invRow2[0] = p.region.invRow2[0]; u.invRow2[1] = p.region.invRow2[1]; u.invRow2[2] = p.region.invRow2[2]; u.invRow2[3] = 0.0f;
    u.invViewportW = p.region.invViewportW;
    u.invViewportH = p.region.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) vertex count
    u.radius       = p.region.radius;
    u.mode         = static_cast<float>(p.mode);
    u.feather      = p.feather;
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
    float mode;                                // register 68 : 0 EraseInside, 1 EraseOutside (rounded to uint)
    float feather;                             //   shape-local px; 0 = hard edge
    float pad0, pad1;
};
static_assert(sizeof(CurveStencilFragUniforms) == 1104,
              "CurveStencilFragUniforms must match the region_stencil_curve.frag cbuffer (69 registers)");

// Resolve a curve region + stencil scalars + viewport into the curve stencil cbuffer bytes. Mirrors
// retropp::curveStencilParams + packs the per-segment control points two registers each, truncating past
// kCurveRegionMaxSegments (with a warning). The boundary is assumed analytic (linear + quadratic); a cubic
// boundary is sampled to a polygon by sampleCurveRegionToPolygon before this path.
CurveStencilFragUniforms makeCurveStencilUniforms(const ShapePoints& region, StencilMode mode, float feather,
                                                  ViewportResolution viewport) {
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
    u.invRow1[0] = p.region.invRow1[0]; u.invRow1[1] = p.region.invRow1[1]; u.invRow1[2] = p.region.invRow1[2]; u.invRow1[3] = 0.0f;
    u.invRow2[0] = p.region.invRow2[0]; u.invRow2[1] = p.region.invRow2[1]; u.invRow2[2] = p.region.invRow2[2]; u.invRow2[3] = 0.0f;
    u.invViewportW = p.region.invViewportW;
    u.invViewportH = p.region.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) segment count
    u.radius       = p.region.radius;
    u.mode         = static_cast<float>(p.mode);
    u.feather      = p.feather;
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

    // Offscreen viewport target: a colour target the compositor renders into, and a sampler
    // source for the blit.
    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width                = static_cast<Uint32>(viewport_.width);
    texInfo.height               = static_cast<Uint32>(viewport_.height);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    target_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!target_) fail("SDL_CreateGPUTexture (viewport) failed");

    // Two viewport-sized scratch targets for the post-process chain. The chain
    // ping-pongs between them, never writing target_, so two suffice for any stage count; both
    // are COLOR_TARGET (a stage writes one) and SAMPLER (the next stage / the blit reads it).
    // Created up front (deterministic, no mid-frame allocation); ≈184 KB total at 160×144 — and
    // never touched when frame.postEffects is empty (an empty chain costs nothing).
    post0_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!post0_) fail("SDL_CreateGPUTexture (post0) failed");
    post1_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!post1_) fail("SDL_CreateGPUTexture (post1) failed");

    // Per-layer effect scratch: a Layer-scope effect renders its layer alone here and
    // composites it back displaced; a Below-scope effect displaces the accumulator into here and
    // swaps it with target_. Same format/usage as target_ (the two are interchangeable for the swap).
    layerScratch_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!layerScratch_) fail("SDL_CreateGPUTexture (layerScratch) failed");

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
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::tile_frag, 0, 3, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                          = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
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
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::sprite_frag, 0, 2, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format                            = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
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
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

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
        colorTarget.format                            = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
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
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

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
        colorTarget.format                            = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
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
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

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
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

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

    // Stencil pipelines (region erase): a fullscreen-triangle pass that reads ONE source (the layer's
    // rendered pixels, t0), computes the region SDF, and writes `source × survival` — erasing the layer
    // in/around the shape to reveal what's behind it. The subtractive sibling of the region_select gate
    // (which selects between two textures); a separate pass, so the gate is untouched. Two variants mirror
    // regionSelect_/regionSelectBlend_: regionStencil_ REPLACES its target (frame-level + Below scope);
    // regionStencilBlend_ composites the stenciled image PREMULTIPLIED-OVER target_ (Layer scope). Both
    // share region_stencil.frag (1 sampler + 1 uniform), differing only in blend state.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::region_stencil_frag, 1, 0, 1);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

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
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

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

    // Blit pipeline: the fragment shader uses one sampled texture (the viewport); the vertex
    // shader needs none. The pipeline's colour target must match the swapchain.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::blit_vert, 0);
        // 1 sampler (the viewport) + 1 uniform buffer (the frame-level colour transform, c.2).
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shaders::blit_frag, 1, 0, 1);

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

Renderer::~Renderer() {
    releaseSpriteBuffers();
    releaseTilemaps();
    releaseAtlases();
    releaseCustomStages();
    if (paletteStore_) SDL_ReleaseGPUTexture(device_, paletteStore_);
    if (blit_)          SDL_ReleaseGPUGraphicsPipeline(device_, blit_);
    if (regionStencilCurveBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilCurveBlend_);
    if (regionStencilCurve_)      SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilCurve_);
    if (regionStencilBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionStencilBlend_);
    if (regionStencil_)  SDL_ReleaseGPUGraphicsPipeline(device_, regionStencil_);
    if (regionSelectCurveBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectCurveBlend_);
    if (regionSelectCurve_)      SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectCurve_);
    if (regionSelectBlend_) SDL_ReleaseGPUGraphicsPipeline(device_, regionSelectBlend_);
    if (regionSelect_)  SDL_ReleaseGPUGraphicsPipeline(device_, regionSelect_);
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
    customReplace_.clear();
    customBlend_.clear();
}

PostProcessStageId Renderer::registerPostProcessStage(const ShaderVariants& fragment) {
    // Build the pipeline pair from the game's fragment + the shared fullscreen-triangle vertex stage. The
    // resource contract is fixed (the engine injects it): 1 sampled source texture + sampler, and TWO
    // uniform cbuffers — slot 0 = the engine cbuffer (RetroppEngineEffect: the edge mode sampleSource()
    // obeys), slot 1 = the shader's OWN reflected params, filled by its generated packer. Two pipelines,
    // differing only in blend state — the no-blend replace (frame-level / Below scope) and the
    // premultiplied-over blend (Layer scope), exactly mirroring displace_ / displaceBlend_.
    auto buildPipeline = [&](bool blend) -> SDL_GPUGraphicsPipeline* {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, shaders::postprocess_vert, 0);
        SDL_GPUShader* fragShader = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, fragment, 1, 0, 2);

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
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
    return id;
}

// Core indexed-atlas upload: one palette INDEX per pixel as R32_UINT, so a pixel can address an
// arbitrary palette. Read in-shader by integer Load — no sampler; colour is resolved from
// a palette at render time, not stored here. The public overloads widen 8-/16-bit source indices
// into the 32-bit texel (Texture2D<uint> reads any width identically).
AtlasId Renderer::uploadAtlas32(const std::uint32_t* indices, int width, int height, int transparentIndex) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");

    // Append this atlas to the flat atlas store (mirroring uploadPalette). The store stacks
    // every atlas vertically so a SINGLE map layer can mix tiles from several sheets — TileCell::
    // atlasSelect picks the region. Keep a CPU mirror of each atlas's pixels so the store can be
    // recreated + re-uploaded whole when a new atlas grows it. Uploads are amortized (load time).
    AtlasEntry entry;
    entry.data.assign(indices, indices + static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    entry.width            = width;
    entry.height           = height;
    entry.transparentIndex = transparentIndex;
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
}

AtlasId Renderer::uploadAtlas(const std::uint8_t* indices, int width, int height, int transparentIndex) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");
    const std::vector<std::uint32_t> widened(indices,
                                             indices + static_cast<std::size_t>(width) * height);
    return uploadAtlas32(widened.data(), width, height, transparentIndex);
}

AtlasId Renderer::uploadAtlas(const std::uint16_t* indices, int width, int height, int transparentIndex) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");
    const std::vector<std::uint32_t> widened(indices,
                                             indices + static_cast<std::size_t>(width) * height);
    return uploadAtlas32(widened.data(), width, height, transparentIndex);
}

AtlasId Renderer::uploadAtlas(const std::uint32_t* indices, int width, int height, int transparentIndex) {
    return uploadAtlas32(indices, width, height, transparentIndex);
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
                                 ContentKind kind, ReadOrder order, int count, int transparentIndex,
                                 int framesPerAnimation, std::optional<AssetPolicy> policy) {
    // Resolve embed-vs-load: per-call > EngineConfig::defaultAssetPolicy > loadAtlas's per-type default
    // (LoadFromPath). An Embed atlas decodes from the bytes the build baked in, keyed by its logical
    // path; if none were baked we fall through to the disk read.
    if (resolveAssetPolicy(policy, detail::configDefaultAssetPolicy(), AssetPolicy::LoadFromPath) ==
        AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> bytes = detail::findEmbeddedAsset(path.view());
            !bytes.empty()) {
            return loadAtlasFromMemory(bytes, assetSize, kind, order, count, transparentIndex,
                                       framesPerAnimation);
        }
    }
    // LoadFromPath (or an un-baked Embed): resolve the logical path against the runtime asset root.
    const LoadedImage img = loadPng(assetRoot() / path.c_str());  // throws on missing / decode / RGBA
    const AtlasId atlas =
        uploadAtlas(img.indices.data(), img.width, img.height, transparentIndex);  // uploads ONCE
    return AtlasManifest{atlas,
                         sliceLayout(PixelSize{img.width, img.height}, assetSize, kind, order, count),
                         seriesFrameGroup(kind, framesPerAnimation)};
}

AtlasManifest Renderer::loadAtlasFromMemory(std::span<const std::uint8_t> bytes, AssetDimensions assetSize,
                                           ContentKind kind, ReadOrder order, int count,
                                           int transparentIndex, int framesPerAnimation) {
    const LoadedImage img = loadPngFromMemory(bytes);
    const AtlasId atlas =
        uploadAtlas(img.indices.data(), img.width, img.height, transparentIndex);
    return AtlasManifest{atlas,
                         sliceLayout(PixelSize{img.width, img.height}, assetSize, kind, order, count),
                         seriesFrameGroup(kind, framesPerAnimation)};
}

PaletteId Renderer::uploadPalette(std::span<const Rgba8> colors) {
    // Arbitrary-size palettes: append the colours to a FLAT, contiguous CPU mirror; the
    // returned PaletteId IS this palette's flat offset into the store. The store texture is that flat
    // array wrapped kPaletteStoreWidth colours wide, its height grown to fit; palettes pack
    // contiguously (no per-palette padding) and may straddle rows. Uploads are amortized (load time /
    // on change), so recreating + re-uploading the whole store each time is cheap.
    const PaletteId id = static_cast<PaletteId>(paletteData_.size());
    paletteData_.insert(paletteData_.end(), colors.begin(), colors.end());

    const int W    = kPaletteStoreWidth;
    const int rows = std::max(1, static_cast<int>((paletteData_.size() + static_cast<std::size_t>(W) - 1)
                                                  / static_cast<std::size_t>(W)));

    if (paletteStore_) SDL_ReleaseGPUTexture(device_, paletteStore_);
    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    texInfo.width                = static_cast<Uint32>(W);
    texInfo.height               = static_cast<Uint32>(rows);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    paletteStore_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!paletteStore_) fail("SDL_CreateGPUTexture (palette store) failed");

    // Upload a W×rows buffer: the flat colours followed by opaque-black padding out to the last row.
    std::vector<Rgba8> upload(static_cast<std::size_t>(W) * static_cast<std::size_t>(rows));
    std::copy(paletteData_.begin(), paletteData_.end(), upload.begin());

    const Uint32 bytes = static_cast<Uint32>(upload.size()) * static_cast<Uint32>(sizeof(Rgba8));
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

    return id;
}

void Renderer::renderFrame(const FrameDrawState& frame, float /*alpha*/) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return;

    // Validate + order the layers (throws or warns per the collision policy).
    const std::vector<std::size_t> order = layerDrawOrder(frame.layers, collisionPolicy_);

    // ── Copy pass: (re)create + upload each TILES layer's tilemap, each SPRITES layer's buffer. ──
    tilemaps_.resize(frame.layers.size());
    spriteBufs_.resize(frame.layers.size());
    std::vector<SDL_GPUTransferBuffer*> scratch;
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
            ti.format               = SDL_GPU_TEXTUREFORMAT_R32_UINT;
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
        tbInfo.size  = count * static_cast<Uint32>(sizeof(std::uint32_t));
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
        if (!transfer) fail("SDL_CreateGPUTransferBuffer (tilemap) failed");

        auto* dst = static_cast<std::uint32_t*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
        if (!dst) fail("SDL_MapGPUTransferBuffer (tilemap) failed");
        const std::size_t have = std::min<std::size_t>(count, tc.cells.size());
        for (std::size_t k = 0; k < have; ++k) dst[k] = packTileCell(tc.cells[k]);
        for (std::size_t k = have; k < count; ++k) dst[k] = 0;  // pad short maps with cell 0 (tile 0, palette 0)
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

    // Build + upload each SPRITES layer's GpuSprite storage buffer (palette rows resolved CPU-
    // side). Grow-only: the buffer is recreated only when this frame's sprite count exceeds the
    // slot's capacity; otherwise it is reused and overwritten in place.
    for (const std::size_t idx : order) {
        const DrawLayer& layer = frame.layers[idx];
        if (contentKind(layer.content) != LayerContentKind::Sprites) continue;
        const SpriteContent& sc = std::get<SpriteContent>(layer.content);
        const int spriteCount = static_cast<int>(sc.sprites.size());
        if (spriteCount <= 0) continue;

        std::vector<GpuSprite> records;
        records.reserve(static_cast<std::size_t>(spriteCount));
        for (const Sprite& s : sc.sprites) {
            records.push_back(makeGpuSprite(s, spritePaletteOffset(sc.palettes, s.palette),
                                            viewport_.width, viewport_.height,
                                            layer.scroll.x, layer.scroll.y, layer.transform));
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

        // The region (storeY, cols, transparentIndex-or-none) of one atlas in the flat store.
        auto atlasRegion = [&](AtlasId aid) -> std::array<std::uint32_t, 3> {
            const AtlasEntry& a = atlases_[static_cast<std::size_t>(aid)];
            const std::uint32_t tIdx =
                a.transparentIndex < 0 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(a.transparentIndex);
            return {static_cast<std::uint32_t>(a.storeY),
                    static_cast<std::uint32_t>(a.width / kTilePx), tIdx};
        };

        if (contentKind(layer.content) == LayerContentKind::Tiles) {
            const TileContent& tc = std::get<TileContent>(layer.content);
            if (tc.widthInTiles <= 0 || tc.heightInTiles <= 0) return;
            const TilemapTex& slot = tilemaps_[idx];
            if (!slot.texture) return;
            if (!atlasStore_ || !paletteStore_) return;  // nothing uploaded → nothing to draw from

            // Resolve the layer's ATLAS SET: the explicit `atlases` set if present, else the
            // single `atlas` as a set of one. Validate every member.
            std::array<AtlasId, kAtlasSetSlots> setIds{};
            std::size_t setN = 0;
            if (!tc.atlases.empty()) {
                setN = std::min(tc.atlases.size(), kAtlasSetSlots);
                for (std::size_t i = 0; i < setN; ++i) setIds[i] = tc.atlases[i];
            } else {
                setIds[0] = tc.atlas;
                setN = 1;
            }
            for (std::size_t i = 0; i < setN; ++i) {
                if (static_cast<std::size_t>(setIds[i]) >= atlases_.size()) return;
            }

            TileUniforms u{};
            u.scrollX   = static_cast<float>(layer.scroll.x);
            u.scrollY   = static_cast<float>(layer.scroll.y);
            u.layerW    = static_cast<float>(viewport_.width);
            u.layerH    = static_cast<float>(viewport_.height);
            u.tilemapW  = static_cast<float>(tc.widthInTiles);
            u.tilemapH  = static_cast<float>(tc.heightInTiles);
            u.tilePx    = static_cast<float>(kTilePx);
            u.alpha     = clampAlpha(layer.alpha);
            u.paletteStoreW = static_cast<float>(kPaletteStoreWidth);

            // Map the layer's palette set to flat offsets for the per-tile palette-select.
            const std::array<std::uint32_t, kPaletteSetSlots> offsets = paletteSetOffsets(tc.palettes);
            std::copy(offsets.begin(), offsets.end(), u.setOffsets);

            // Fill the atlas-set region slots: real members get their store region; spare slots replicate
            // slot 0 so an out-of-range atlasSelect degenerately resolves to the first sheet (and never a
            // cols==0 divide), mirroring an out-of-range palette-select resolving to offset 0.
            for (std::size_t s = 0; s < kAtlasSetSlots; ++s) {
                const std::array<std::uint32_t, 3> r = atlasRegion(s < setN ? setIds[s] : setIds[0]);
                u.atlasRegions[s * 4 + 0] = r[0];
                u.atlasRegions[s * 4 + 1] = r[1];
                u.atlasRegions[s * 4 + 2] = r[2];
                u.atlasRegions[s * 4 + 3] = 0u;
            }

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

            // The tile path is all integer Load — bind three read-only storage textures (the flat atlas
            // store, this layer's tilemap cells, the palette store) at t0/t1/t2; no sampler.
            SDL_GPUTexture* storageTextures[3] = {atlasStore_, slot.texture, paletteStore_};
            SDL_BindGPUGraphicsPipeline(pass, tile_);
            SDL_BindGPUFragmentStorageTextures(pass, 0, storageTextures, 3);
            SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle
        } else {  // LayerContentKind::Sprites
            const SpriteContent& sc = std::get<SpriteContent>(layer.content);
            const int spriteCount = static_cast<int>(sc.sprites.size());
            if (spriteCount <= 0) return;
            const SpriteBuf& slot = spriteBufs_[idx];
            if (!slot.buffer) return;
            const auto atlasIdx = static_cast<std::size_t>(sc.atlas);
            if (atlasIdx >= atlases_.size()) return;
            if (!atlasStore_ || !paletteStore_) return;
            const std::array<std::uint32_t, 3> r = atlasRegion(sc.atlas);  // (storeY, cols, _)

            SpriteFragUniforms fu{};
            fu.atlasCols     = static_cast<float>(r[1]);
            fu.tilePx        = static_cast<float>(kTilePx);
            fu.alpha         = clampAlpha(layer.alpha);
            fu.paletteStoreW = static_cast<float>(kPaletteStoreWidth);
            fu.atlasStoreY   = static_cast<float>(r[0]);

            // Instanced per-sprite quads: the vertex stage reads the sprite records (already in
            // clip space) from a storage buffer (t0 space0) — no vertex uniform; the fragment
            // stage reads the flat atlas store + palette store (t0/t1 space2) + its uniform. 6
            // verts × spriteCount instances.
            SDL_GPUTexture* fragStorage[2] = {atlasStore_, paletteStore_};
            SDL_BindGPUGraphicsPipeline(pass, sprite_);
            SDL_BindGPUVertexStorageBuffers(pass, 0, &slot.buffer, 1);
            SDL_BindGPUFragmentStorageTextures(pass, 0, fragStorage, 2);
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(spriteCount), 0, 0);
        }
    };

    // Whether a screen-space effect can be rendered this frame. A built-in (RowDisplacement / Ripple)
    // always can; a Custom effect is renderable iff its handle indexes a registered stage (its parameters
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
    // Blank-edge colour), Ripple → ripple_/rippleBlend_ + RippleParams; a Custom effect binds the
    // registered pipeline pair + pushes the game's own uniform bytes. Same scope/compositing/ping-pong
    // plumbing for every kind.
    auto runEffect = [&](SDL_GPUTexture* dest, SDL_GPUTexture* source, const ScreenSpaceEffect& effect,
                         bool blankTransparent, bool blend, SDL_GPULoadOp loadOp) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);

        const SDL_GPUTextureSamplerBinding binding{source, sampler_};  // nearest, CLAMP_TO_EDGE
        if (effectUsesCustomShader(effect)) {
            const auto id = static_cast<std::size_t>(effect.customShader);
            SDL_BindGPUGraphicsPipeline(pass, blend ? customBlend_[id] : customReplace_[id]);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            // Slot 0 — the engine cbuffer: the edge mode sampleSource() obeys, from the effect's `edge`
            // (Blank ⇒ transparent outside the frame, the default; Stretch ⇒ clamp). The layer decides it.
            const EngineEffectFragUniforms eng{
                effect.edge == DisplacementEdge::Stretch ? 1u : 0u, 0u, 0u, 0u};
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
                                        p.phase, p.invViewportW, p.invViewportH, p.decay};
            SDL_BindGPUGraphicsPipeline(pass, blend ? rippleBlend_ : ripple_);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &ru, sizeof(ru));
        } else {
            const DisplaceParams p =
                displaceParams(effect, PixelSize{viewport_.width, viewport_.height}, blankTransparent);
            const DisplaceFragUniforms du{p.amplitude, p.frequency, p.phase, p.axis,
                                          p.invViewportW, p.invViewportH, p.edge, p.blankTransparent};
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
                               const ShapePoints& region, bool blend, SDL_GPULoadOp loadOp) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = dest;
        t.clear_color = kBackdropClear;
        t.load_op     = loadOp;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);

        const SDL_GPUTextureSamplerBinding binds[2] = {{eff, sampler_}, {source, sampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, binds, 2);

        // An analytic curve boundary (linear + quadratic) takes the curve pipelines + cbuffer — exact,
        // no facets. A curve-free region OR a curve carrying a cubic segment (sampled to a polygon here)
        // takes the polygon pipelines — byte-identical to the shipped path for a curve-free region.
        if (!region.curve.empty() && curveRegionIsAnalytic(region.curve)) {
            const CurveRegionSelectFragUniforms cu = makeCurveRegionUniforms(region, viewport_);
            SDL_BindGPUGraphicsPipeline(pass, blend ? regionSelectCurveBlend_ : regionSelectCurve_);
            SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
        } else {
            const RegionSelectFragUniforms ru = region.curve.empty()
                ? makeRegionUniforms(region, viewport_)
                : makeRegionUniforms(sampleCurveRegionToPolygon(region), viewport_);
            SDL_BindGPUGraphicsPipeline(pass, blend ? regionSelectBlend_ : regionSelect_);
            SDL_PushGPUFragmentUniformData(cmd, 0, &ru, sizeof(ru));
        }
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };

    // The stencil pass: read ONE `source` (t0) and write `source × survival` — erasing the source's own
    // pixels in/around `region` per `mode`/`feather` (EraseInside punches a hole, EraseOutside keeps the
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

        if (!region.curve.empty() && curveRegionIsAnalytic(region.curve)) {
            const CurveStencilFragUniforms cu = makeCurveStencilUniforms(region, mode, feather, viewport_);
            SDL_BindGPUGraphicsPipeline(pass, blend ? regionStencilCurveBlend_ : regionStencilCurve_);
            SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
        } else {
            const StencilFragUniforms su = region.curve.empty()
                ? makeStencilUniforms(region, mode, feather, viewport_)
                : makeStencilUniforms(sampleCurveRegionToPolygon(region), mode, feather, viewport_);
            SDL_BindGPUGraphicsPipeline(pass, blend ? regionStencilBlend_ : regionStencil_);
            SDL_PushGPUFragmentUniformData(cmd, 0, &su, sizeof(su));
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
    };

    // Append the confined step for ONE effect. Every effect is region-agnostic: it confines to `defaultShape`
    // (the owning Region's shape) when it has one, else it is whole-reach. A Transparency is no exception —
    // its shape comes from its Region just like a colour effect's. None / invalid-Custom effects are dropped.
    auto appendEffectSteps = [&](std::vector<ConfinedStep>& steps, const ScreenSpaceEffect& e,
                                 bool hasDefaultShape, const ShapePoints& defaultShape) {
        if (e.kind == ScreenSpaceEffectKind::None || !effectRenderable(e)) return;
        if (hasDefaultShape) steps.push_back({&e, true, defaultShape});  // confined by the owning Region
        else                 steps.push_back({&e, false, {}});           // whole-reach (no shape)
    };

    // A layer's confined-step list: each whole-reach effect in the layer's effects chain (in order), then
    // each region's effects (confined to that region's shape). The per-layer loop partitions the result by
    // scope (Layer vs Below) — so a Below-scope step (e.g. a transparency side effect) runs on the
    // accumulator after the layer composites, reaching the layers showing through a see-through region.
    auto buildSteps = [&](const DrawLayer& layer) {
        std::vector<ConfinedStep> steps;
        for (const ScreenSpaceEffect& e : layer.effects)
            appendEffectSteps(steps, e, /*hasDefaultShape=*/false, {});
        for (const Region& region : layer.regions)
            for (const ScreenSpaceEffect& e : region.effects)
                appendEffectSteps(steps, e, /*hasDefaultShape=*/true, region.shape);
        return steps;
    };

    // Apply one Below-scope confined step to the WHOLE accumulator (target_): transform/erase it confined
    // to the step's shape into layerScratch_, then swap it in — this layer's content (already composited
    // into target_) AND everything beneath it. The single-step case is the plain whole-layer Below composite.
    auto applyBelowStep = [&](const ConfinedStep& s) {
        if (s.eff->kind == ScreenSpaceEffectKind::Transparency) {
            runStencil(layerScratch_, target_, s.shape, s.eff->stencil, s.eff->feather,
                       /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
        } else if (s.confined && s.shape.hasRegion()) {
            runEffect(post0_, target_, *s.eff, /*blankTransparent=*/false, /*blend=*/false,
                      SDL_GPU_LOADOP_DONT_CARE);
            runRegionSelect(layerScratch_, post0_, target_, s.shape, /*blend=*/false,
                            SDL_GPU_LOADOP_DONT_CARE);
        } else {
            runEffect(layerScratch_, target_, *s.eff, /*blankTransparent=*/false, /*blend=*/false,
                      SDL_GPU_LOADOP_DONT_CARE);
        }
        std::swap(target_, layerScratch_);
    };

    // Apply an isolated layer's confined-step chain on its own premultiplied scratch (starting at
    // layerScratch_, where the layer content was rendered): every step but the LAST replaces into a
    // ping-pong scratch (so step n+1 sees step n's output), the LAST composites premultiplied-over
    // target_. A single step is the plain per-layer composite (one effect, composited once).
    auto applyLayerChain = [&](const std::vector<ConfinedStep>& steps, SDL_GPULoadOp compositeLoad) {
        SDL_GPUTexture* pool[3] = {layerScratch_, post0_, post1_};
        auto other = [&](SDL_GPUTexture* a, SDL_GPUTexture* b) -> SDL_GPUTexture* {
            for (SDL_GPUTexture* t : pool)
                if (t != a && t != b) return t;
            return pool[0];
        };
        SDL_GPUTexture* cur = layerScratch_;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const ConfinedStep& s    = steps[i];
            const bool          last = (i + 1 == steps.size());
            const SDL_GPULoadOp lop  = last ? compositeLoad : SDL_GPU_LOADOP_DONT_CARE;
            if (s.eff->kind == ScreenSpaceEffectKind::Transparency) {
                SDL_GPUTexture* dest = last ? target_ : other(cur, cur);
                runStencil(dest, cur, s.shape, s.eff->stencil, s.eff->feather, /*blend=*/last, lop);
                if (!last) cur = dest;
            } else if (s.confined && s.shape.hasRegion()) {
                SDL_GPUTexture* tmp  = other(cur, cur);
                SDL_GPUTexture* dest = last ? target_ : other(cur, tmp);
                runEffect(tmp, cur, *s.eff, /*blankTransparent=*/true, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
                runRegionSelect(dest, tmp, cur, s.shape, /*blend=*/last, lop);
                if (!last) cur = dest;
            } else {
                SDL_GPUTexture* dest = last ? target_ : other(cur, cur);
                runEffect(dest, cur, *s.eff, /*blankTransparent=*/true, /*blend=*/last, lop);
                if (!last) cur = dest;
            }
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

    for (const std::size_t idx : order) {
        const DrawLayer& layer = frame.layers[idx];

        // The layer's full confined-step list: each whole-reach effect in the layer's effects chain, then
        // each region's effects (confined to the region's shape). buildSteps drops None / invalid-Custom.
        std::vector<ConfinedStep> steps = buildSteps(layer);

        // Nothing to do beyond the layer's own content → the batched faithful path.
        if (steps.empty()) {
            if (!batch) openBatch();
            drawLayer(batch, idx);
            continue;
        }

        // Two-phase scope partition. Layer-scope steps work on the layer's OWN isolated content (composited
        // over the accumulator); Below-scope steps adjust the WHOLE accumulator after the layer composites.
        // Submission order is preserved within each phase; Layer always runs before Below — the only coherent
        // order, since a Below step reads the post-composite accumulator. A mixed-scope step list is therefore
        // defined, not ambiguous (a transparency side effect, emitted Below, reaches the layers showing
        // through the see-through region); a chain that interleaves Below before Layer is the escalation case.
        std::vector<ConfinedStep> layerSteps, belowSteps;
        for (const ConfinedStep& s : steps)
            (effectIsBelowScope(*s.eff) ? belowSteps : layerSteps).push_back(s);

        if (!layerSteps.empty()) {
            // Layer (isolated) scope: render this layer alone into layerScratch_ (transparent-cleared), then
            // apply the Layer-scope chain on the layer's own scratch — the LAST step composites premultiplied-
            // over target_ (transparent blank so an exposed strip reveals the layers below). A single step is
            // the plain per-layer composite (one effect, composited once).
            closeBatch();
            {
                SDL_GPUColorTargetInfo lt{};
                lt.texture     = layerScratch_;
                lt.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};  // transparent
                lt.load_op     = SDL_GPU_LOADOP_CLEAR;
                lt.store_op    = SDL_GPU_STOREOP_STORE;
                SDL_GPURenderPass* lp = SDL_BeginGPURenderPass(cmd, &lt, 1, nullptr);
                drawLayer(lp, idx);
                SDL_EndGPURenderPass(lp);
            }
            const SDL_GPULoadOp compositeLoad = targetInitialized ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
            targetInitialized = true;
            applyLayerChain(layerSteps, compositeLoad);
        } else {
            // No Layer-scope step: composite the layer's own content straight into the accumulator (the
            // batched faithful draw); the Below-scope steps below then adjust the whole accumulator.
            if (!batch) openBatch();
            drawLayer(batch, idx);
            closeBatch();
        }

        // Below-scope phase: each step adjusts the whole accumulator at this z (this layer's content AND
        // everything beneath it), confined to its region's shape, in submission order.
        for (const ConfinedStep& s : belowSteps) applyBelowStep(s);
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
    // The frame-level steps: the whole-frame postEffects (no shape) then the frame's confined regions (each
    // region's effects, confined to that region's shape). buildSteps' frame analogue. Scope plays no role
    // here — frame-level steps are inherently whole-frame, run in submission order on the composited image.
    std::vector<ConfinedStep> frameSteps;
    for (const ScreenSpaceEffect& e : frame.postEffects)
        appendEffectSteps(frameSteps, e, /*hasDefaultShape=*/false, {});
    for (const Region& region : frame.regions)
        for (const ScreenSpaceEffect& e : region.effects)
            appendEffectSteps(frameSteps, e, /*hasDefaultShape=*/true, region.shape);

    SDL_GPUTexture* blitSource = target_;
    {
        SDL_GPUTexture* readTex    = target_;
        SDL_GPUTexture* scratch[2] = {post0_, post1_};
        std::size_t     applied    = 0;  // counts only rendered passes → preserves read≠write alternation
        for (const ConfinedStep& s : frameSteps) {
            SDL_GPUTexture* writeTex = scratch[applied % 2];

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
                runEffect(layerScratch_, readTex, *s.eff,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
                runRegionSelect(writeTex, layerScratch_, readTex, s.shape,
                                /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            } else {
                runEffect(writeTex, readTex, *s.eff,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            }
            blitSource = writeTex;
            readTex    = writeTex;
            ++applied;
        }
    }

    // ── Blit pass: viewport → swapchain, integer-scaled + letterboxed. ──────────────────────────
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 width  = 0;
    Uint32 height = 0;
    // On Metal, the BLOCKING acquire busy-waits a core (SDL bug); use the non-blocking acquire there and
    // let the host frame deadline pace. Non-blocking returns true with a null swapchain when the frame
    // isn't ready yet → the existing `swapchain != nullptr` guard skips this frame's blit/present cleanly
    // (paced, so skips are rare). Vulkan/D3D12 keep the blocking acquire (they OS-block, no spin).
    const bool acquired = acquireNonBlocking_
        ? SDL_AcquireGPUSwapchainTexture(cmd, window_, &swapchain, &width, &height)
        : SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, &width, &height);
    if (acquired && swapchain != nullptr) {
        SDL_GPUColorTargetInfo scTarget{};
        scTarget.texture     = swapchain;
        scTarget.clear_color = kLetterboxClear;
        scTarget.load_op     = SDL_GPU_LOADOP_CLEAR;
        scTarget.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &scTarget, 1, nullptr);

        // The viewport always fills the window at the largest integer scale that fits, centred +
        // letterboxed. Output SIZE is the window's size (Platform owns it, via setWindowSize sized to
        // viewport × the chosen scale); the blit just fills whatever window it's given, crisply.
        const IntRect dest = integerScaleToFitRect(
            PixelSize{static_cast<int>(width), static_cast<int>(height)},
            PixelSize{viewport_.width, viewport_.height});

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

        // Frame-level post-composite colour transform: a default frame resolves to the identity
        // (mul=1, add=0, strength=0) → the faithful blit, value-for-value.
        const FrameColorTransform ct = frameColorTransform(frame.globalModifier, frame.blend);
        const BlitFragUniforms bu{ct.mulR, ct.mulG, ct.mulB, 0.0f,
                                  ct.addR, ct.addG, ct.addB, 0.0f,
                                  ct.flashR, ct.flashG, ct.flashB, ct.flashStrength};

        SDL_BindGPUGraphicsPipeline(pass, blit_);
        // Select the blit sampler by the runtime mode — nearest (faithful, crisp) or bilinear
        // (smoothed). Same blit pipeline; only the bound sampler differs (sampler state is
        // pipeline-independent, so no shader change).
        SDL_GPUSampler* blitSampler = (sampling_ == SamplingMode::Bilinear) ? bilinear_ : sampler_;
        // The blit source is the post-process chain's final output, or target_ when the chain is
        // empty (the faithful path).
        const SDL_GPUTextureSamplerBinding binding{blitSource, blitSampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, &bu, sizeof(bu));
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

}  // namespace retropp
