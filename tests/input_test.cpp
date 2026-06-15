#include <gtest/gtest.h>

#include "retropp/input.h"

using retropp::Button;
using retropp::ButtonSet;
using retropp::InputState;

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
