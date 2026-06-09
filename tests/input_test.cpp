#include <gtest/gtest.h>

#include "gbcpp/input.h"

using gbcpp::Button;
using gbcpp::ButtonSet;
using gbcpp::InputState;

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
    s.set(Button::Select, true);  // bit 7
    EXPECT_EQ(s.bits(), 0b1000'0001u);
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

TEST(InputState, HeldReflectsTheSampledRaw) {
    InputState in;
    ButtonSet raw;
    raw.set(Button::A, true);
    in.sampleTick(raw);
    EXPECT_TRUE(in.isHeld(Button::A));
    EXPECT_FALSE(in.isHeld(Button::B));
}

TEST(InputState, JustPressedFiresOnlyOnTheTransitionTick) {
    InputState in;
    ButtonSet down;
    down.set(Button::A, true);

    in.sampleTick(down);  // released (baseline) → held: pressed edge on tick 1
    EXPECT_TRUE(in.justPressed(Button::A));
    EXPECT_TRUE(in.isHeld(Button::A));

    in.sampleTick(down);  // held → held: no edge
    EXPECT_FALSE(in.justPressed(Button::A));
    EXPECT_TRUE(in.isHeld(Button::A));
}

TEST(InputState, JustReleasedFiresOnlyOnTheReleaseTick) {
    InputState in;
    ButtonSet down;
    down.set(Button::B, true);
    const ButtonSet up;  // all released

    in.sampleTick(down);
    in.sampleTick(up);  // held → released: release edge
    EXPECT_TRUE(in.justReleased(Button::B));
    EXPECT_FALSE(in.isHeld(Button::B));

    in.sampleTick(up);  // released → released: no edge
    EXPECT_FALSE(in.justReleased(Button::B));
}

TEST(InputState, ButtonHeldAcrossManyTicksPressesOnce) {
    InputState in;
    ButtonSet down;
    down.set(Button::Left, true);

    in.sampleTick(down);
    EXPECT_TRUE(in.justPressed(Button::Left));
    for (int i = 0; i < 5; ++i) {
        in.sampleTick(down);
        EXPECT_FALSE(in.justPressed(Button::Left));
    }
    EXPECT_TRUE(in.isHeld(Button::Left));
}
