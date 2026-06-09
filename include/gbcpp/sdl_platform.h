#pragma once

#include <cstdint>
#include <string>

#include <SDL3/SDL.h>

#include "gbcpp/input.h"
#include "gbcpp/input_map.h"
#include "gbcpp/platform.h"

namespace gbcpp {

// The production Platform: owns an SDL_Window, an SDL_GPUDevice, and the swapchain
// association. The constructor initialises SDL (video + gamepad), creates the window,
// acquires the GPU device, and claims the window for it; the destructor releases them
// in reverse order. Single-threaded — every call runs on the platform thread.
//
// ENG-2.A draws nothing but a cleared frame: presentClearFrame() acquires the
// swapchain texture, runs a render pass whose load-op is a solid clear colour, and
// presents — binding no graphics pipeline and no shader. The real draw-state API is
// ENG-2.B.
class SdlPlatform : public Platform {
public:
    struct Config {
        std::string  title  = "GBCPP";
        int          width  = 160 * 4;  // a default window size; the internal
        int          height = 144 * 4;  // viewport (ENG-2.B) is separate from this.
    };

    SdlPlatform() : SdlPlatform(Config{}) {}
    explicit SdlPlatform(const Config& config);
    ~SdlPlatform() override;

    SdlPlatform(const SdlPlatform&)            = delete;
    SdlPlatform& operator=(const SdlPlatform&) = delete;

    void pumpEvents() override;
    [[nodiscard]] bool quitRequested() const override { return quit_; }
    [[nodiscard]] ButtonSet buttons() const override { return buttons_; }
    void presentClearFrame() override;

    // The live, rebindable controls. ENG-5 swaps in a profile loaded from config or
    // edited in a rebinding UI; the input path reads whatever is set here.
    [[nodiscard]] const ControlBindings& bindings() const noexcept { return bindings_; }
    void setBindings(const ControlBindings& bindings) noexcept { bindings_ = bindings; }

    // The detected family of the connected pad (Unknown when none) — for button-glyph
    // selection and per-family default profiles. Updated on connect / disconnect.
    [[nodiscard]] ControllerType controllerType() const noexcept { return controllerType_; }

private:
    void openGamepad(SDL_JoystickID id);
    void closeGamepad(SDL_JoystickID id);
    [[nodiscard]] ButtonSet sampleDevices() const;

    SDL_Window*    window_  = nullptr;
    SDL_GPUDevice* gpu_     = nullptr;
    SDL_Gamepad*   gamepad_ = nullptr;  // the first connected pad, if any
    ButtonSet      buttons_;
    ControlBindings bindings_ = ControlBindings::defaults();
    ControllerType  controllerType_ = ControllerType::Unknown;
    bool           quit_    = false;
};

}  // namespace gbcpp
