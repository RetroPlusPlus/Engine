#pragma once

#include <SDL3/SDL.h>

#include "gbcpp/viewport.h"

namespace gbcpp {

// The GPU renderer: owns the offscreen internal viewport (an SDL_GPU colour target the
// game draws into), the blit pipeline + sampler that scale it onto the swapchain, and
// the per-frame submission. It is constructed from a live device + window (handed out by
// SdlPlatform) — drawing is the renderer's job, the platform owns window/device/input.
//
// At ENG-2.B.1 the viewport contents are a fixed bring-up colour: renderFrame() clears
// the offscreen target, then blits it integer-scaled and letterboxed onto the swapchain.
// ENG-2.B.2 replaces the clear with the layered compositor that fills the viewport from
// submitted draw state; the blit + scaling path here is unchanged by that.
//
// Like SdlPlatform, the renderer is exercised by the window demo and runtime-verified on
// a dev machine — opening a GPU device/swapchain needs a display the headless CI runners
// lack — while its device-free math (shader-format selection, blit rect) is unit-tested.
class Renderer {
public:
    // Creates the offscreen viewport target, the blit shaders + pipeline (selecting the
    // bytecode format the device accepts), and a nearest sampler. Throws std::runtime_error
    // on any GPU resource-creation failure. The window must already be claimed for the
    // device (SdlPlatform does this at construction).
    Renderer(SDL_GPUDevice* device, SDL_Window* window, ViewportConfig viewport = {});
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // One frame: fill the viewport with the bring-up colour, then clear the swapchain to
    // the letterbox colour and blit the viewport into the integer-scaled, centred dest
    // rect. `alpha` is the ENG-1 interpolation factor; unused while the viewport contents
    // are static, wired through so the render-callback contract is unchanged for B.2.
    void renderFrame(float alpha);

private:
    SDL_GPUDevice*           device_;
    SDL_Window*              window_;
    ViewportConfig           viewport_;
    SDL_GPUTexture*          target_  = nullptr;  // offscreen viewport colour target
    SDL_GPUGraphicsPipeline* blit_    = nullptr;
    SDL_GPUSampler*          sampler_ = nullptr;
};

}  // namespace gbcpp
