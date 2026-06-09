#include "gbcpp/sdl_platform.h"

#include <stdexcept>
#include <string>

#include "gbcpp/input_map.h"

namespace gbcpp {

namespace {
// The bring-up clear colour: a dark slate. ENG-2.A presents nothing else; ENG-2.B's
// compositor renders the real viewport into the frame instead of this clear.
constexpr SDL_FColor kClearColour{0.06f, 0.07f, 0.10f, 1.0f};

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string{what} + ": " + SDL_GetError());
}
}  // namespace

SdlPlatform::SdlPlatform(const Config& config) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fail("SDL_Init failed");
    }

    window_ = SDL_CreateWindow(config.title.c_str(), config.width, config.height, 0);
    if (!window_) {
        SDL_Quit();
        fail("SDL_CreateWindow failed");
    }

    // Created with every shader format the supported backends accept so SDL picks an
    // available backend (Vulkan/SPIRV, D3D12/DXIL, Metal/MSL). ENG-2.A binds no
    // pipeline, but the device still requires a valid format set at creation.
    gpu_ = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
        /*debug_mode=*/false, /*name=*/nullptr);
    if (!gpu_) {
        SDL_DestroyWindow(window_);
        SDL_Quit();
        fail("SDL_CreateGPUDevice failed");
    }

    if (!SDL_ClaimWindowForGPUDevice(gpu_, window_)) {
        SDL_DestroyGPUDevice(gpu_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
        fail("SDL_ClaimWindowForGPUDevice failed");
    }

    // Pace presentation to the display refresh. VSYNC is SDL_GPU's default and is
    // guaranteed supported, but it is set explicitly because the host loop has no
    // sleep of its own — SDL's present block IS the frame pacer. Without vsync, the
    // pump → advance → present loop would busy-spin a core. A configurable present
    // mode (uncapped / mailbox) is a later concern; the faithful default is vsync.
    SDL_SetGPUSwapchainParameters(gpu_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  SDL_GPU_PRESENTMODE_VSYNC);
}

SdlPlatform::~SdlPlatform() {
    // Reverse construction order.
    if (gamepad_) SDL_CloseGamepad(gamepad_);
    if (gpu_ && window_) SDL_ReleaseWindowFromGPUDevice(gpu_, window_);
    if (gpu_) SDL_DestroyGPUDevice(gpu_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void SdlPlatform::openGamepad(SDL_JoystickID id) {
    if (gamepad_) return;  // track the first pad only
    gamepad_ = SDL_OpenGamepad(id);
    if (gamepad_) {
        // Detect the family so prompts/glyphs and per-family defaults can adapt; SDL
        // has already normalised the button layout, so input itself needs no per-type
        // handling.
        controllerType_ = controllerTypeFrom(SDL_GetGamepadType(gamepad_));
    }
}

void SdlPlatform::closeGamepad(SDL_JoystickID id) {
    if (gamepad_ && SDL_GetGamepadID(gamepad_) == id) {
        SDL_CloseGamepad(gamepad_);
        gamepad_ = nullptr;
        controllerType_ = ControllerType::Unknown;
    }
}

ButtonSet SdlPlatform::sampleDevices() const {
    ButtonSet held;

    // Read each button's currently-bound physical input. Reading through bindings_
    // (not the fixed default tables) is what makes the controls configurable.
    const bool* keys = SDL_GetKeyboardState(nullptr);
    for (int i = 0; i < kButtonCount; ++i) {
        const auto button = static_cast<Button>(i);
        if (keys && keys[bindings_.keyFor(button)]) held.set(button, true);
        if (gamepad_ && SDL_GetGamepadButton(gamepad_, bindings_.gamepadButtonFor(button))) {
            held.set(button, true);
        }
    }

    return held;
}

void SdlPlatform::pumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                quit_ = true;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                openGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                closeGamepad(event.gdevice.which);
                break;
            default:
                break;
        }
    }
    buttons_ = sampleDevices();
}

void SdlPlatform::presentClearFrame() {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_);
    if (!cmd) return;

    SDL_GPUTexture* swapchain = nullptr;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, nullptr, nullptr) &&
        swapchain) {
        SDL_GPUColorTargetInfo target{};
        target.texture     = swapchain;
        target.clear_color = kClearColour;
        target.load_op     = SDL_GPU_LOADOP_CLEAR;
        target.store_op    = SDL_GPU_STOREOP_STORE;

        // No pipeline bound: the clear load-op is the entire frame.
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        SDL_EndGPURenderPass(pass);
    }

    // Submit even when no swapchain texture was available (e.g. minimised window), so
    // the command buffer is never leaked.
    SDL_SubmitGPUCommandBuffer(cmd);
}

}  // namespace gbcpp
