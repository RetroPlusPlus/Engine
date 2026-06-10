#include <gtest/gtest.h>

#include "gbcpp/input.h"
#include "gbcpp/input_map.h"

namespace gbcpp {
namespace {

// InputProfile = the TARGET console's logical button set. Presets are compile-time
// constants (used here in constexpr contexts to prove it). Distinct from ControllerType
// (the host pad family) — see controls_test.cpp.

TEST(InputProfile, GameBoyHasTheEightAndNotTheSnesFour) {
    constexpr InputProfile gb = InputProfile::GameBoy;
    for (Button b : {Button::Up, Button::Down, Button::Left, Button::Right,
                     Button::A, Button::B, Button::Start, Button::Select}) {
        EXPECT_TRUE(gb.has(b));
    }
    EXPECT_FALSE(gb.has(Button::X));
    EXPECT_FALSE(gb.has(Button::Y));
    EXPECT_FALSE(gb.has(Button::L));
    EXPECT_FALSE(gb.has(Button::R));
}

TEST(InputProfile, SnesAddsXYLROnTopOfTheEight) {
    constexpr InputProfile snes = InputProfile::Snes;
    EXPECT_TRUE(snes.has(Button::X));
    EXPECT_TRUE(snes.has(Button::Y));
    EXPECT_TRUE(snes.has(Button::L));
    EXPECT_TRUE(snes.has(Button::R));
    // still carries the base eight
    EXPECT_TRUE(snes.has(Button::A));
    EXPECT_TRUE(snes.has(Button::Start));
    EXPECT_TRUE(snes.has(Button::Select));
}

TEST(InputProfile, GameBoyNesMasterSystemShareTheIdenticalMask) {
    // Same LOGICAL button set; they differ only in name + (deferred) glyph labels.
    EXPECT_EQ(InputProfile::GameBoy.buttons, InputProfile::Nes.buttons);
    EXPECT_EQ(InputProfile::GameBoy.buttons, InputProfile::MasterSystem.buttons);
    // SNES genuinely differs (its button SET is larger).
    EXPECT_NE(InputProfile::GameBoy.buttons, InputProfile::Snes.buttons);
}

TEST(InputProfile, MasterSystemExposesEightLogicalInputs) {
    // SMS: 1->A, 2->B, Pause->Start, Reset->Select. Console placement of Pause/Reset and
    // the "1"/"2" labels are glyph-layer metadata, not part of the logical set.
    constexpr InputProfile sms = InputProfile::MasterSystem;
    EXPECT_TRUE(sms.has(Button::A));       // 1
    EXPECT_TRUE(sms.has(Button::B));       // 2
    EXPECT_TRUE(sms.has(Button::Start));   // Pause
    EXPECT_TRUE(sms.has(Button::Select));  // Reset
    EXPECT_FALSE(sms.has(Button::X));      // no SNES face buttons
}

TEST(InputProfile, NamesAreSet) {
    EXPECT_EQ(InputProfile::GameBoy.name, "Game Boy");
    EXPECT_EQ(InputProfile::Nes.name, "NES");
    EXPECT_EQ(InputProfile::MasterSystem.name, "Master System");
    EXPECT_EQ(InputProfile::Snes.name, "SNES");
}

TEST(MakeButtonSet, BuildsTheRequestedMask) {
    constexpr ButtonSet s = makeButtonSet({Button::A, Button::L, Button::Select});
    EXPECT_TRUE(s.held(Button::A));
    EXPECT_TRUE(s.held(Button::L));       // bit 8 — proves the wide path through makeButtonSet
    EXPECT_TRUE(s.held(Button::Select));  // bit 11
    EXPECT_FALSE(s.held(Button::B));
    EXPECT_EQ(s.bits(), (1u << 4) | (1u << 8) | (1u << 11));
}

TEST(MakeButtonSet, EmptyListIsTheEmptySet) {
    constexpr ButtonSet s = makeButtonSet({});
    EXPECT_EQ(s.bits(), 0u);
}

}  // namespace
}  // namespace gbcpp
