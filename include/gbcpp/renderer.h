#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <SDL3/SDL.h>

#include "gbcpp/draw_state.h"
#include "gbcpp/output.h"
#include "gbcpp/palette.h"
#include "gbcpp/shader_format.h"
#include "gbcpp/viewport.h"

namespace gbcpp {

// The GPU renderer: owns the offscreen internal viewport (an SDL_GPU colour target the game
// draws into), the tile-compositing pipeline + indexed-atlas/tilemap/palette textures that
// fill it from submitted draw state, the blit pipeline + sampler that scale it onto the
// swapchain, and the per-frame submission. It is constructed from a live device + window
// (handed out by SdlPlatform) — drawing is the renderer's job, the platform owns
// window/device/input.
//
// Colour model (ENG-2.B.2.b): the atlas is INDEXED (one palette index per pixel); palettes
// are a separate amortized resource (uploadPalette → a row of a renderer-owned palette store);
// each tile cell selects a palette within its layer's set; the tile shader applies the colour
// per-pixel. This is the faithful GB/C model — colour = index + selected palette at render
// time — not a baked-RGBA atlas and not a hardware palette-RAM poke.
//
// renderFrame() composites the submitted FrameDrawState into the offscreen viewport (z-
// sorted, alpha-blended; INDEXED TILES layers — ENG-2.B.2.b — and SPRITES layers — ENG-2.B.2.c.1,
// instanced per-sprite quads with colour-index-0 transparency — interleave by z; frame-level
// modifiers are ENG-2.B.2.c.2) and then blits it integer-scaled + letterboxed onto the swapchain
// (the ENG-2.B.1 path, unchanged). Atlas index data and palette colour data are each uploaded
// once (amortized); the draw state references them by handle and is rebuilt fresh each frame.
//
// Like SdlPlatform, the renderer is exercised by the window demo and runtime-verified on a
// dev machine — opening a GPU device/swapchain needs a display the headless CI runners lack
// — while its device-free math (draw order, tilemap coordinates, shader-format selection) is
// unit-tested.
class Renderer {
public:
    // Creates the offscreen viewport target, the tile + blit pipelines (selecting the
    // bytecode format the device accepts), and a nearest sampler. Throws std::runtime_error
    // on any GPU resource-creation failure. The window must already be claimed for the
    // device (SdlPlatform does this at construction).
    Renderer(SDL_GPUDevice* device, SDL_Window* window, ViewportResolution viewport = {});
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Upload an INDEXED tile/sprite atlas once (amortized — at load time / tileset swap),
    // returning a handle the draw state references. ONE palette index per pixel (R8), tightly
    // packed, row-major — NOT RGBA; colour comes from a palette at render time. The renderer
    // owns the GPU texture; the handle stays valid until the renderer is destroyed (no eviction
    // here). Throws std::runtime_error on a GPU failure.
    //
    // `transparentIndex` is this source's per-source indexed transparency policy (ENG-2.B.3.a):
    // a TILES layer drawing from this atlas renders that colour index as a HOLE (discarded,
    // revealing whatever is behind it). The default −1 declares NO transparent index → every
    // index is opaque, byte-identical to the pre-B.3.a faithful tile output. (The sprite path
    // keeps its own hardwired index-0 OBJ transparency — unifying the two is ENG-2.B.3.b.)
    AtlasId uploadAtlas(const std::uint8_t* indices, int width, int height, int transparentIndex = -1);

    // Upload one palette's colours once (amortized — on change), returning the handle the draw
    // state's palette set references. Arbitrary entry count (the span length); written into one
    // row of the renderer-owned palette store. Entries beyond the store width throw; the store
    // is created lazily on the first call. Valid until the renderer is destroyed (no eviction).
    // Throws std::runtime_error on a GPU failure or an over-wide palette.
    PaletteId uploadPalette(std::span<const Rgba8> colors);

    // Register a game-authored custom shader stage (ENG-2.C.3 / Issue 5), returning a handle the
    // draw state references via ScreenSpaceEffect{ .kind = Custom, .customShader = <handle> } at
    // EITHER scope — per-layer (DrawLayer::effect) or frame-level (postEffects). A custom shader is a
    // first-class effect kind, composing with the built-in effects in the same machinery.
    //
    // `fragment` is the per-platform bytecode the game produced from its OWN HLSL through the engine's
    // build-time generator (shaders/gen_shader.cmake — the same path the engine uses internally; see
    // the exposed gbcpp_generate_shader CMake function). The game supplies a FRAGMENT only — the
    // engine's shared fullscreen-triangle postprocess.vert is the vertex stage. The fragment's
    // resource contract mirrors the built-in displacement stage exactly: one sampled source texture +
    // sampler (t0/s0, space2) and, when uniformSize > 0, one uniform cbuffer (b0, space3) the game
    // fills per frame via ScreenSpaceEffect::uniform.
    //
    // `uniformSize` is the byte size of that cbuffer (0 = no uniform). It must be 0 or a positive
    // multiple of 16 (SDL_GPU register packing) — throws std::invalid_argument otherwise. Builds two
    // pipelines once (a no-blend replace pipeline for frame-level / Below scope, a premultiplied-over
    // blend pipeline for Layer scope), mirroring displace_/displaceBlend_. Handles stay valid until
    // the renderer is destroyed (no eviction / unregister). Throws std::runtime_error on a GPU
    // pipeline-creation failure.
    PostProcessStageId registerPostProcessStage(const ShaderVariants& fragment, std::uint32_t uniformSize);

    // One frame: composite the submitted INDEXED TILES + SPRITES layers into the offscreen
    // viewport (z-sorted, alpha-blended; per-tile palette-select + flip applied in-shader from
    // the layer's palette set), run the frame-level post-process chain (frame.postEffects —
    // ENG-2.C.2.a row-displacement; an empty chain is a no-op), then blit the result integer-
    // scaled + letterboxed to the swapchain with the frame-level colour transform (the ENG-2.B.1
    // path, unchanged). `alpha` is the ENG-1 interpolation factor. Throws std::invalid_argument
    // on a layer-key collision when the collision policy is Throw (the default in debug builds;
    // see setLayerCollisionPolicy).
    void renderFrame(const FrameDrawState& frame, float alpha);

    // The runtime reaction when a frame submits colliding layer keys (duplicate z or id).
    // Defaults to kDefaultCollisionPolicy (Throw in debug, WarnAndResolve in release); a host
    // can override it (e.g. force Throw in a soak test, or WarnAndResolve in a kiosk build).
    void setLayerCollisionPolicy(LayerKeyCollisionPolicy policy) noexcept { collisionPolicy_ = policy; }
    [[nodiscard]] LayerKeyCollisionPolicy layerCollisionPolicy() const noexcept { return collisionPolicy_; }

    // Blit sampling, runtime-dynamic (ENG-2.C.1). The default (Nearest) reproduces the faithful
    // baseline value-for-value; the consumer reads config.enhancements.sampling and calls this.
    // Nearest = crisp integer pixels; Bilinear = smoothed upscale. Both samplers are created at
    // construction; the blit binds the one this selects. The viewport always fills the window at the
    // largest integer scale that fits (integerScaleToFitRect) — output SIZE is the window's size,
    // owned by the platform (Platform::setWindowSize), not a renderer mode.
    void setSamplingMode(SamplingMode mode) noexcept { sampling_ = mode; }
    [[nodiscard]] SamplingMode samplingMode() const noexcept { return sampling_; }

private:
    // An uploaded indexed atlas: the GPU texture (R8_UINT) + its pixel dimensions (for tile-
    // grid addressing) + its per-source transparent colour index (−1 = none; ENG-2.B.3.a).
    struct Atlas { SDL_GPUTexture* texture = nullptr; int width = 0; int height = 0; int transparentIndex = -1; };
    // A per-layer tilemap cell texture (R32_UINT, packTileCell'd), recreated when its tile
    // dimensions change.
    struct TilemapTex { SDL_GPUTexture* texture = nullptr; int widthInTiles = 0; int heightInTiles = 0; };
    // A per-layer sprite storage buffer (GpuSprite records). Grow-only: recreated only when a
    // frame's sprite count exceeds its capacity (in sprites), reused across frames otherwise.
    struct SpriteBuf { SDL_GPUBuffer* buffer = nullptr; int capacity = 0; };

    void releaseAtlases();
    void releaseTilemaps();
    void releaseSpriteBuffers();
    void releaseCustomStages();

    SDL_GPUDevice*           device_;
    SDL_Window*              window_;
    ViewportResolution       viewport_;
    SDL_GPUTexture*          target_       = nullptr;  // offscreen viewport colour target
    SDL_GPUTexture*          post0_        = nullptr;  // post-process scratch A (viewport-sized)
    SDL_GPUTexture*          post1_        = nullptr;  // post-process scratch B (ping-ponged with A)
    SDL_GPUTexture*          layerScratch_ = nullptr;  // per-layer effect scratch (ENG-2.C.2.b); swapped with target_ for Below
    SDL_GPUGraphicsPipeline* tile_         = nullptr;  // indexed tilemap → atlas → palette compositor
    SDL_GPUGraphicsPipeline* sprite_       = nullptr;  // instanced per-sprite-quad → atlas → palette
    SDL_GPUGraphicsPipeline* displace_     = nullptr;  // row-displacement post-process stage (ENG-2.C.2.a)
    SDL_GPUGraphicsPipeline* displaceBlend_ = nullptr; // displace + premultiplied-over composite (ENG-2.C.2.b Layer scope)
    SDL_GPUGraphicsPipeline* blit_         = nullptr;  // viewport → swapchain blit pipeline
    SDL_GPUSampler*          sampler_      = nullptr;  // nearest, clamped (tile atlas + faithful blit)
    SDL_GPUSampler*          bilinear_     = nullptr;  // linear, clamped (blit only; SamplingMode::Bilinear)
    SDL_GPUTexture*          paletteStore_ = nullptr;  // RGBA8 store, one row per PaletteId
    std::vector<Atlas>       atlases_;                 // indexed by AtlasId
    std::vector<TilemapTex>  tilemaps_;                // indexed by frame.layers position
    std::vector<SpriteBuf>   spriteBufs_;              // indexed by frame.layers position
    std::vector<Rgba8>       paletteRows_;             // CPU mirror of the store; one fixed-width row per PaletteId
    // Registered custom shader stages (ENG-2.C.3), indexed by PostProcessStageId. Each stage builds a
    // pipeline PAIR from the game's fragment — replace (frame-level / Below scope) + premultiplied-over
    // blend (Layer scope) — mirroring displace_/displaceBlend_; the uniform size is validated per pass.
    std::vector<SDL_GPUGraphicsPipeline*> customReplace_;       // no-blend; one per registered stage
    std::vector<SDL_GPUGraphicsPipeline*> customBlend_;         // premultiplied-over; one per registered stage
    std::vector<std::uint32_t>            customUniformSizes_;  // declared cbuffer size per registered stage
    LayerKeyCollisionPolicy  collisionPolicy_ = kDefaultCollisionPolicy;
    SamplingMode             sampling_     = SamplingMode::Nearest;  // blit sampler (faithful default)
};

}  // namespace gbcpp
