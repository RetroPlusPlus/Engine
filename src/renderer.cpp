#include "gbcpp/renderer.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "gbcpp/geometry.h"
#include "gbcpp/shader_format.h"
#include "shaders/generated/blit_frag.h"
#include "shaders/generated/blit_vert.h"

namespace gbcpp {

namespace {

// The bring-up viewport fill (a distinct blue) and the letterbox bars (black). The blue
// filling a centred, integer-scaled rect on a black field is the visible proof that the
// offscreen-render → blit → present path works; B.2 replaces the fill with real content.
constexpr SDL_FColor kViewportClear{0.10f, 0.45f, 0.70f, 1.0f};
constexpr SDL_FColor kLetterboxClear{0.0f, 0.0f, 0.0f, 1.0f};

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

SDL_GPUShader* createShader(SDL_GPUDevice* device, SDL_GPUShaderStage stage,
                            const ShaderVariants& variants, Uint32 numSamplers) {
    const auto chosen = selectShader(SDL_GetGPUShaderFormats(device), variants);
    if (!chosen) fail("no compatible shader format for this GPU device");

    SDL_GPUShaderCreateInfo info{};
    info.code_size    = chosen->first.size;
    info.code         = chosen->first.data;
    info.entrypoint   = chosen->first.entrypoint;
    info.format       = chosen->second;
    info.stage        = stage;
    info.num_samplers = numSamplers;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (!shader) fail("SDL_CreateGPUShader failed");
    return shader;
}

}  // namespace

Renderer::Renderer(SDL_GPUDevice* device, SDL_Window* window, ViewportConfig viewport)
    : device_(device), window_(window), viewport_(viewport) {
    // Offscreen viewport target: a colour target the game renders into, and a sampler
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

    // Blit pipeline: the fragment shader uses one sampled texture (the viewport); the
    // vertex shader needs none. The pipeline's colour target must match the swapchain.
    SDL_GPUShader* vertex   = createShader(device_, SDL_GPU_SHADERSTAGE_VERTEX, blitVertVariants(), 0);
    SDL_GPUShader* fragment = createShader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, blitFragVariants(), 1);

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);

    SDL_GPUGraphicsPipelineCreateInfo pipeline{};
    pipeline.vertex_shader                       = vertex;
    pipeline.fragment_shader                     = fragment;
    pipeline.primitive_type                      = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline.rasterizer_state.fill_mode          = SDL_GPU_FILLMODE_FILL;
    pipeline.rasterizer_state.cull_mode          = SDL_GPU_CULLMODE_NONE;
    pipeline.rasterizer_state.front_face         = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline.multisample_state.sample_count      = SDL_GPU_SAMPLECOUNT_1;
    pipeline.target_info.color_target_descriptions = &colorTarget;
    pipeline.target_info.num_color_targets       = 1;
    blit_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

    // The pipeline holds its own references to the shaders; release ours either way.
    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragment);
    if (!blit_) fail("SDL_CreateGPUGraphicsPipeline failed");

    // Nearest filtering, clamped — the faithful baseline (bilinear/CRT are ENG-2.C).
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter     = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter     = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &samplerInfo);
    if (!sampler_) fail("SDL_CreateGPUSampler failed");
}

Renderer::~Renderer() {
    // Reverse creation order.
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
    if (blit_)    SDL_ReleaseGPUGraphicsPipeline(device_, blit_);
    if (target_)  SDL_ReleaseGPUTexture(device_, target_);
}

void Renderer::renderFrame(float /*alpha*/) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return;

    // Pass 1 — fill the offscreen viewport. No pipeline bound: the clear load-op is the
    // whole pass. (B.2 replaces this with the layered compositor.)
    {
        SDL_GPUColorTargetInfo vpTarget{};
        vpTarget.texture     = target_;
        vpTarget.clear_color = kViewportClear;
        vpTarget.load_op     = SDL_GPU_LOADOP_CLEAR;
        vpTarget.store_op    = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &vpTarget, 1, nullptr);
        SDL_EndGPURenderPass(pass);
    }

    // Pass 2 — clear the swapchain to the letterbox colour, then blit the viewport into
    // the integer-scaled, centred destination rect.
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 width = 0;
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

        // The viewport transform maps the fullscreen triangle onto the dest rect; the
        // scissor (clamped to the swapchain) keeps the oversized triangle from bleeding
        // past it into the letterbox bars — needed because the viewport alone does not
        // clip rasterization.
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

    // Submit even with no swapchain texture (e.g. minimised) so the command buffer is
    // never leaked.
    SDL_SubmitGPUCommandBuffer(cmd);
}

}  // namespace gbcpp
