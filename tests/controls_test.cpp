#include <optional>

#include <gtest/gtest.h>

#include "retropp/input.h"
#include "retropp/input_map.h"

namespace retropp {
namespace {

// SDL's fine-grained gamepad types collapse to the engine's controller families —
// the "automatic Xbox / PS5 detection" surface, tested against SDL enum values with
// no live device.
TEST(ControllerType, XboxFamilyDetected) {
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_XBOX360), ControllerType::Xbox);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_XBOXONE), ControllerType::Xbox);
}

TEST(ControllerType, PlayStationFamilyDetected) {
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_PS3), ControllerType::PlayStation);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_PS4), ControllerType::PlayStation);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_PS5), ControllerType::PlayStation);
}

TEST(ControllerType, NintendoFamilyDetected) {
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO),
              ControllerType::Nintendo);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR),
              ControllerType::Nintendo);
}

TEST(ControllerType, StandardAndUnknown) {
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_STANDARD), ControllerType::Standard);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_UNKNOWN), ControllerType::Unknown);
}

// Default bindings mirror the canonical default profile in both directions.
TEST(ControlBindings, DefaultsMatchCanonicalProfile) {
    const ControlBindings b = ControlBindings::defaults();

    EXPECT_EQ(b.fromScancode(SDL_SCANCODE_X), std::make_optional(Button::A));
    EXPECT_EQ(b.fromScancode(SDL_SCANCODE_UP), std::make_optional(Button::Up));
    EXPECT_EQ(b.fromGamepadButton(SDL_GAMEPAD_BUTTON_SOUTH), std::make_optional(Button::A));
    EXPECT_EQ(b.fromGamepadButton(SDL_GAMEPAD_BUTTON_BACK), std::make_optional(Button::Select));

    EXPECT_EQ(b.keyFor(Button::A), SDL_SCANCODE_X);
    EXPECT_EQ(b.keyFor(Button::Start), SDL_SCANCODE_RETURN);
    EXPECT_EQ(b.gamepadButtonFor(Button::B), SDL_GAMEPAD_BUTTON_EAST);
}

TEST(ControlBindings, DefaultsForGamepadFlipNintendoFaceButtons) {
    const ControlBindings xbox = ControlBindings::defaultsForGamepad(ControllerType::Xbox);
    const ControlBindings ps   = ControlBindings::defaultsForGamepad(ControllerType::PlayStation);
    const ControlBindings nin  = ControlBindings::defaultsForGamepad(ControllerType::Nintendo);

    // Xbox / PlayStation keep the positional layout: A = south (bottom) = confirm.
    EXPECT_EQ(xbox.gamepadButtonFor(Button::A), SDL_GAMEPAD_BUTTON_SOUTH);
    EXPECT_EQ(ps.gamepadButtonFor(Button::A),   SDL_GAMEPAD_BUTTON_SOUTH);
    EXPECT_EQ(xbox.gamepadButtonFor(Button::A), ps.gamepadButtonFor(Button::A));

    // Nintendo: logical A/B/X/Y bind to the Nintendo-LABELLED positions (A at east, etc.) so a
    // Switch player's labelled "A" confirms — the A/B and X/Y transposition vs Xbox.
    EXPECT_EQ(nin.gamepadButtonFor(Button::A), SDL_GAMEPAD_BUTTON_EAST);
    EXPECT_EQ(nin.gamepadButtonFor(Button::B), SDL_GAMEPAD_BUTTON_SOUTH);
    EXPECT_EQ(nin.gamepadButtonFor(Button::X), SDL_GAMEPAD_BUTTON_NORTH);
    EXPECT_EQ(nin.gamepadButtonFor(Button::Y), SDL_GAMEPAD_BUTTON_WEST);

    // The keyboard half is unaffected by controller family.
    EXPECT_EQ(nin.keyFor(Button::A), SDL_SCANCODE_X);
}

// Rebinding a key reroutes the lookup and frees the old scancode.
TEST(ControlBindings, RebindKeyChangesLookup) {
    ControlBindings b = ControlBindings::defaults();
    b.bindKey(Button::A, SDL_SCANCODE_J);

    EXPECT_EQ(b.fromScancode(SDL_SCANCODE_J), std::make_optional(Button::A));
    EXPECT_FALSE(b.fromScancode(SDL_SCANCODE_X).has_value());  // old binding cleared
    EXPECT_EQ(b.keyFor(Button::A), SDL_SCANCODE_J);
}

TEST(ControlBindings, RebindGamepadButtonChangesLookup) {
    ControlBindings b = ControlBindings::defaults();
    // Rebind to an unbound button (WEST/NORTH now default to X/Y).
    b.bindGamepadButton(Button::A, SDL_GAMEPAD_BUTTON_RIGHT_STICK);

    EXPECT_EQ(b.fromGamepadButton(SDL_GAMEPAD_BUTTON_RIGHT_STICK), std::make_optional(Button::A));
    EXPECT_FALSE(b.fromGamepadButton(SDL_GAMEPAD_BUTTON_SOUTH).has_value());
    EXPECT_EQ(b.gamepadButtonFor(Button::A), SDL_GAMEPAD_BUTTON_RIGHT_STICK);
}

}  // namespace
}  // namespace retropp
