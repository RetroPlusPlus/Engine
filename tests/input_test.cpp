#include <gtest/gtest.h>

#include "retropp/analog_input.h"
#include "retropp/input.h"

using retropp::AnalogInput;
using retropp::Button;
using retropp::ButtonSet;
using retropp::InputState;
using retropp::MouseButton;
using retropp::Stick;
using retropp::Trigger;

namespace {

// Most digital cases want the common case: a button held this tick is also the press source for the
// tick (heldUnion == level — what the run loop produces for a continuously-held button). This helper
// keeps those readable while sampleTick's real signature is (held, pressedSinceTick, analog).
void step(InputState& in, ButtonSet level) {
    in.sampleTick(level, level, AnalogInput{});
}

ButtonSet only(Button b) {
    ButtonSet s;
    s.set(b, true);
    return s;
}

}  // namespace

TEST(ButtonSet, DefaultIsAllReleased) {
    ButtonSet s;
    EXPECT_EQ(s.bits(), 0u);
    EXPECT_FALSE(s.held(Button::A));
}

TEST(ButtonSet, SetAndHeldRoundTripPerButton) {
    ButtonSet s;
    s.set(Button::Start, true);
    EXPECT_TRUE(s.held(Button::Start));
    EXPECT_FALSE(s.held(Button::Select));  // a neighbouring bit is untouched
    s.set(Button::Start, false);
    EXPECT_FALSE(s.held(Button::Start));
}

TEST(ButtonSet, BitsArePackedOneBitPerButton) {
    ButtonSet s;
    s.set(Button::Up, true);      // bit 0
    s.set(Button::Select, true);  // bit 11 (the highest shipped button)
    EXPECT_EQ(s.bits(), (1u << 0) | (1u << 11));
}

TEST(ButtonSet, HoldsButtonsBeyondBitSeven) {
    // The GB-era uint8_t storage could not hold bits >= 8; the widened uint32_t can.
    // This is the regression guard for the storage-width generalization.
    ButtonSet s;
    s.set(Button::L, true);       // bit 8
    s.set(Button::R, true);       // bit 9
    s.set(Button::Select, true);  // bit 11
    EXPECT_TRUE(s.held(Button::L));
    EXPECT_TRUE(s.held(Button::R));
    EXPECT_TRUE(s.held(Button::Select));
    EXPECT_FALSE(s.held(Button::A));
    EXPECT_EQ(s.bits(), (1u << 8) | (1u << 9) | (1u << 11));
}

TEST(ButtonSet, EqualityComparesHeldState) {
    ButtonSet a;
    ButtonSet b;
    EXPECT_EQ(a, b);
    a.set(Button::B, true);
    EXPECT_NE(a, b);
    b.set(Button::B, true);
    EXPECT_EQ(a, b);
}

TEST(ButtonSet, UnionMergesHeldBitsOfBothSets) {
    // The press-buffering union the run loop relies on: OR keeps every button held in either set.
    ButtonSet a = only(Button::A);
    ButtonSet right = only(Button::Right);
    EXPECT_EQ((a | right).bits(), only(Button::A).bits() | only(Button::Right).bits());
    a |= right;
    EXPECT_TRUE(a.held(Button::A));
    EXPECT_TRUE(a.held(Button::Right));
}

TEST(InputState, HeldReflectsTheSampledRaw) {
    InputState in;
    step(in, only(Button::A));
    EXPECT_TRUE(in.isHeld(Button::A));
    EXPECT_FALSE(in.isHeld(Button::B));
}

TEST(InputState, JustPressedFiresOnlyOnTheTransitionTick) {
    InputState in;
    const ButtonSet down = only(Button::A);

    step(in, down);  // released (baseline) → held: pressed edge on tick 1
    EXPECT_TRUE(in.justPressed(Button::A));
    EXPECT_TRUE(in.isHeld(Button::A));

    step(in, down);  // held → held: no edge
    EXPECT_FALSE(in.justPressed(Button::A));
    EXPECT_TRUE(in.isHeld(Button::A));
}

TEST(InputState, JustReleasedFiresOnlyOnTheReleaseTick) {
    InputState in;
    const ButtonSet down = only(Button::B);
    const ButtonSet up;  // all released

    step(in, down);
    step(in, up);  // held → released: release edge
    EXPECT_TRUE(in.justReleased(Button::B));
    EXPECT_FALSE(in.isHeld(Button::B));

    step(in, up);  // released → released: no edge
    EXPECT_FALSE(in.justReleased(Button::B));
}

TEST(InputState, ButtonHeldAcrossManyTicksPressesOnce) {
    InputState in;
    const ButtonSet down = only(Button::Left);

    step(in, down);
    EXPECT_TRUE(in.justPressed(Button::Left));
    for (int i = 0; i < 5; ++i) {
        step(in, down);
        EXPECT_FALSE(in.justPressed(Button::Left));
    }
    EXPECT_TRUE(in.isHeld(Button::Left));
}

// ── Press buffering — the §I #24 fix ──────────────────────────────────────────────────────────────

TEST(InputState, SubTickTapStillRegistersAsAPress) {
    // A tap whose button was seen held in a frame poll (pressedSinceTick) but is already released by
    // tick time (held = released) must STILL fire justPressed exactly once — the input that was
    // previously dropped while a direction was held. isHeld is honest (false), so nothing sticks.
    InputState in;
    const ButtonSet none;
    in.sampleTick(/*held=*/none, /*pressedSinceTick=*/only(Button::A), AnalogInput{});
    EXPECT_TRUE(in.justPressed(Button::A));
    EXPECT_FALSE(in.isHeld(Button::A));

    // The following tick with nothing pressed must NOT re-fire it.
    in.sampleTick(none, none, AnalogInput{});
    EXPECT_FALSE(in.justPressed(Button::A));
}

TEST(InputState, HeldDirectionDoesNotSuppressABufferedFireTap) {
    // The exact reported scenario: a direction held continuously while fire is tapped between ticks.
    // The direction stays held (no press edge after the first), and the fire tap registers its press.
    InputState in;
    const ButtonSet right = only(Button::Right);
    in.sampleTick(right, right, AnalogInput{});  // start holding Right
    EXPECT_TRUE(in.justPressed(Button::Right));

    // Next tick: Right still held; A was tapped and released between ticks (in the union, not the level).
    ButtonSet unionMask = right;
    unionMask.set(Button::A, true);
    in.sampleTick(/*held=*/right, /*pressedSinceTick=*/unionMask, AnalogInput{});
    EXPECT_TRUE(in.isHeld(Button::Right));
    EXPECT_FALSE(in.justPressed(Button::Right));  // direction does not re-press
    EXPECT_TRUE(in.justPressed(Button::A));        // the fire tap is not dropped
    EXPECT_FALSE(in.isHeld(Button::A));
}

// ── Analog / pointer surface ──────────────────────────────────────────────────────────────────────

TEST(InputState, StoresCursorAndOnScreenFlag) {
    InputState in;
    AnalogInput a;
    a.cursor = retropp::Vec2i{40, 72};
    a.cursorOnScreen = true;
    in.sampleTick(ButtonSet{}, ButtonSet{}, a);
    EXPECT_EQ(in.cursor(), (retropp::Vec2i{40, 72}));
    EXPECT_TRUE(in.cursorOnScreen());
}

TEST(InputState, CursorDeltaIsThePerTickViewportChange) {
    InputState in;
    AnalogInput a;
    a.cursor = retropp::Vec2i{10, 10};
    in.sampleTick(ButtonSet{}, ButtonSet{}, a);  // first sample: prev is default {0,0}
    EXPECT_EQ(in.cursorDelta(), (retropp::Vec2i{10, 10}));

    a.cursor = retropp::Vec2i{13, 7};
    in.sampleTick(ButtonSet{}, ButtonSet{}, a);
    EXPECT_EQ(in.cursorDelta(), (retropp::Vec2i{3, -3}));
}

TEST(InputState, RawDeltaAndWheelArePassedThroughFromTheSample) {
    InputState in;
    AnalogInput a;
    a.rawDeltaX = 4.5f;
    a.rawDeltaY = -2.0f;
    a.wheel = 1.0f;
    in.sampleTick(ButtonSet{}, ButtonSet{}, a);
    EXPECT_FLOAT_EQ(in.rawDeltaX(), 4.5f);
    EXPECT_FLOAT_EQ(in.rawDeltaY(), -2.0f);
    EXPECT_FLOAT_EQ(in.wheel(), 1.0f);
}

TEST(InputState, MouseButtonEdgesMirrorTheDigitalEdges) {
    InputState in;
    AnalogInput up;          // no mouse buttons
    AnalogInput leftDown;
    leftDown.mouseHeld = std::uint8_t{1} << static_cast<int>(MouseButton::Left);

    in.sampleTick(ButtonSet{}, ButtonSet{}, leftDown);  // released → held
    EXPECT_TRUE(in.mouseHeld(MouseButton::Left));
    EXPECT_TRUE(in.mouseJustPressed(MouseButton::Left));
    EXPECT_FALSE(in.mouseJustReleased(MouseButton::Left));

    in.sampleTick(ButtonSet{}, ButtonSet{}, leftDown);  // held → held
    EXPECT_FALSE(in.mouseJustPressed(MouseButton::Left));

    in.sampleTick(ButtonSet{}, ButtonSet{}, up);        // held → released
    EXPECT_FALSE(in.mouseHeld(MouseButton::Left));
    EXPECT_TRUE(in.mouseJustReleased(MouseButton::Left));
}

TEST(AnalogInput, MouseDownTestsTheRightBit) {
    AnalogInput a;
    a.mouseHeld = std::uint8_t{1} << static_cast<int>(MouseButton::Right);
    EXPECT_TRUE(a.mouseDown(MouseButton::Right));
    EXPECT_FALSE(a.mouseDown(MouseButton::Left));
    EXPECT_FALSE(a.mouseDown(MouseButton::Middle));
}

TEST(InputState, SticksAndTriggersPassThrough) {
    InputState in;
    AnalogInput a;
    a.leftX = -0.5f;  a.leftY = 0.25f;
    a.rightX = 0.9f;  a.rightY = -0.1f;
    a.triggerL = 0.3f; a.triggerR = 0.7f;
    in.sampleTick(ButtonSet{}, ButtonSet{}, a);
    EXPECT_FLOAT_EQ(in.stick(Stick::Left).x, -0.5f);
    EXPECT_FLOAT_EQ(in.stick(Stick::Left).y, 0.25f);
    EXPECT_FLOAT_EQ(in.stick(Stick::Right).x, 0.9f);
    EXPECT_FLOAT_EQ(in.stick(Stick::Right).y, -0.1f);
    EXPECT_FLOAT_EQ(in.trigger(Trigger::Left), 0.3f);
    EXPECT_FLOAT_EQ(in.trigger(Trigger::Right), 0.7f);
}
