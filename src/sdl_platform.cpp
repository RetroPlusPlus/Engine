#include "gbcpp/sdl_platform.h"

#include <stdexcept>
#include <string>

#include "gbcpp/input_map.h"

namespace gbcpp {

namespace {
[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string{what} + ": " + SDL_GetError());
}
}  // namespace

SdlPlatform::SdlPlatform(const EngineConfig& config) : activeProfile_(config.inputProfile) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fail("SDL_Init failed");
    }

    window_ = SDL_CreateWindow(config.window.title.c_str(), config.window.width,
                               config.window.height, 0);
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
        // Apply the family's default mapping (e.g. the Nintendo A/B face-button flip so a
        // Switch player's labelled A confirms). Suppressed once the host/user has rebound —
        // a custom binding is never clobbered by plugging in a pad. The assignment is direct
        // (not via setBindings) so it does NOT itself mark the bindings customized.
        if (!bindingsCustomized_) {
            bindings_ = ControlBindings::defaultsForGamepad(controllerType_);
        }
    }
}

void SdlPlatform::closeGamepad(SDL_JoystickID id) {
    if (gamepad_ && SDL_GetGamepadID(gamepad_) == id) {
        SDL_CloseGamepad(gamepad_);
        gamepad_ = nullptr;
        controllerType_ = ControllerType::Unknown;
        // Revert the gamepad half to the positional defaults when the family-specific pad
        // leaves (the keyboard half is family-independent, so unaffected). Skipped if the
        // bindings were customized — those stay as the host/user set them.
        if (!bindingsCustomized_) {
            bindings_ = ControlBindings::defaults();
        }
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

    // Report only the buttons the active profile exposes (a Game Boy profile never reports
    // X/Y/L/R even from a pad that has them).
    return activeProfile_.mask(held);
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

PixelSize SdlPlatform::drawableSize() const {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return PixelSize{width, height};
}

}  // namespace gbcpp
