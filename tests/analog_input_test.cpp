// Device-free coverage for the analog/pointer surface as it threads through the run loop and the
// windowed host — plus the regression guard for the held-direction-plus-fire input drop (ENGINE_
// DISCUSSION_ISSUES §I #24). No window, no GPU, no real mouse: a ManualClock + MockPlatform drive the
// exact chain a live host would.
#include <gtest/gtest.h>

#include "retropp/analog_input.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/run_loop.h"
#include "retropp/timing.h"
#include "retropp/windowed_host.h"

#include "manual_clock.h"
#include "mock_platform.h"

namespace retropp {
namespace {

using test::ManualClock;
using test::MockPlatform;

constexpr auto kTickPeriod = TimingProfile::GameBoyColor.tickPeriod();

ButtonSet only(Button b) {
    ButtonSet s;
    s.set(b, true);
    return s;
}

// ── Relative-quantity accumulation between ticks (§2.4) ──────────────────────────────────────────

TEST(AnalogRunLoop, RelativeQuantitiesAccumulateBetweenTicksThenReset) {
    ManualClock clock;
    RunLoop loop{clock};
    int ticks = 0;
    float seenRawX = -1.0f, seenWheel = -1.0f;
    loop.setTick([&](const InputState& in) {
        ++ticks;
        seenRawX = in.rawDeltaX();
        seenWheel = in.wheel();
    });
    loop.advance();  // settle baseline (0 ticks)

    // Two host frames of motion before a tick — the deltas must SUM, not overwrite.
    AnalogInput f1;
    f1.rawDeltaX = 3.0f;
    f1.wheel = 1.0f;
    loop.setRawAnalog(f1);
    AnalogInput f2;
    f2.rawDeltaX = 2.0f;
    f2.wheel = 0.5f;
    loop.setRawAnalog(f2);

    clock.advanceBy(kTickPeriod);
    loop.advance();
    EXPECT_EQ(ticks, 1);
    EXPECT_FLOAT_EQ(seenRawX, 5.0f);   // 3 + 2 accumulated across both frames
    EXPECT_FLOAT_EQ(seenWheel, 1.5f);  // 1.0 + 0.5

    // The next tick with no new motion sees zero — the accumulator reset on the previous tick.
    clock.advanceBy(kTickPeriod);
    loop.advance();
    EXPECT_EQ(ticks, 2);
    EXPECT_FLOAT_EQ(seenRawX, 0.0f);
    EXPECT_FLOAT_EQ(seenWheel, 0.0f);
}

TEST(AnalogRunLoop, AbsoluteQuantitiesTakeTheLatestFrameValue) {
    ManualClock clock;
    RunLoop loop{clock};
    Vec2i seenCursor{};
    loop.setTick([&](const InputState& in) { seenCursor = in.cursor(); });
    loop.advance();  // settle

    AnalogInput f1;
    f1.cursor = Vec2i{10, 20};
    loop.setRawAnalog(f1);
    AnalogInput f2;
    f2.cursor = Vec2i{33, 44};  // a later frame moved the pointer
    loop.setRawAnalog(f2);

    clock.advanceBy(kTickPeriod);
    loop.advance();
    EXPECT_EQ(seenCursor, (Vec2i{33, 44}));  // latest absolute, not summed
}

// ── The §I #24 regression: a press on a zero-tick frame is not dropped ────────────────────────────

TEST(AnalogRunLoop, PressOnAZeroTickFrameStillFiresAtTheNextTick) {
    ManualClock clock;
    RunLoop loop{clock};
    int presses = 0;
    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::A)) ++presses;
    });
    loop.advance();  // settle

    // Frame 1: A is pressed, but the clock has not advanced a full period → this advance runs 0 ticks.
    // Before the press-buffering fix the level here was simply overwritten and the press was lost.
    loop.setRawInput(only(Button::A));
    loop.advance();  // 0 ticks

    // Frame 2: A is already released; now a tick fires. It must STILL observe the frame-1 press.
    loop.setRawInput(ButtonSet{});
    clock.advanceBy(kTickPeriod);
    loop.advance();  // 1 tick

    EXPECT_EQ(presses, 1);
}

TEST(AnalogRunLoop, ContinuousHoldAcrossZeroTickFramesPressesExactlyOnce) {
    ManualClock clock;
    RunLoop loop{clock};
    int presses = 0;
    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::Right)) ++presses;
    });
    loop.advance();  // settle

    // Several zero-tick frames while Right is held, then a run of real ticks — the press must fire once.
    for (int i = 0; i < 3; ++i) {
        loop.setRawInput(only(Button::Right));
        loop.advance();  // 0 ticks (clock not advanced)
    }
    for (int i = 0; i < 4; ++i) {
        loop.setRawInput(only(Button::Right));
        clock.advanceBy(kTickPeriod);
        loop.advance();  // 1 tick each
    }
    EXPECT_EQ(presses, 1);
}

// ── End-to-end through the windowed host (the in-tree consumer proof) ──────────────────────────────

TEST(AnalogWindowedHost, ScriptedPointerAndButtonReachTheTick) {
    ManualClock clock;
    RunLoop loop{clock};
    MockPlatform platform{4};

    int aPresses = 0;
    Vec2i lastCursor{};
    bool sawOnScreen = false;
    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::A)) ++aPresses;
        lastCursor = in.cursor();
        if (in.cursorOnScreen()) sawOnScreen = true;
    });

    int pump = 0;
    platform.setOnPump([&] {
        ++pump;
        clock.advanceBy(kTickPeriod);  // one tick per host iteration
        AnalogInput a;
        a.cursor = Vec2i{pump * 10, pump * 5};
        a.cursorOnScreen = true;
        platform.setAnalog(a);
        platform.setHeld(pump == 2 ? only(Button::A) : ButtonSet{});  // tap A on iteration 2 only
    });

    WindowedHost{loop, platform}.run();

    EXPECT_EQ(platform.pumpCount(), 4);
    EXPECT_EQ(aPresses, 1);                       // the button threaded through and fired once
    EXPECT_TRUE(sawOnScreen);                     // the on-screen flag threaded through
    EXPECT_EQ(lastCursor, (Vec2i{40, 20}));       // the final iteration's cursor reached the tick
}

// ── The pointer-capture seam ──────────────────────────────────────────────────────────────────────

TEST(AnalogPointerCapture, SeamTogglesTrackedState) {
    MockPlatform platform{1};
    Platform& seam = platform;  // exercise through the abstract interface

    EXPECT_FALSE(seam.pointerCaptured());
    seam.setPointerCaptured(true);
    EXPECT_TRUE(seam.pointerCaptured());
    seam.setPointerCaptured(false);
    EXPECT_FALSE(seam.pointerCaptured());
}

}  // namespace
}  // namespace retropp
