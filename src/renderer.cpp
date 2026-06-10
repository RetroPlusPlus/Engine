#include "gbcpp/renderer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <variant>

#include "gbcpp/geometry.h"
#include "gbcpp/shader_format.h"
#include "shaders/generated/blit_frag.h"
#include "shaders/generated/blit_vert.h"
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
    float pad0, pad1;
    std::uint32_t setRows[kPaletteSetSlots];  // registers 3..6 (uint4 ×4 in HLSL)
};
static_assert(sizeof(TileUniforms) == 112, "TileUniforms must match the HLSL cbuffer layout");
static_assert(kPaletteSetSlots == 16, "setRows packs as uint4[4]; the shader assumes K=16");

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

SDL_GPUShader* createShader(SDL_GPUDevice* device, SDL_GPUShaderStage stage,
                            const ShaderVariants& variants, Uint32 numSamplers,
                            Uint32 numStorageTextures = 0, Uint32 numUniformBuffers = 0) {
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

    // Blit pipeline: the fragment shader uses one sampled texture (the viewport); the vertex
    // shader needs none. The pipeline's colour target must match the swapchain.
    {
        SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, blitVertVariants(), 0);
        SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, blitFragVariants(), 1);

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
    releaseTilemaps();
    releaseAtlases();
    if (paletteStore_) SDL_ReleaseGPUTexture(device_, paletteStore_);
    if (blit_)         SDL_ReleaseGPUGraphicsPipeline(device_, blit_);
    if (tile_)         SDL_ReleaseGPUGraphicsPipeline(device_, tile_);
    if (sampler_)      SDL_ReleaseGPUSampler(device_, sampler_);
    if (target_)       SDL_ReleaseGPUTexture(device_, target_);
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

AtlasId Renderer::uploadAtlas(const std::uint8_t* indices, int width, int height) {
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

    atlases_.push_back(Atlas{texture, width, height});
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

    // ── Copy pass: (re)create + upload each TILES layer's tilemap index texture. ──────────
    tilemaps_.resize(frame.layers.size());
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
    if (copy) SDL_EndGPUCopyPass(copy);

    // ── Viewport pass: clear the backdrop, composite TILES layers back-to-front (alpha). ──
    {
        SDL_GPUColorTargetInfo vpTarget{};
        vpTarget.texture     = target_;
        vpTarget.clear_color = kBackdropClear;
        vpTarget.load_op     = SDL_GPU_LOADOP_CLEAR;
        vpTarget.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &vpTarget, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, tile_);

        for (const std::size_t idx : order) {
            const DrawLayer& layer = frame.layers[idx];
            if (contentKind(layer.content) != LayerContentKind::Tiles) continue;  // SPRITES → ENG-2.B.2.b
            const TileContent& tc = std::get<TileContent>(layer.content);
            if (tc.widthInTiles <= 0 || tc.heightInTiles <= 0) continue;
            const TilemapTex& slot = tilemaps_[idx];
            if (!slot.texture) continue;
            const auto atlasIdx = static_cast<std::size_t>(tc.atlas);
            if (atlasIdx >= atlases_.size()) continue;
            if (!paletteStore_) continue;  // no palette uploaded → nothing to colour from
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

            // Map the layer's palette set to store rows for the per-tile palette-select.
            const std::array<std::uint32_t, kPaletteSetSlots> rows = paletteSetRows(tc.palettes);
            std::copy(rows.begin(), rows.end(), u.setRows);

            // The tile path is all integer Load — bind three read-only storage textures (atlas,
            // tilemap cells, palette store) at t0/t1/t2; no sampler.
            SDL_GPUTexture* storageTextures[3] = {atlas.texture, slot.texture, paletteStore_};
            SDL_BindGPUFragmentStorageTextures(pass, 0, storageTextures, 3);
            SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);  // one fullscreen triangle
        }
        SDL_EndGPURenderPass(pass);
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

        SDL_BindGPUGraphicsPipeline(pass, blit_);
        const SDL_GPUTextureSamplerBinding binding{target_, sampler_};
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

}  // namespace gbcpp
