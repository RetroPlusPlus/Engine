#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "retropp/audio.h"
#include "retropp/input.h"
#include "retropp/platform.h"

namespace retropp::test {

// A headless AudioSink for the device-free suite: it opens no device. start() records the rate /
// channels / pull the AudioSystem hands it; drain() invokes that pull on demand (standing in for the
// audio thread), so a test can observe exactly what the chain produced without a sound device.
class CaptureAudioSink final : public AudioSink {
public:
    void start(unsigned rate, int channels, AudioPullFn pull) override {
        rate_ = rate;
        channels_ = channels;
        pull_ = std::move(pull);
        started_ = true;
    }
    void stop() override {
        started_ = false;
        pull_ = nullptr;
    }

    // Pull up to `n` frames through the stored pull, exactly as the audio thread would; returns the
    // frames actually produced (a short vector means the chain underflowed).
    std::vector<AudioFrame> drain(std::size_t n) {
        std::vector<AudioFrame> out(n);
        const std::size_t got = pull_ ? pull_(std::span<AudioFrame>(out.data(), out.size())) : 0;
        out.resize(got);
        return out;
    }

    [[nodiscard]] unsigned rate() const noexcept { return rate_; }
    [[nodiscard]] int channels() const noexcept { return channels_; }
    [[nodiscard]] bool started() const noexcept { return started_; }

private:
    AudioPullFn pull_;
    unsigned    rate_ = 0;
    int         channels_ = 0;
    bool        started_ = false;
};

// A headless Platform stand-in for the windowed-host suite. It opens no window and
// touches no device: it reports a fixed held-button state and drawable size, latches
// quit after a set number of pumps, counts pumps, and runs an optional per-pump hook
// (used to advance an injected clock so the run loop ticks deterministically).
class MockPlatform final : public Platform {
public:
    explicit MockPlatform(int quitAfterPumps) noexcept
        : quitAfter_(quitAfterPumps), quit_(quitAfterPumps <= 0) {}

    void setHeld(ButtonSet held) noexcept { held_ = held; }
    void setAnalog(const AnalogInput& a) noexcept { analog_ = a; }
    void setOnPump(std::function<void()> fn) { onPump_ = std::move(fn); }
    void setDrawableSize(PixelSize size) noexcept { drawable_ = size; }
    void setUsableDisplaySize(PixelSize size) noexcept { usable_ = size; }

    void pumpEvents() override {
        ++pumpCount_;
        if (onPump_) onPump_();
        if (pumpCount_ >= quitAfter_) quit_ = true;
    }
    [[nodiscard]] bool quitRequested() const override { return quit_; }
    [[nodiscard]] ButtonSet buttons() const override { return held_; }
    [[nodiscard]] AnalogInput analog() const override { return analog_; }
    void setPointerCaptured(bool captured) override { pointerCaptured_ = captured; }
    [[nodiscard]] bool pointerCaptured() const override { return pointerCaptured_; }
    [[nodiscard]] PixelSize drawableSize() const override { return drawable_; }

    // Headless window sizing: track the requested logical size and reflect it as the drawable
    // (density 1, so logical == physical) so tests can observe a resize through drawableSize().
    void setWindowSize(PixelSize size) override { drawable_ = size; }
    [[nodiscard]] PixelSize usableDisplaySize() const override { return usable_; }

    // Headless fullscreen: just track the requested state (no window to toggle).
    void setFullscreen(bool enabled) override { fullscreen_ = enabled; }
    [[nodiscard]] bool isFullscreen() const override { return fullscreen_; }

    [[nodiscard]] int pumpCount() const noexcept { return pumpCount_; }

private:
    int  quitAfter_;
    bool quit_;
    int  pumpCount_ = 0;
    bool fullscreen_ = false;
    bool pointerCaptured_ = false;
    ButtonSet held_;
    AnalogInput analog_;
    PixelSize drawable_{640, 576};   // 4× the GB viewport by default
    PixelSize usable_{4096, 4096};   // a roomy default "display" for headless scale-fit tests
    std::function<void()> onPump_;
};

}  // namespace retropp::test
