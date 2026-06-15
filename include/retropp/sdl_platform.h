#pragma once

#include <SDL3/SDL.h>

#include "retropp/audio.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/input_map.h"
#include "retropp/platform.h"

namespace retropp {

// The production AudioSink: an SDL audio stream on the default playback device. start() opens the
// stream at the requested rate and begins draining the supplied pull on SDL's audio thread (the
// stream callback); stop() destroys it. Stereo signed-16 PCM (matching AudioFrame).
//
// Freely constructible — make one per AudioSystem (an AudioSystem drives one of these). Multiple
// sinks open multiple device streams that the OS mixes, so several AudioSystems (chiptune, PCM, …)
// play at once. Requires SDL audio initialised — SdlPlatform does that in its constructor (SDL_INIT_
// AUDIO); a game has a platform, so this is satisfied by the time it builds an AudioSystem.
class SdlAudioSink final : public AudioSink {
public:
    SdlAudioSink() = default;
    ~SdlAudioSink() override;

    SdlAudioSink(const SdlAudioSink&)            = delete;
    SdlAudioSink& operator=(const SdlAudioSink&) = delete;

    void start(unsigned rate, int channels, AudioPullFn pull) override;
    void stop() override;

private:
    // SDL's audio-thread callback: pull frames, silence-fill underflow, feed the stream.
    static void SDLCALL audioCallback(void* userdata, SDL_AudioStream* stream,
                                      int additionalAmount, int totalAmount);

    SDL_AudioStream* stream_ = nullptr;
    AudioPullFn      pull_;
};

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
    // profile from config.inputProfile. The default argument reads EngineConfig::active —
    // the set-once active config (seeded by EngineConfig::setActive(); see engine_config.h) —
    // so a bare `SdlPlatform platform;` inherits the host's configured window + input profile.
    // The default arg is evaluated at each call, so it reflects the current `active`. With no
    // setActive() call `active` is the faithful Game Boy Color baseline. SdlPlatform takes the
    // WHOLE config (window title + inputProfile), so it reads `active` rather than a fanned field.
    explicit SdlPlatform(const EngineConfig& config = EngineConfig::active);
    ~SdlPlatform() override;

    SdlPlatform(const SdlPlatform&)            = delete;
    SdlPlatform& operator=(const SdlPlatform&) = delete;

    void pumpEvents() override;
    [[nodiscard]] bool quitRequested() const override { return quit_; }
    [[nodiscard]] ButtonSet buttons() const override { return buttons_; }
    [[nodiscard]] PixelSize drawableSize() const override;

    // Resize the window to `size` logical points (SDL_SetWindowSize) and query the window's display's
    // usable area in logical points (SDL_GetDisplayUsableBounds) — together they let the output
    // scaling pick the largest window scale that fits the screen. See the Platform seam for units.
    void setWindowSize(PixelSize size) override;
    [[nodiscard]] PixelSize usableDisplaySize() const override;

    // Native fullscreen via SDL_SetWindowFullscreen with a NULL fullscreen-mode — SDL3's
    // borderless desktop fullscreen, which on macOS is a real fullscreen Space (the
    // platform-native idiom, not a fake borderless window). Does not make the window freely
    // resizable; the renderer's letterbox/integer-scale blit absorbs the new target size.
    void setFullscreen(bool enabled) override;
    [[nodiscard]] bool isFullscreen() const override { return fullscreen_; }

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
    bool           fullscreen_ = false;  // current fullscreen state (seeded from config at construction)
    bool           quit_    = false;
};

}  // namespace retropp
