#include <gtest/gtest.h>

#include "gbcpp/input.h"
#include "gbcpp/run_loop.h"
#include "gbcpp/windowed_host.h"

#include "manual_clock.h"
#include "mock_platform.h"

namespace gbcpp {
namespace {

using test::ManualClock;
using test::MockPlatform;

// A platform that quits after N pumps drives exactly N host iterations, and the render
// callback fires once per iteration.
TEST(WindowedHost, QuitAfterNPumpsRunsExactlyNIterations) {
    ManualClock  clock;
    RunLoop      loop{clock};
    MockPlatform platform{5};

    int renders = 0;
    loop.setRender([&](float) { ++renders; });

    WindowedHost host{loop, platform};
    host.run();

    EXPECT_EQ(platform.pumpCount(), 5);
    EXPECT_EQ(renders, 5);  // render fired once per iteration
}

// A platform already quit-requested before the first iteration runs zero iterations:
// no pump, no advance, no present.
TEST(WindowedHost, ImmediateQuitRunsZeroIterations) {
    ManualClock  clock;
    RunLoop      loop{clock};
    MockPlatform platform{0};

    int renders = 0;
    loop.setRender([&](float) { ++renders; });

    WindowedHost host{loop, platform};
    host.run();

    EXPECT_EQ(platform.pumpCount(), 0);
    EXPECT_EQ(renders, 0);
    EXPECT_EQ(loop.tickCount(), 0u);
}

// Each iteration pushes the platform's current ButtonSet into the loop BEFORE
// advancing, so a held button supplied by the platform reaches the tick callback.
// The per-pump hook advances the injected clock by one tick period, so a tick fires
// each iteration and samples the pushed input.
TEST(WindowedHost, PushedInputReachesTickCallback) {
    ManualClock  clock;
    RunLoop      loop{clock};
    MockPlatform platform{6};
    platform.setOnPump([&] { clock.advanceBy(kTickPeriod); });

    ButtonSet held;
    held.set(Button::A, true);
    held.set(Button::Right, true);
    platform.setHeld(held);

    bool sawA = false, sawRight = false, sawStart = false;
    loop.setTick([&](const InputState& in) {
        if (in.isHeld(Button::A))     sawA = true;
        if (in.isHeld(Button::Right)) sawRight = true;
        if (in.isHeld(Button::Start)) sawStart = true;  // never held → must stay false
    });

    WindowedHost host{loop, platform};
    host.run();

    EXPECT_GT(loop.tickCount(), 0u);  // ticks actually fired
    EXPECT_TRUE(sawA);
    EXPECT_TRUE(sawRight);
    EXPECT_FALSE(sawStart);
}

// Ordering guard: each iteration pushes input BEFORE advancing, so a tick firing on
// that iteration observes the input pushed on the same iteration. The loop's baseline
// is pre-consumed with one manual advance() so the host's very first iteration already
// produces a tick — which therefore sees the pushed input only if setRawInput precedes
// advance. Swapping the host's pump/setRawInput/advance order turns this red.
TEST(WindowedHost, PushedInputIsObservedOnTheSameIteration) {
    ManualClock  clock;
    RunLoop      loop{clock};

    int  ticks = 0;
    bool pressedB = false;
    loop.setTick([&](const InputState& in) {
        ++ticks;
        if (in.justPressed(Button::B)) pressedB = true;
    });

    // Consume the lazy baseline (started_ = true, last_ = now) with no tick, so the
    // host's first iteration's advance() crosses a full tick period.
    loop.advance();

    MockPlatform platform{1};
    platform.setOnPump([&] { clock.advanceBy(kTickPeriod); });
    ButtonSet held;
    held.set(Button::B, true);
    platform.setHeld(held);

    WindowedHost host{loop, platform};
    host.run();

    EXPECT_EQ(ticks, 1);
    EXPECT_TRUE(pressedB);
}

}  // namespace
}  // namespace gbcpp
