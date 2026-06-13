#pragma once

#include <functional>
#include <utility>

#include "gbcpp/input.h"
#include "gbcpp/platform.h"

namespace gbcpp::test {

// A headless Platform stand-in for the windowed-host suite. It opens no window and
// touches no device: it reports a fixed held-button state and drawable size, latches
// quit after a set number of pumps, counts pumps, and runs an optional per-pump hook
// (used to advance an injected clock so the run loop ticks deterministically).
class MockPlatform final : public Platform {
public:
    explicit MockPlatform(int quitAfterPumps) noexcept
        : quitAfter_(quitAfterPumps), quit_(quitAfterPumps <= 0) {}

    void setHeld(ButtonSet held) noexcept { held_ = held; }
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
    ButtonSet held_;
    PixelSize drawable_{640, 576};   // 4× the GB viewport by default
    PixelSize usable_{4096, 4096};   // a roomy default "display" for headless scale-fit tests
    std::function<void()> onPump_;
};

}  // namespace gbcpp::test
