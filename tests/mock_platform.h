#pragma once

#include <functional>
#include <utility>

#include "gbcpp/input.h"
#include "gbcpp/platform.h"

namespace gbcpp::test {

// A headless Platform stand-in for the windowed-host suite. It opens no window and
// touches no device: it reports a fixed held-button state, latches quit after a set
// number of pumps, counts pumps/presents, and runs an optional per-pump hook (used to
// advance an injected clock so the run loop ticks deterministically).
class MockPlatform final : public Platform {
public:
    explicit MockPlatform(int quitAfterPumps) noexcept
        : quitAfter_(quitAfterPumps), quit_(quitAfterPumps <= 0) {}

    void setHeld(ButtonSet held) noexcept { held_ = held; }
    void setOnPump(std::function<void()> fn) { onPump_ = std::move(fn); }

    void pumpEvents() override {
        ++pumpCount_;
        if (onPump_) onPump_();
        if (pumpCount_ >= quitAfter_) quit_ = true;
    }
    [[nodiscard]] bool quitRequested() const override { return quit_; }
    [[nodiscard]] ButtonSet buttons() const override { return held_; }
    void presentClearFrame() override { ++presentCount_; }

    [[nodiscard]] int pumpCount() const noexcept { return pumpCount_; }
    [[nodiscard]] int presentCount() const noexcept { return presentCount_; }

private:
    int  quitAfter_;
    bool quit_;
    int  pumpCount_    = 0;
    int  presentCount_ = 0;
    ButtonSet held_;
    std::function<void()> onPump_;
};

}  // namespace gbcpp::test
