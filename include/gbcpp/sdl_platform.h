#pragma once

#include <SDL3/SDL.h>

#include "gbcpp/engine_config.h"
#include "gbcpp/input.h"
#include "gbcpp/input_map.h"
#include "gbcpp/platform.h"

namespace gbcpp {

// The production Platform: owns an SDL_Window, an SDL_GPUDevice, and the swapchain
// association. The constructor initialises SDL (video + gamepad), creates the window
// (from the EngineConfig's WindowConfig), acquires the GPU device, and claims the window
// for it; the destructor releases them in reverse order. Single-threaded — every call
// runs on the platform thread.
//
// It owns the window, device, and input — not the drawing. Drawing is the Renderer's
// job: it takes device()/window() and submits frames. The swapchain stays sized to the
// window, so drawableSize() reports the current physical size each frame for letterboxing.
//
// The active InputProfile (from the config) masks the sampled input: the platform only
// ever reports the buttons that profile exposes (a Game Boy profile never reports X/Y/L/R).
class SdlPlatform : public Platform {
public:
    // The canonical startup constructor: window from config.window, active controller
    // profile from config.inputProfile. The default argument reproduces the faithful
    // Game Boy Color baseline (default window size, Game Boy button profile).
    explicit SdlPlatform(const EngineConfig& config = {});
    ~SdlPlatform() override;

    SdlPlatform(const SdlPlatform&)            = delete;
    SdlPlatform& operator=(const SdlPlatform&) = delete;

    void pumpEvents() override;
    [[nodiscard]] bool quitRequested() const override { return quit_; }
    [[nodiscard]] ButtonSet buttons() const override { return buttons_; }
    [[nodiscard]] PixelSize drawableSize() const override;

    // The live GPU device + window the renderer draws with. Exposed (rather than hidden
    // behind a present method) because the renderer is a separate object that owns the
    // pipeline/viewport and submits frames against this device — Issue 2's open-internals
    // posture. SDL types appear here by design (this is the SDL platform).
    [[nodiscard]] SDL_GPUDevice* device() const noexcept { return gpu_; }
    [[nodiscard]] SDL_Window*    window() const noexcept { return window_; }

    // The live, rebindable controls. ENG-5 swaps in a profile loaded from config or
    // edited in a rebinding UI; the input path reads whatever is set here. Calling this
    // marks the bindings as customized, which suppresses the on-connect auto-apply of
    // per-family gamepad defaults (so a user/host rebind is never clobbered by plugging
    // in a different pad).
    [[nodiscard]] const ControlBindings& bindings() const noexcept { return bindings_; }
    void setBindings(const ControlBindings& bindings) noexcept {
        bindings_ = bindings;
        bindingsCustomized_ = true;
    }

    // The active controller profile — the platform masks its sampled input by this, so
    // only the profile's buttons are ever reported. Runtime-settable (a game may switch
    // profiles); seeded from the EngineConfig at construction.
    [[nodiscard]] const InputProfile& activeProfile() const noexcept { return activeProfile_; }
    void setActiveProfile(const InputProfile& profile) noexcept { activeProfile_ = profile; }

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
    InputProfile    activeProfile_ = InputProfile::GameBoy;  // sampled input is masked by this
    bool           bindingsCustomized_ = false;  // suppresses on-connect family-default auto-apply
    bool           quit_    = false;
};

}  // namespace gbcpp
