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
#include "shaders/generated/region_select_frag.h"
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
// uploadPalette, so palette capacity is W × maxTextureHeight — arbitrary for any real use (ENG-2.K
// removes the former 256-colour cap). 16384 keeps the height minimal for typical palettes, and
// W×4 = 65536 B/row is 256-aligned for backend upload-pitch requirements.
constexpr int kPaletteStoreWidth = 16384;

// Per-layer uniform block — must match tile.frag.hlsl's TileUniforms cbuffer byte-for-byte
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
    float invRow0[4];            // ENG-2.D.1: inverse transform homography, rows 0..2 (registers 7..9)
    float invRow1[4];
    float invRow2[4];
    std::uint32_t hasTransform;  // register 10: x = hasTransform (0/1)
    std::uint32_t transformEdge; //              y = footprint edge (0 Blank / 1 Stretch)
    std::uint32_t wrap;          //              z = tilemap wrap mode (0 Repeat / 1 Clamp / 2 Blank) (ENG-2.E)
    std::uint32_t pad3;          //              w pad
    // ENG-2.L atlas SET → store regions: slot i (a TileCell::atlasSelect) = (storeY, cols,
    // transparentIndex, _) of the i-th sheet in the layer's set; registers 11..26 (uint4 ×16 in HLSL).
    std::uint32_t atlasRegions[kAtlasSetSlots * 4];
};
static_assert(sizeof(TileUniforms) == 432, "TileUniforms must match the HLSL cbuffer layout");
static_assert(kPaletteSetSlots == 16, "setOffsets packs as uint4[4]; the shader assumes K=16");
static_assert(kAtlasSetSlots == 16, "atlasRegions packs as uint4[16]; the shader assumes K=16");

// The sprite vertex stage carries NO uniform buffer: the screen→clip transform is baked CPU-side
// into each GpuSprite (retropp::makeGpuSprite), so the vertex stage is a pure storage-buffer read.
// This sidesteps a Metal [[buffer]]-namespace collision a storage+uniform vertex stage would hit
// under the single-pass shader toolchain (see PLAN Amendment A2).

// Sprite fragment uniform — must match sprite.frag.hlsl's SpriteFragUniforms cbuffer (two
// 16-byte registers). A sprite layer is single-atlas; atlasStoreY is that atlas's top row in the
// flat atlas store (ENG-2.L), atlasCols its width in tiles.
struct SpriteFragUniforms {
    float atlasCols;     // register 0: atlas width in tiles
    float tilePx;        // tile edge length, pixels
    float alpha;         // layer alpha, [0,1]
    float paletteStoreW; // palette-store row width (colours); flat offset → (f%W, f/W)
    float atlasStoreY;   // register 1: this atlas's top row in the flat atlas store (ENG-2.L)
    float pad0, pad1, pad2;
};
static_assert(sizeof(SpriteFragUniforms) == 32, "SpriteFragUniforms must match the HLSL cbuffer");

// Blit fragment uniform — the frame-level post-composite colour transform (ENG-2.B.2.c.2).
// Must match blit.frag.hlsl's BlitUniforms cbuffer byte-for-byte (three 16-byte registers:
// float3 + pad each). Filled from retropp::frameColorTransform(globalModifier, blend); the identity
// (mul=1, add=0, strength=0) reproduces the faithful baseline blit value-for-value.
struct BlitFragUniforms {
    float mulR, mulG, mulB, pad0;                 // register 0
    float addR, addG, addB, pad1;                 // register 1
    float flashR, flashG, flashB, flashStrength;  // register 2
};
static_assert(sizeof(BlitFragUniforms) == 48, "BlitFragUniforms must match the blit.frag cbuffer");

// Row-displacement stage uniform (ENG-2.C.2.a) — must match displace.frag.hlsl's DisplaceUniforms
// cbuffer byte-for-byte (two 16-byte registers). Filled from retropp::displaceParams(effect, viewport);
// the layout mirrors DisplaceParams's fields, with the axis carried as a uint.
struct DisplaceFragUniforms {
    float         amplitude, frequency, phase;  // register 0
    std::uint32_t axis;                         //   (0 = Horizontal, 1 = Vertical)
    float         invViewportW, invViewportH;
    std::uint32_t edge;                         //   (0 = Blank, 1 = Stretch)
    std::uint32_t blankTransparent;             //   (0 = opaque backdrop, 1 = transparent) — register 1
};
static_assert(sizeof(DisplaceFragUniforms) == 32, "DisplaceFragUniforms must match the displace.frag cbuffer");

// Built-in radial-ripple stage uniform (ENG-2.I.a) — must match ripple.frag.hlsl's RippleUniforms
// cbuffer byte-for-byte (two 16-byte registers). Filled from retropp::rippleParams(effect, viewport);
// the layout mirrors RippleParams's fields (centre normalized px→UV, the inverse-viewport amplitude
// scale, the radial decay).
struct RippleFragUniforms {
    float centerU, centerV, amplitude, frequency;  // register 0
    float phase, invViewportW, invViewportH, decay; // register 1
};
static_assert(sizeof(RippleFragUniforms) == 32, "RippleFragUniforms must match the ripple.frag cbuffer");

// Scratch buffer size for a custom effect's cbuffer (ENG-2.I.b). A custom shader declares its OWN cbuffer
// (its own named params); the build reflects it and generates a packer (custom_effect_packers.h) that
// writes those params' bytes from the effect's inline fields. The renderer hands the packer a buffer this
// big, then pushes the size the packer reports — it never reads the param fields itself, so its view of
// ScreenSpaceEffect is independent of which params any consumer shader declares. 256 B covers a generous
// cbuffer (16 float4 registers); the packer's size is validated against it.
inline constexpr std::uint32_t kMaxCustomEffectUniformBytes = 256;

// The engine-controlled custom-effect cbuffer (ENG-2.I.b) — must match retropp_effect.hlsli's
// RetroppEngineEffect (b0, space3) byte-for-byte. Carries the edge mode sampleSource() obeys, set from the
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

// Region-select gate uniform (ENG-2.F) — must match region_select.frag.hlsl's RegionUniforms cbuffer
// byte-for-byte (36 × 16-byte registers). The ≤64 polygon vertices pack two-per-register (a cbuffer
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
    u.invRow0[0] = p.invRow0[0]; u.invRow0[1] = p.invRow0[1]; u.invRow0[2] = p.invRow0[2]; u.invRow0[3] = 0.0f;
    u.invRow1[0] = p.invRow1[0]; u.invRow1[1] = p.invRow1[1]; u.invRow1[2] = p.invRow1[2]; u.invRow1[3] = 0.0f;
    u.invRow2[0] = p.invRow2[0]; u.invRow2[1] = p.invRow2[1]; u.invRow2[2] = p.invRow2[2]; u.invRow2[3] = 0.0f;
    u.invViewportW = p.invViewportW;
    u.invViewportH = p.invViewportH;
    u.count        = static_cast<float>(n);  // the effective (post-truncation) vertex count
    u.radius       = p.radius;
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

    // Two viewport-sized scratch targets for the post-process chain (ENG-2.C.2.a). The chain
    // ping-pongs between them, never writing target_, so two suffice for any stage count; both
    // are COLOR_TARGET (a stage writes one) and SAMPLER (the next stage / the blit reads it).
    // Created up front (deterministic, no mid-frame allocation); ≈184 KB total at 160×144 — and
    // never touched when frame.postEffects is empty, so the faithful path is byte-unchanged.
    post0_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!post0_) fail("SDL_CreateGPUTexture (post0) failed");
    post1_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!post1_) fail("SDL_CreateGPUTexture (post1) failed");

    // Per-layer effect scratch (ENG-2.C.2.b): a Layer-scope effect renders its layer alone here and
    // composites it back displaced; a Below-scope effect displaces the accumulator into here and
    // swaps it with target_. Same format/usage as target_ (the two are interchangeable for the swap).
    layerScratch_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!layerScratch_) fail("SDL_CreateGPUTexture (layerScratch) failed");

    // Nearest filtering, clamped — the faithful baseline (bilinear/CRT are ENG-2.C). Shared
    // by the tile compositor (atlas sampling) and the blit (viewport sampling).
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
    // the blit-only alternate the renderer binds under SamplingMode::Bilinear (ENG-2.C.1). The
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

    // Row-displacement post-process pipeline (ENG-2.C.2.a): a fullscreen-triangle pass that
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

    // Per-layer (Layer scope) composite pipeline (ENG-2.C.2.b): the SAME displace shaders, but this
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

    // Built-in radial-ripple post-process pipeline (ENG-2.I.a): the second engine effect kind, the
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

    // Region-select gate pipelines (ENG-2.F): a fullscreen-triangle pass that reads the effect result
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
// arbitrary palette (ENG-2.K). Read in-shader by integer Load — no sampler; colour is resolved from
// a palette at render time, not stored here. The public overloads widen 8-/16-bit source indices
// into the 32-bit texel (Texture2D<uint> reads any width identically).
AtlasId Renderer::uploadAtlas32(const std::uint32_t* indices, int width, int height, int transparentIndex) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");

    // ENG-2.L: append this atlas to the flat atlas store (mirroring uploadPalette). The store stacks
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
    // Arbitrary-size palettes (ENG-2.K): append the colours to a FLAT, contiguous CPU mirror; the
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

    // ── Viewport composite (ENG-2.C.2.b: segmented for per-layer screen-space effects) ─────────
    // Layers composite back-to-front into target_. A layer with NO effect draws straight into the
    // accumulator (consecutive such layers batch into one render pass — CLEAR on the first, LOAD
    // after). A per-layer effect splits the loop: a Below-scope effect draws its content into the
    // accumulator then displaces the WHOLE accumulator (this layer + everything beneath) and swaps it
    // in; a Layer-scope effect renders ITS layer alone into layerScratch_ and composites it back
    // displaced. A frame with NO effect layers never splits → exactly the pre-C.2.b single CLEAR pass
    // over all layers → byte-identical. ────────────────────────────────────────────────────────────

    // One tile/sprite layer drawn into the given pass — shared by the batched path, the Below content
    // draw, and the isolated-Layer offscreen render. (The per-layer screen-space effect is realized by
    // the caller, not here; this just composites the layer's content.)
    auto drawLayer = [&](SDL_GPURenderPass* pass, std::size_t idx) {
        const DrawLayer& layer = frame.layers[idx];

        // The region (storeY, cols, transparentIndex-or-none) of one atlas in the flat store (ENG-2.L).
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

            // Resolve the layer's ATLAS SET (ENG-2.L): the explicit `atlases` set if present, else the
            // single `atlas` as a set of one (the byte-identical pre-ENG-2.L path). Validate every member.
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

            // ENG-2.D.1 — per-layer transform: upload the INVERSE homography (the fragment maps a
            // destination pixel back to content space, perspective divide included) + the footprint
            // edge mode. Identity ⇒ hasTransform 0 ⇒ the fragment takes the faithful pre-D.1 path
            // byte-for-byte.
            u.hasTransform  = layer.transform.isIdentity() ? 0u : 1u;
            u.transformEdge = static_cast<std::uint32_t>(layer.transformEdge);
            // ENG-2.E — per-layer tilemap wrap mode (Repeat default ⇒ faithful toroidal output).
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

    // The region gate (ENG-2.F): read `eff` (the full-frame effect result, t0) + `source` (the
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
        const RegionSelectFragUniforms ru = makeRegionUniforms(region, viewport_);
        SDL_BindGPUGraphicsPipeline(pass, blend ? regionSelectBlend_ : regionSelect_);
        SDL_BindGPUFragmentSamplers(pass, 0, binds, 2);
        SDL_PushGPUFragmentUniformData(cmd, 0, &ru, sizeof(ru));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
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

        // No effect — OR an invalid Custom pass under WarnAndResolve (effectRenderable warns + returns
        // false) — composites on the unchanged faithful path. (Under Throw, effectRenderable throws.)
        if (!layerHasScreenSpaceEffect(layer) || !effectRenderable(layer.effect)) {
            if (!batch) openBatch();
            drawLayer(batch, idx);
            continue;
        }

        if (effectIsBelowScope(layer.effect)) {            // adjustment layer: this layer + everything below
            if (!batch) openBatch();
            drawLayer(batch, idx);                         // composite into the accumulator first
            closeBatch();
            // Transform the whole accumulated target_ into layerScratch_ (opaque-backdrop blank, like
            // the frame-level chain — it is transforming the opaque scene), then SWAP so target_ becomes
            // the transformed accumulator. DONT_CARE: the fullscreen pass overwrites every pixel. With a
            // region (ENG-2.F), the effect lands in post0_ (free here) and the gate selects
            // inside?eff:target into layerScratch_ — the displacement confines to the shape, the rest of
            // the scene below rides through unchanged.
            if (layer.effect.region.hasRegion()) {
                runEffect(post0_, target_, layer.effect,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
                runRegionSelect(layerScratch_, post0_, target_, layer.effect.region,
                                /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            } else {
                runEffect(layerScratch_, target_, layer.effect,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            }
            std::swap(target_, layerScratch_);
            continue;
        }

        // Layer (isolated) scope: render this layer alone into layerScratch_ (transparent-cleared),
        // then composite it back into target_ transformed (premultiplied-over, transparent blank so the
        // exposed strip reveals the layers below).
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
        if (layer.effect.region.hasRegion()) {
            // Region (ENG-2.F): the displaced isolated layer lands (replace) in post0_, then the gate
            // composites inside?eff:layer premultiplied-over target_ — inside the shape the layer is
            // effected, outside it composites undisplaced. Both eff and layerScratch_ are premultiplied.
            runEffect(post0_, layerScratch_, layer.effect,
                      /*blankTransparent=*/true, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            runRegionSelect(target_, post0_, layerScratch_, layer.effect.region,
                            /*blend=*/true, compositeLoad);
        } else {
            runEffect(target_, layerScratch_, layer.effect,
                      /*blankTransparent=*/true, /*blend=*/true, compositeLoad);
        }
    }
    closeBatch();

    // Empty frame (no layer ever cleared target_): clear it to the backdrop so the blit shows the
    // backdrop, matching the pre-C.2.b always-cleared viewport pass.
    if (!targetInitialized) {
        SDL_GPUColorTargetInfo t{};
        t.texture     = target_;
        t.clear_color = kBackdropClear;
        t.load_op     = SDL_GPU_LOADOP_CLEAR;
        t.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        SDL_EndGPURenderPass(pass);
    }

    // ── Post-process chain (ENG-2.C.2.a + .C.3): frame-level screen-space effects, run on the finished
    //    viewport image before the blit. Each active effect is one fullscreen-triangle pass that reads
    //    the previous image and writes a scratch target; the two scratch targets strictly alternate by
    //    APPLIED-pass count, so a pass never samples the texture it writes (target_ is never written).
    //    Effects dispatch on kind — a built-in RowDisplacement binds displace_ + resolved params; a
    //    Custom effect (ENG-2.C.3) binds its registered replace pipeline + pushes the game's uniform,
    //    composing in submission order with the built-ins. An invalid Custom pass is skipped (under
    //    WarnAndResolve) without advancing the ping-pong. An empty chain leaves blitSource == target_ →
    //    the blit is byte-identical to C.1. ──────────────────────────────────────────────────────────
    const std::vector<ScreenSpaceEffect> effects = activeFrameEffects(frame);
    SDL_GPUTexture* blitSource = target_;
    {
        SDL_GPUTexture* readTex    = target_;
        SDL_GPUTexture* scratch[2] = {post0_, post1_};
        std::size_t     applied    = 0;  // counts only rendered passes → preserves read≠write alternation
        for (const ScreenSpaceEffect& effect : effects) {
            if (!effectRenderable(effect)) continue;  // invalid Custom under WarnAndResolve → skip
            SDL_GPUTexture* writeTex = scratch[applied % 2];

            // runEffect carries the built-in (displace_) vs Custom (customReplace_) dispatch the inline
            // pass used to do here; blend=false + blankTransparent=false = the frame-level replace. With
            // a region (ENG-2.F), the effect lands full-frame in layerScratch_ (free during the frame-
            // level chain) and the gate selects inside?eff:readTex into writeTex. Empty region → the
            // single replace pass, byte-identical to the pre-ENG-2.F chain.
            if (effect.region.hasRegion()) {
                runEffect(layerScratch_, readTex, effect,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
                runRegionSelect(writeTex, layerScratch_, readTex, effect.region,
                                /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            } else {
                runEffect(writeTex, readTex, effect,
                          /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
            }
            blitSource = writeTex;
            readTex    = writeTex;
            ++applied;
        }
    }

    // ── Blit pass (ENG-2.B.1, unchanged): viewport → swapchain, integer-scaled + letterboxed. ──
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 width  = 0;
    Uint32 height = 0;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, &width, &height) &&
        swapchain != nullptr) {
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

        // Frame-level post-composite colour transform (ENG-2.B.2.c.2): a default frame resolves
        // to the identity (mul=1, add=0, strength=0) → byte-identical to the pre-c.2 blit.
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
        // empty (the faithful path — byte-identical to C.1).
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
