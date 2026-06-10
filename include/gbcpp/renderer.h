#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <SDL3/SDL.h>

#include "gbcpp/draw_state.h"
#include "gbcpp/palette.h"
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
// sorted, alpha-blended INDEXED TILES layers — ENG-2.B.2.b; SPRITES + frame modifiers are
// ENG-2.B.2.c) and then blits it integer-scaled + letterboxed onto the swapchain (the
// ENG-2.B.1 path, unchanged). Atlas index data and palette colour data are each uploaded once
// (amortized); the draw state references them by handle and is rebuilt fresh each frame.
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
    AtlasId uploadAtlas(const std::uint8_t* indices, int width, int height);

    // Upload one palette's colours once (amortized — on change), returning the handle the draw
    // state's palette set references. Arbitrary entry count (the span length); written into one
    // row of the renderer-owned palette store. Entries beyond the store width throw; the store
    // is created lazily on the first call. Valid until the renderer is destroyed (no eviction).
    // Throws std::runtime_error on a GPU failure or an over-wide palette.
    PaletteId uploadPalette(std::span<const Rgba8> colors);

    // One frame: composite the submitted INDEXED TILES layers into the offscreen viewport
    // (z-sorted, alpha-blended; per-tile palette-select + flip applied in-shader from the
    // layer's palette set), then blit the viewport integer-scaled + letterboxed to the
    // swapchain (the ENG-2.B.1 path, unchanged). SPRITES layers + frame-level modifiers are
    // ENG-2.B.2.c. `alpha` is the ENG-1 interpolation factor. Throws std::invalid_argument on
    // a layer-key collision when the collision policy is Throw (the default in debug builds;
    // see setLayerCollisionPolicy).
    void renderFrame(const FrameDrawState& frame, float alpha);

    // The runtime reaction when a frame submits colliding layer keys (duplicate z or id).
    // Defaults to kDefaultCollisionPolicy (Throw in debug, WarnAndResolve in release); a host
    // can override it (e.g. force Throw in a soak test, or WarnAndResolve in a kiosk build).
    void setLayerCollisionPolicy(LayerKeyCollisionPolicy policy) noexcept { collisionPolicy_ = policy; }
    [[nodiscard]] LayerKeyCollisionPolicy layerCollisionPolicy() const noexcept { return collisionPolicy_; }

private:
    // An uploaded indexed atlas: the GPU texture (R8_UINT) + its pixel dimensions (for tile-
    // grid addressing).
    struct Atlas { SDL_GPUTexture* texture = nullptr; int width = 0; int height = 0; };
    // A per-layer tilemap cell texture (R32_UINT, packTileCell'd), recreated when its tile
    // dimensions change.
    struct TilemapTex { SDL_GPUTexture* texture = nullptr; int widthInTiles = 0; int heightInTiles = 0; };

    void releaseAtlases();
    void releaseTilemaps();

    SDL_GPUDevice*           device_;
    SDL_Window*              window_;
    ViewportResolution       viewport_;
    SDL_GPUTexture*          target_       = nullptr;  // offscreen viewport colour target
    SDL_GPUGraphicsPipeline* tile_         = nullptr;  // indexed tilemap → atlas → palette compositor
    SDL_GPUGraphicsPipeline* blit_         = nullptr;  // viewport → swapchain blit pipeline
    SDL_GPUSampler*          sampler_      = nullptr;  // nearest, clamped (blit only now)
    SDL_GPUTexture*          paletteStore_ = nullptr;  // RGBA8 store, one row per PaletteId
    std::vector<Atlas>       atlases_;                 // indexed by AtlasId
    std::vector<TilemapTex>  tilemaps_;                // indexed by frame.layers position
    std::vector<Rgba8>       paletteRows_;             // CPU mirror of the store; one fixed-width row per PaletteId
    LayerKeyCollisionPolicy  collisionPolicy_ = kDefaultCollisionPolicy;
};

}  // namespace gbcpp
