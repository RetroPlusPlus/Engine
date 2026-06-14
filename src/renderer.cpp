#include "gbcpp/renderer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <variant>

#include "gbcpp/geometry.h"
#include "gbcpp/postprocess.h"
#include "gbcpp/shader_format.h"
#include "shaders/generated/blit_frag.h"
#include "shaders/generated/blit_vert.h"
#include "shaders/generated/displace_frag.h"
#include "shaders/generated/postprocess_vert.h"
#include "shaders/generated/sprite_frag.h"
#include "shaders/generated/sprite_vert.h"
#include "shaders/generated/tile_frag.h"
#include "shaders/generated/tile_vert.h"

namespace gbcpp {

namespace {

// The backdrop the viewport pass clears to before compositing (opaque black, behind every
// layer) and the letterbox bars around the scaled viewport on the swapchain.
constexpr SDL_FColor kBackdropClear{0.0f, 0.0f, 0.0f, 1.0f};
constexpr SDL_FColor kLetterboxClear{0.0f, 0.0f, 0.0f, 1.0f};

// The Game Boy tile edge length. The atlas grid and tilemap addressing are in these units.
constexpr int kTilePx = 8;

// The palette store's fixed row width, in colours. Covers every PaletteSize preset (max 16)
// with generous, backend-safe headroom; rows are zero-padded out to this width. The store's
// height (row count) grows with each uploadPalette.
constexpr int kPaletteStoreWidth = 256;

// Per-layer uniform block — must match tile.frag.hlsl's TileUniforms cbuffer byte-for-byte
// (std140-style 16-byte-register packing; no member straddles a 16-byte boundary). The
// trailing setRows[16] maps a TileCell::palette (0..15) → a palette-store row; it lays out
// as 64 contiguous bytes, identical to the shader's `uint4 uSetRows[4]` (4 × 16 B registers).
struct TileUniforms {
    float scrollX, scrollY;      // register 0
    float layerW, layerH;
    float tilemapW, tilemapH;    // register 1
    float atlasCols, atlasRows;
    float tilePx;                // register 2
    float alpha;
    float transparentIndex;      // per-source index-hole transparency; <0 = none (ENG-2.B.3.a)
    float pad1;
    std::uint32_t setRows[kPaletteSetSlots];  // registers 3..6 (uint4 ×4 in HLSL)
    float invRow0[4];            // ENG-2.D.1: inverse transform homography, rows 0..2 (registers 7..9)
    float invRow1[4];
    float invRow2[4];
    std::uint32_t hasTransform;  // register 10: x = hasTransform (0/1)
    std::uint32_t transformEdge; //              y = footprint edge (0 Blank / 1 Stretch)
    std::uint32_t pad2, pad3;
};
static_assert(sizeof(TileUniforms) == 176, "TileUniforms must match the HLSL cbuffer layout");
static_assert(kPaletteSetSlots == 16, "setRows packs as uint4[4]; the shader assumes K=16");

// The sprite vertex stage carries NO uniform buffer: the screen→clip transform is baked CPU-side
// into each GpuSprite (gbcpp::makeGpuSprite), so the vertex stage is a pure storage-buffer read.
// This sidesteps a Metal [[buffer]]-namespace collision a storage+uniform vertex stage would hit
// under the single-pass shader toolchain (see PLAN Amendment A2).

// Sprite fragment uniform — must match sprite.frag.hlsl's SpriteFragUniforms cbuffer (one
// 16-byte register).
struct SpriteFragUniforms {
    float atlasCols;  // atlas width in tiles
    float tilePx;     // tile edge length, pixels
    float alpha;      // layer alpha, [0,1]
    float pad;
};
static_assert(sizeof(SpriteFragUniforms) == 16, "SpriteFragUniforms must match the HLSL cbuffer");

// Blit fragment uniform — the frame-level post-composite colour transform (ENG-2.B.2.c.2).
// Must match blit.frag.hlsl's BlitUniforms cbuffer byte-for-byte (three 16-byte registers:
// float3 + pad each). Filled from gbcpp::frameColorTransform(globalModifier, blend); the identity
// (mul=1, add=0, strength=0) reproduces the faithful baseline blit value-for-value.
struct BlitFragUniforms {
    float mulR, mulG, mulB, pad0;                 // register 0
    float addR, addG, addB, pad1;                 // register 1
    float flashR, flashG, flashB, flashStrength;  // register 2
};
static_assert(sizeof(BlitFragUniforms) == 48, "BlitFragUniforms must match the blit.frag cbuffer");

// Row-displacement stage uniform (ENG-2.C.2.a) — must match displace.frag.hlsl's DisplaceUniforms
// cbuffer byte-for-byte (two 16-byte registers). Filled from gbcpp::displaceParams(effect, viewport);
// the layout mirrors DisplaceParams's fields, with the axis carried as a uint.
struct DisplaceFragUniforms {
    float         amplitude, frequency, phase;  // register 0
    std::uint32_t axis;                         //   (0 = Horizontal, 1 = Vertical)
    float         invViewportW, invViewportH;
    std::uint32_t edge;                         //   (0 = Blank, 1 = Stretch)
    std::uint32_t blankTransparent;             //   (0 = opaque backdrop, 1 = transparent) — register 1
};
static_assert(sizeof(DisplaceFragUniforms) == 32, "DisplaceFragUniforms must match the displace.frag cbuffer");

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string{what} + ": " + SDL_GetError());
}

ShaderVariants blitVertVariants() {
    using namespace shaders::blit_vert;
    return ShaderVariants{{kSpirv, sizeof(kSpirv), kSpirvEntrypoint},
                          {kDxil, sizeof(kDxil), kDxilEntrypoint},
                          {kMsl, sizeof(kMsl), kMslEntrypoint}};
}

ShaderVariants blitFragVariants() {
    using namespace shaders::blit_frag;
    return ShaderVariants{{kSpirv, sizeof(kSpirv), kSpirvEntrypoint},
                          {kDxil, sizeof(kDxil), kDxilEntrypoint},
                          {kMsl, sizeof(kMsl), kMslEntrypoint}};
}

ShaderVariants tileVertVariants() {
    using namespace shaders::tile_vert;
    return ShaderVariants{{kSpirv, sizeof(kSpirv), kSpirvEntrypoint},
                          {kDxil, sizeof(kDxil), kDxilEntrypoint},
                          {kMsl, sizeof(kMsl), kMslEntrypoint}};
}

ShaderVariants tileFragVariants() {
    using namespace shaders::tile_frag;
    return ShaderVariants{{kSpirv, sizeof(kSpirv), kSpirvEntrypoint},
                          {kDxil, sizeof(kDxil), kDxilEntrypoint},
                          {kMsl, sizeof(kMsl), kMslEntrypoint}};
}

ShaderVariants spriteVertVariants() {
    using namespace shaders::sprite_vert;
    return ShaderVariants{{kSpirv, sizeof(kSpirv), kSpirvEntrypoint},
                          {kDxil, sizeof(kDxil), kDxilEntrypoint},
                          {kMsl, sizeof(kMsl), kMslEntrypoint}};
}

ShaderVariants spriteFragVariants() {
    using namespace shaders::sprite_frag;
    return ShaderVariants{{kSpirv, sizeof(kSpirv), kSpirvEntrypoint},
                          {kDxil, sizeof(kDxil), kDxilEntrypoint},
                          {kMsl, sizeof(kMsl), kMslEntrypoint}};
}

ShaderVariants postprocessVertVariants() {
    using namespace shaders::postprocess_vert;
    return ShaderVariants{{kSpirv, sizeof(kSpirv), kSpirvEntrypoint},
                          {kDxil, sizeof(kDxil), kDxilEntrypoint},
                          {kMsl, sizeof(kMsl), kMslEntrypoint}};
}

ShaderVariants displaceFragVariants() {
    using namespace shaders::displace_frag;
    return ShaderVariants{{kSpirv, sizeof(kSpirv), kSpirvEntrypoint},
                          {kDxil, sizeof(kDxil), kDxilEntrypoint},
                          {kMsl, sizeof(kMsl), kMslEntrypoint}};
}

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
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, tileVertVariants(), 0, 0, 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, tileFragVariants(), 0, 3, 1);

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
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, spriteVertVariants(), 0, 0, 0, 1);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, spriteFragVariants(), 0, 2, 1);

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
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, postprocessVertVariants(), 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, displaceFragVariants(), 1, 0, 1);

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
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, postprocessVertVariants(), 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, displaceFragVariants(), 1, 0, 1);

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

    // Blit pipeline: the fragment shader uses one sampled texture (the viewport); the vertex
    // shader needs none. The pipeline's colour target must match the swapchain.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, blitVertVariants(), 0);
        // 1 sampler (the viewport) + 1 uniform buffer (the frame-level colour transform, c.2).
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, blitFragVariants(), 1, 0, 1);

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
    for (Atlas& a : atlases_) {
        if (a.texture) SDL_ReleaseGPUTexture(device_, a.texture);
    }
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
    customUniformSizes_.clear();
}

PostProcessStageId Renderer::registerPostProcessStage(const ShaderVariants& fragment,
                                                      std::uint32_t uniformSize) {
    if (!uniformSizeIsValid(uniformSize)) {
        throw std::invalid_argument(
            "registerPostProcessStage: uniformSize must be 0 or a positive multiple of 16");
    }

    // Build the pipeline pair from the game's fragment + the shared fullscreen-triangle vertex stage.
    // Identical resource contract to the displacement stage: 1 sampled source texture + sampler, and
    // (when uniformSize > 0) 1 uniform cbuffer. Two pipelines, differing only in blend state — the
    // no-blend replace (frame-level / Below scope) and the premultiplied-over blend (Layer scope),
    // exactly mirroring displace_ / displaceBlend_.
    const Uint32 numUniforms = (uniformSize > 0) ? 1u : 0u;

    auto buildPipeline = [&](bool blend) -> SDL_GPUGraphicsPipeline* {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, postprocessVertVariants(), 0);
        SDL_GPUShader* fragShader = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, fragment, 1, 0, numUniforms);

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
    customUniformSizes_.push_back(uniformSize);
    return id;
}

AtlasId Renderer::uploadAtlas(const std::uint8_t* indices, int width, int height, int transparentIndex) {
    if (width <= 0 || height <= 0) fail("uploadAtlas: non-positive dimensions");

    // Indexed atlas: one palette index per pixel (R8_UINT), read in-shader by integer Load —
    // no sampler. Colour is resolved from a palette at render time, not stored here.
    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = SDL_GPU_TEXTUREFORMAT_R8_UINT;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    texInfo.width                = static_cast<Uint32>(width);
    texInfo.height               = static_cast<Uint32>(height);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &texInfo);
    if (!texture) fail("SDL_CreateGPUTexture (atlas) failed");

    const Uint32 bytes = static_cast<Uint32>(width) * static_cast<Uint32>(height);  // 1 B/pixel
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
    if (!transfer) fail("SDL_CreateGPUTransferBuffer (atlas) failed");

    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) fail("SDL_MapGPUTransferBuffer (atlas) failed");
    std::memcpy(mapped, indices, bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) fail("SDL_AcquireGPUCommandBuffer (atlas) failed");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = transfer;
    src.offset          = 0;
    src.pixels_per_row  = static_cast<Uint32>(width);
    src.rows_per_layer  = static_cast<Uint32>(height);
    SDL_GPUTextureRegion dst{};
    dst.texture = texture;
    dst.w       = static_cast<Uint32>(width);
    dst.h       = static_cast<Uint32>(height);
    dst.d       = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);

    atlases_.push_back(Atlas{texture, width, height, transparentIndex});
    return static_cast<AtlasId>(atlases_.size() - 1);
}

PaletteId Renderer::uploadPalette(std::span<const Rgba8> colors) {
    if (colors.size() > static_cast<std::size_t>(kPaletteStoreWidth)) {
        fail("uploadPalette: palette wider than the store");
    }

    // Append the new row to the CPU mirror (zero-padded to the fixed store width), then
    // (re)create the store texture at the new height and re-upload every row. Palette uploads
    // are amortized (load time / on change), so the full re-upload is cheap and keeps the
    // store exactly as tall as the live palette count.
    const PaletteId id = static_cast<PaletteId>(paletteRows_.size() / kPaletteStoreWidth);
    const std::size_t base = paletteRows_.size();
    paletteRows_.resize(base + kPaletteStoreWidth);  // value-init pads with opaque black
    for (std::size_t i = 0; i < colors.size(); ++i) paletteRows_[base + i] = colors[i];

    const int rows = static_cast<int>(paletteRows_.size() / kPaletteStoreWidth);
    if (paletteStore_) SDL_ReleaseGPUTexture(device_, paletteStore_);
    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texInfo.usage                = SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    texInfo.width                = static_cast<Uint32>(kPaletteStoreWidth);
    texInfo.height               = static_cast<Uint32>(rows);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels           = 1;
    texInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    paletteStore_ = SDL_CreateGPUTexture(device_, &texInfo);
    if (!paletteStore_) fail("SDL_CreateGPUTexture (palette store) failed");

    const Uint32 bytes = static_cast<Uint32>(paletteRows_.size()) * static_cast<Uint32>(sizeof(Rgba8));
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
    if (!transfer) fail("SDL_CreateGPUTransferBuffer (palette store) failed");

    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) fail("SDL_MapGPUTransferBuffer (palette store) failed");
    std::memcpy(mapped, paletteRows_.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) fail("SDL_AcquireGPUCommandBuffer (palette store) failed");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = transfer;
    src.offset          = 0;
    src.pixels_per_row  = static_cast<Uint32>(kPaletteStoreWidth);
    src.rows_per_layer  = static_cast<Uint32>(rows);
    SDL_GPUTextureRegion dst{};
    dst.texture = paletteStore_;
    dst.w       = static_cast<Uint32>(kPaletteStoreWidth);
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
            records.push_back(makeGpuSprite(s, spritePaletteRow(sc.palettes, s.palette),
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

        if (contentKind(layer.content) == LayerContentKind::Tiles) {
            const TileContent& tc = std::get<TileContent>(layer.content);
            if (tc.widthInTiles <= 0 || tc.heightInTiles <= 0) return;
            const TilemapTex& slot = tilemaps_[idx];
            if (!slot.texture) return;
            const auto atlasIdx = static_cast<std::size_t>(tc.atlas);
            if (atlasIdx >= atlases_.size()) return;
            if (!paletteStore_) return;  // no palette uploaded → nothing to colour from
            const Atlas& atlas = atlases_[atlasIdx];

            TileUniforms u{};
            u.scrollX   = static_cast<float>(layer.scroll.x);
            u.scrollY   = static_cast<float>(layer.scroll.y);
            u.layerW    = static_cast<float>(viewport_.width);
            u.layerH    = static_cast<float>(viewport_.height);
            u.tilemapW  = static_cast<float>(tc.widthInTiles);
            u.tilemapH  = static_cast<float>(tc.heightInTiles);
            u.atlasCols = static_cast<float>(atlas.width / kTilePx);
            u.atlasRows = static_cast<float>(atlas.height / kTilePx);
            u.tilePx    = static_cast<float>(kTilePx);
            u.alpha     = clampAlpha(layer.alpha);
            // Per-source index-hole transparency (ENG-2.B.3.a): −1.0 (the atlas default) leaves
            // the shader's discard branch untaken → byte-identical faithful opaque output.
            u.transparentIndex = static_cast<float>(atlas.transparentIndex);

            // Map the layer's palette set to store rows for the per-tile palette-select.
            const std::array<std::uint32_t, kPaletteSetSlots> rows = paletteSetRows(tc.palettes);
            std::copy(rows.begin(), rows.end(), u.setRows);

            // ENG-2.D.1 — per-layer transform: upload the INVERSE homography (the fragment maps a
            // destination pixel back to content space, perspective divide included) + the footprint
            // edge mode. Identity ⇒ hasTransform 0 ⇒ the fragment takes the faithful pre-D.1 path
            // byte-for-byte.
            u.hasTransform  = layer.transform.isIdentity() ? 0u : 1u;
            u.transformEdge = static_cast<std::uint32_t>(layer.transformEdge);
            const Transform inv = layer.transform.inverse();
            u.invRow0[0] = inv.m00; u.invRow0[1] = inv.m01; u.invRow0[2] = inv.m02; u.invRow0[3] = 0.0f;
            u.invRow1[0] = inv.m10; u.invRow1[1] = inv.m11; u.invRow1[2] = inv.m12; u.invRow1[3] = 0.0f;
            u.invRow2[0] = inv.m20; u.invRow2[1] = inv.m21; u.invRow2[2] = inv.m22; u.invRow2[3] = 0.0f;

            // The tile path is all integer Load — bind three read-only storage textures
            // (atlas, tilemap cells, palette store) at t0/t1/t2; no sampler.
            SDL_GPUTexture* storageTextures[3] = {atlas.texture, slot.texture, paletteStore_};
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
            if (!paletteStore_) return;  // no palette uploaded → nothing to colour from
            const Atlas& atlas = atlases_[atlasIdx];

            SpriteFragUniforms fu{};
            fu.atlasCols = static_cast<float>(atlas.width / kTilePx);
            fu.tilePx    = static_cast<float>(kTilePx);
            fu.alpha     = clampAlpha(layer.alpha);

            // Instanced per-sprite quads: the vertex stage reads the sprite records (already in
            // clip space) from a storage buffer (t0 space0) — no vertex uniform; the fragment
            // stage reads the indexed atlas + palette store (t0/t1 space2) + its uniform. 6
            // verts × spriteCount instances.
            SDL_GPUTexture* fragStorage[2] = {atlas.texture, paletteStore_};
            SDL_BindGPUGraphicsPipeline(pass, sprite_);
            SDL_BindGPUVertexStorageBuffers(pass, 0, &slot.buffer, 1);
            SDL_BindGPUFragmentStorageTextures(pass, 0, fragStorage, 2);
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(spriteCount), 0, 0);
        }
    };

    // Whether a screen-space effect can be rendered this frame. A built-in (RowDisplacement) always
    // can; a Custom effect (ENG-2.C.3) is renderable only if its handle indexes a registered stage and
    // its uniform byte-count matches that stage's declared size. An invalid Custom pass throws under
    // the Throw collision policy (the debug default — surface a bad registration immediately) and is
    // skipped-with-warning under WarnAndResolve (keep a shipped game up). Shared by the per-layer +
    // frame-level realizations below.
    auto effectRenderable = [&](const ScreenSpaceEffect& effect) -> bool {
        if (!effectUsesCustomShader(effect)) return true;
        const std::size_t count = customReplace_.size();
        const auto id           = static_cast<std::size_t>(effect.customShader);
        const std::uint32_t sz  = id < count ? customUniformSizes_[id] : 0u;
        if (customStagePassValid(effect, count, sz)) return true;
        if (collisionPolicy_ == LayerKeyCollisionPolicy::Throw) {
            throw std::invalid_argument(
                "renderFrame: invalid custom shader stage pass (bad handle or uniform size)");
        }
        SDL_Log("gbcpp: skipping invalid custom shader stage pass (bad handle or uniform size)");
        return false;
    };

    // Run one effect pass: read `source`, write `dest`. `blend` picks the replace pipeline (the opaque
    // accumulator displace for a Below effect / the frame-level chain) or the premultiplied-over
    // composite pipeline (an isolated Layer's effected image back onto target_). The pass is shader-
    // agnostic: a built-in RowDisplacement binds displace_/displaceBlend_ + the resolved DisplaceParams
    // (`blankTransparent` controlling the Blank-edge colour); a Custom effect binds the registered
    // pipeline pair + pushes the game's own uniform bytes. Same scope/compositing/ping-pong plumbing.
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
            if (!effect.uniform.empty()) {
                SDL_PushGPUFragmentUniformData(cmd, 0, effect.uniform.data(),
                                               static_cast<Uint32>(effect.uniform.size()));
            }
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
            // the transformed accumulator. DONT_CARE: the fullscreen pass overwrites every pixel.
            runEffect(layerScratch_, target_, layer.effect,
                      /*blankTransparent=*/false, /*blend=*/false, SDL_GPU_LOADOP_DONT_CARE);
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
        runEffect(target_, layerScratch_, layer.effect,
                  /*blankTransparent=*/true, /*blend=*/true, compositeLoad);
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

            SDL_GPUColorTargetInfo ppTarget{};
            ppTarget.texture  = writeTex;
            ppTarget.load_op  = SDL_GPU_LOADOP_DONT_CARE;  // the fullscreen pass overwrites every pixel
            ppTarget.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ppTarget, 1, nullptr);

            const SDL_GPUTextureSamplerBinding binding{readTex, sampler_};  // nearest, CLAMP_TO_EDGE
            if (effectUsesCustomShader(effect)) {
                const auto id = static_cast<std::size_t>(effect.customShader);
                SDL_BindGPUGraphicsPipeline(pass, customReplace_[id]);  // frame-level = replace (no blend)
                SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
                if (!effect.uniform.empty()) {
                    SDL_PushGPUFragmentUniformData(cmd, 0, effect.uniform.data(),
                                                   static_cast<Uint32>(effect.uniform.size()));
                }
            } else {
                // activeFrameEffects filtered out None; a built-in here is RowDisplacement (an unknown
                // future kind resolves to identity params → no-op).
                const DisplaceParams p = displaceParams(effect, PixelSize{viewport_.width, viewport_.height});
                const DisplaceFragUniforms du{p.amplitude, p.frequency, p.phase, p.axis,
                                              p.invViewportW, p.invViewportH, p.edge, p.blankTransparent};
                SDL_BindGPUGraphicsPipeline(pass, displace_);
                SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
                SDL_PushGPUFragmentUniformData(cmd, 0, &du, sizeof(du));
            }
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle

            SDL_EndGPURenderPass(pass);
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

}  // namespace gbcpp
