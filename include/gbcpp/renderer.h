#pragma once

#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

#include "gbcpp/draw_state.h"
#include "gbcpp/viewport.h"

namespace gbcpp {

// The GPU renderer: owns the offscreen internal viewport (an SDL_GPU colour target the game
// draws into), the tile-compositing pipeline + atlas/tilemap textures that fill it from
// submitted draw state, the blit pipeline + sampler that scale it onto the swapchain, and
// the per-frame submission. It is constructed from a live device + window (handed out by
// SdlPlatform) — drawing is the renderer's job, the platform owns window/device/input.
//
// renderFrame() composites the submitted FrameDrawState into the offscreen viewport (z-
// sorted, alpha-blended TILES layers — ENG-2.B.2.a; SPRITES + frame modifiers are ENG-2.B.2.b)
// and then blits it integer-scaled + letterboxed onto the swapchain (the ENG-2.B.1 path,
// unchanged). Atlas pixel data is uploaded once via uploadAtlas(); the draw state references
// it by handle and is rebuilt fresh each frame.
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
    Renderer(SDL_GPUDevice* device, SDL_Window* window, ViewportConfig viewport = {});
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Upload tile/sprite pixel data once (amortized — at load time / tileset swap), returning
    // a handle the draw state references. RGBA8, tightly packed, row-major. The renderer owns
    // the GPU texture; the handle stays valid until the renderer is destroyed (no eviction at
    // ENG-2.B.2.a). Throws std::runtime_error on a GPU failure.
    AtlasId uploadAtlas(const std::uint8_t* rgba, int width, int height);

    // One frame: composite the submitted layers into the offscreen viewport (z-sorted, alpha-
    // blended), then blit the viewport integer-scaled + letterboxed to the swapchain (the
    // ENG-2.B.1 path, unchanged). `alpha` is the ENG-1 interpolation factor. Throws
    // std::invalid_argument on a layer-key collision when the collision policy is Throw
    // (the default in debug builds; see setLayerCollisionPolicy).
    void renderFrame(const FrameDrawState& frame, float alpha);

    // The runtime reaction when a frame submits colliding layer keys (duplicate z or id).
    // Defaults to kDefaultCollisionPolicy (Throw in debug, WarnAndResolve in release); a host
    // can override it (e.g. force Throw in a soak test, or WarnAndResolve in a kiosk build).
    void setLayerCollisionPolicy(LayerKeyCollisionPolicy policy) noexcept { collisionPolicy_ = policy; }
    [[nodiscard]] LayerKeyCollisionPolicy layerCollisionPolicy() const noexcept { return collisionPolicy_; }

private:
    // An uploaded atlas: the GPU texture + its pixel dimensions (for tile-grid addressing).
    struct Atlas { SDL_GPUTexture* texture = nullptr; int width = 0; int height = 0; };
    // A per-layer tilemap index texture (R16_UINT), recreated when its tile dimensions change.
    struct TilemapTex { SDL_GPUTexture* texture = nullptr; int widthInTiles = 0; int heightInTiles = 0; };

    void releaseAtlases();
    void releaseTilemaps();

    SDL_GPUDevice*           device_;
    SDL_Window*              window_;
    ViewportConfig           viewport_;
    SDL_GPUTexture*          target_  = nullptr;  // offscreen viewport colour target
    SDL_GPUGraphicsPipeline* tile_    = nullptr;  // tilemap → atlas compositor pipeline
    SDL_GPUGraphicsPipeline* blit_    = nullptr;  // viewport → swapchain blit pipeline
    SDL_GPUSampler*          sampler_ = nullptr;  // nearest, clamped (shared by tile + blit)
    std::vector<Atlas>       atlases_;            // indexed by AtlasId
    std::vector<TilemapTex>  tilemaps_;           // indexed by frame.layers position
    LayerKeyCollisionPolicy  collisionPolicy_ = kDefaultCollisionPolicy;
};

}  // namespace gbcpp
