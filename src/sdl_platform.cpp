#include "retropp/sdl_platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "retropp/input_map.h"

namespace retropp {

namespace {
[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string{what} + ": " + SDL_GetError());
}

// Normalise a stick axis (SDL Sint16, [-32768, 32767]) to [-1, 1] with a per-axis dead-zone, rescaled
// so motion begins right at the dead-zone edge (no jump). Y follows SDL: down is positive.
float normStick(Sint16 raw) noexcept {
    constexpr float kDeadZone = 0.15f;
    float v = std::clamp(static_cast<float>(raw) / 32767.0f, -1.0f, 1.0f);
    const float mag = std::abs(v);
    if (mag < kDeadZone) return 0.0f;
    return (v < 0 ? -1.0f : 1.0f) * (mag - kDeadZone) / (1.0f - kDeadZone);
}

// Normalise a trigger axis (SDL Sint16, [0, 32767]) to [0, 1] with a small dead-zone, rescaled.
float normTrigger(Sint16 raw) noexcept {
    constexpr float kDeadZone = 0.06f;
    const float v = std::clamp(static_cast<float>(raw) / 32767.0f, 0.0f, 1.0f);
    if (v < kDeadZone) return 0.0f;
    return (v - kDeadZone) / (1.0f - kDeadZone);
}
}  // namespace

SdlPlatform::SdlPlatform(const EngineConfig& config)
    : viewport_{config.viewport.width, config.viewport.height},
      activeProfile_(config.inputProfile) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fail("SDL_Init failed");
    }

    // Window size = viewport × windowScale in LOGICAL points, clamped down so it fits the primary
    // display's usable area (fitWindowScale) — so the window is always an integer multiple of the
    // game's native resolution and never opens larger than the screen, even for a large viewport.
    // The usable bounds are queried before the window exists, off the primary display; a query
    // failure passes a degenerate {0,0} so fitWindowScale falls back to the unclamped target.
    PixelSize usable{};
    if (const SDL_DisplayID disp = SDL_GetPrimaryDisplay(); disp != 0) {
        if (SDL_Rect bounds{}; SDL_GetDisplayUsableBounds(disp, &bounds)) {
            usable = PixelSize{bounds.w, bounds.h};
        }
    }
    const PixelSize vp{config.viewport.width, config.viewport.height};
    const int scale = fitWindowScale(vp, usable, config.enhancements.windowScale);

    // HIGH_PIXEL_DENSITY: the window's drawable is created at the display's PHYSICAL pixel
    // resolution (on a 2× Retina panel, a 640-logical-point window has a 1280-pixel drawable).
    // drawableSize() reports SDL_GetWindowSizeInPixels (true physical pixels) and the blit fills the
    // drawable at the largest integer scale that fits, so the art renders crisp at native resolution.
    window_ = SDL_CreateWindow(config.window.title.c_str(), vp.width * scale, vp.height * scale,
                               SDL_WINDOW_HIGH_PIXEL_DENSITY);
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

    // Apply the startup fullscreen toggle once. Default (false) leaves the faithful windowed
    // baseline untouched; a host that opts in opens straight into fullscreen.
    if (config.enhancements.fullscreen) {
        setFullscreen(true);
    }
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
    // Relative quantities are per-PUMP: reset, accumulate from this pump's events, and the run loop
    // sums them across pumps between ticks (so motion is never lost on a zero-tick frame).
    frameRawDX_ = 0.0f;
    frameRawDY_ = 0.0f;
    frameWheel_ = 0.0f;

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
            case SDL_EVENT_MOUSE_MOTION:
                // Absolute position (window logical points) for the cursor map; relative motion (raw
                // device delta) accumulated for the spinner. In relative-capture mode SDL keeps the
                // position pinned and reports only the deltas — exactly what a spinner integrates.
                mouseWinX_  = event.motion.x;
                mouseWinY_  = event.motion.y;
                frameRawDX_ += event.motion.xrel;
                frameRawDY_ += event.motion.yrel;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const bool down = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                std::uint8_t bit = 0;
                switch (event.button.button) {
                    case SDL_BUTTON_LEFT:   bit = std::uint8_t{1} << static_cast<int>(MouseButton::Left);   break;
                    case SDL_BUTTON_RIGHT:  bit = std::uint8_t{1} << static_cast<int>(MouseButton::Right);  break;
                    case SDL_BUTTON_MIDDLE: bit = std::uint8_t{1} << static_cast<int>(MouseButton::Middle); break;
                    default: break;
                }
                if (down) mouseHeld_ |= bit;
                else      mouseHeld_ = static_cast<std::uint8_t>(mouseHeld_ & ~bit);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                frameWheel_ += event.wheel.y;
                break;
            default:
                break;
        }
    }
    buttons_ = sampleDevices();
}

AnalogInput SdlPlatform::analog() const {
    AnalogInput a;
    a.rawDeltaX = frameRawDX_;
    a.rawDeltaY = frameRawDY_;
    a.wheel     = frameWheel_;
    a.mouseHeld = mouseHeld_;

    // Map the pointer into viewport space by inverting the renderer's blit. The blit destination is
    // recomputed from the same pure function and inputs the renderer uses (drawableSize() = swapchain
    // size, viewport_), so the mapped coordinate matches what is actually drawn. drawableSize() is
    // PHYSICAL pixels (HIGH_PIXEL_DENSITY window); the mouse event position is LOGICAL points, so it is
    // scaled by the window's pixel density before inversion (the units-pinning the plan called out).
    const PixelSize draw = drawableSize();
    const IntRect blit = integerScaleToFitRect(draw, viewport_);
    const float density = SDL_GetWindowPixelDensity(window_);
    const Vec2i winPx{static_cast<int>(mouseWinX_ * density), static_cast<int>(mouseWinY_ * density)};
    const ViewportHit hit = windowToViewport(winPx, blit, viewport_);
    a.cursor = hit.pos;
    // There is no meaningful absolute cursor while captured (the OS position is pinned); report
    // off-screen so a consumer doesn't draw a stale reticle during a spinner level.
    a.cursorOnScreen = hit.inside && !pointerCaptured_;

    if (gamepad_) {
        a.leftX    = normStick(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX));
        a.leftY    = normStick(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY));
        a.rightX   = normStick(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHTX));
        a.rightY   = normStick(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHTY));
        a.triggerL = normTrigger(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        a.triggerR = normTrigger(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    }
    return a;
}

void SdlPlatform::setPointerCaptured(bool captured) {
    // SDL relative-mouse mode: hides + confines the OS cursor and reports unbounded relative motion.
    // On failure the tracked state stays as it was (the window mode is unchanged).
    if (SDL_SetWindowRelativeMouseMode(window_, captured)) {
        pointerCaptured_ = captured;
    }
}

void SdlPlatform::setCursorVisible(bool visible) {
    // SDL show/hide of the OS cursor — independent of relative-mouse capture (which hides the cursor as
    // a side effect of confining it). Absolute cursor tracking is unaffected: analog() keeps reporting
    // the position. On failure the tracked state is unchanged.
    if (visible ? SDL_ShowCursor() : SDL_HideCursor()) {
        cursorVisible_ = visible;
    }
}

PixelSize SdlPlatform::drawableSize() const {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return PixelSize{width, height};
}

void SdlPlatform::setWindowSize(PixelSize size) {
    // Logical points (SDL window size is logical); the drawable follows at × the display density.
    SDL_SetWindowSize(window_, size.width, size.height);
}

PixelSize SdlPlatform::usableDisplaySize() const {
    SDL_Rect bounds{};
    const SDL_DisplayID disp = SDL_GetDisplayForWindow(window_);
    if (disp != 0 && SDL_GetDisplayUsableBounds(disp, &bounds)) {
        return PixelSize{bounds.w, bounds.h};
    }
    return drawableSize();  // safe fallback when the display can't be queried
}

// ── SdlAudioSink (ENG-4.A) ──────────────────────────────────────────────────────────────────────

SdlAudioSink::~SdlAudioSink() { stop(); }

void SdlAudioSink::audioCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount,
                                 int /*totalAmount*/) {
    auto* self = static_cast<SdlAudioSink*>(userdata);
    if (additionalAmount <= 0 || !self->pull_) {
        return;
    }
    // additionalAmount is bytes; a stereo S16 frame is sizeof(AudioFrame) (4) bytes. Pull in fixed
    // chunks, silence-filling any underflow, and feed each chunk to the stream.
    constexpr int kFrameBytes = static_cast<int>(sizeof(AudioFrame));
    int framesNeeded = additionalAmount / kFrameBytes;
    std::array<AudioFrame, 512> buf{};
    while (framesNeeded > 0) {
        const int n = std::min(framesNeeded, static_cast<int>(buf.size()));
        const std::size_t got = self->pull_(std::span<AudioFrame>(buf.data(), static_cast<std::size_t>(n)));
        for (std::size_t i = got; i < static_cast<std::size_t>(n); ++i) {
            buf[i] = AudioFrame{};  // underflow → silence
        }
        SDL_PutAudioStreamData(stream, buf.data(), n * kFrameBytes);
        framesNeeded -= n;
    }
}

void SdlAudioSink::start(unsigned rate, int channels, AudioPullFn pull) {
    stop();  // idempotent restart
    pull_ = std::move(pull);
    // Request a small device buffer so a freshly-played note reaches the speakers promptly. A note's
    // onset latency is dominated by the device buffer (the ring is empty while idle), so SDL's larger
    // default would make presses feel late; ~256 frames ≈ 5 ms at 48 kHz. The ring still absorbs jitter
    // behind it. (CoreAudio's own output latency remains a floor below this.)
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;  // native-endian signed 16-bit — matches AudioFrame's int16 L/R
    spec.channels = channels;
    spec.freq = static_cast<int>(rate);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &audioCallback, this);
    if (!stream_) {
        pull_ = nullptr;
        fail("SDL_OpenAudioDeviceStream failed");
    }
    SDL_ResumeAudioStreamDevice(stream_);  // the device starts paused; begin draining
}

void SdlAudioSink::stop() {
    if (stream_ != nullptr) {
        SDL_DestroyAudioStream(stream_);  // guarantees the callback is no longer running
        stream_ = nullptr;
    }
    pull_ = nullptr;  // safe to clear: the callback can no longer fire
}

void SdlPlatform::setFullscreen(bool enabled) {
    // NULL fullscreen-mode = SDL3 borderless desktop fullscreen (a real macOS fullscreen Space;
    // a borderless desktop fill elsewhere). The window's fullscreen display mode is left unset, so
    // SDL keeps the desktop resolution; the renderer's letterbox/integer-scale blit absorbs the
    // new drawable size. On failure the tracked state stays as it was (the window is unchanged).
    if (SDL_SetWindowFullscreen(window_, enabled)) {
        fullscreen_ = enabled;
    }
}

// ── Frame pacing (PACE-INTERP sub-block 1) ──────────────────────────────────────

std::chrono::nanoseconds SdlPlatform::nowMonotonic() const {
    // SDL_GetTicksNS: monotonic nanoseconds since SDL init — the same clock domain SDL_DelayPrecise
    // sleeps in, so the host's deadline arithmetic and its sleep agree.
    return std::chrono::nanoseconds{static_cast<std::int64_t>(SDL_GetTicksNS())};
}

std::chrono::nanoseconds SdlPlatform::displayRefreshPeriod() const {
    // Live query of the display the window is currently on, so dragging the window to a different-refresh
    // monitor re-paces with no event handling. Period = 1 / refresh_rate; fall back to 60 Hz when the
    // rate is unknown (0) or the query fails, reusing the canonical Hz60 period constant.
    if (const SDL_DisplayID disp = SDL_GetDisplayForWindow(window_); disp != 0) {
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(disp);
            mode && mode->refresh_rate > 0.0f) {
            return std::chrono::nanoseconds{
                static_cast<std::int64_t>(1'000'000'000.0 / mode->refresh_rate + 0.5)};
        }
    }
    return std::chrono::nanoseconds{static_cast<std::int64_t>(TickPeriodNs::Hz60)};
}

void SdlPlatform::sleepPrecise(std::chrono::nanoseconds duration) {
    // Guard > 0 before the Uint64 cast — a non-positive remainder must not underflow into a multi-century
    // sleep. SDL_DelayPrecise high-resolution-sleeps the bulk and busy-waits only the sub-ms tail.
    if (duration > std::chrono::nanoseconds::zero()) {
        SDL_DelayPrecise(static_cast<Uint64>(duration.count()));
    }
}

}  // namespace retropp
