#include <array>
#include <optional>

#include <gtest/gtest.h>

#include "gbcpp/input.h"
#include "gbcpp/input_map.h"

namespace gbcpp {
namespace {

TEST(InputMap, KeyboardDefaultsMapToButtons) {
    EXPECT_EQ(mapScancode(SDL_SCANCODE_UP),        std::make_optional(Button::Up));
    EXPECT_EQ(mapScancode(SDL_SCANCODE_DOWN),      std::make_optional(Button::Down));
    EXPECT_EQ(mapScancode(SDL_SCANCODE_LEFT),      std::make_optional(Button::Left));
    EXPECT_EQ(mapScancode(SDL_SCANCODE_RIGHT),     std::make_optional(Button::Right));
    EXPECT_EQ(mapScancode(SDL_SCANCODE_X),         std::make_optional(Button::A));
    EXPECT_EQ(mapScancode(SDL_SCANCODE_Z),         std::make_optional(Button::B));
    EXPECT_EQ(mapScancode(SDL_SCANCODE_RETURN),    std::make_optional(Button::Start));
    EXPECT_EQ(mapScancode(SDL_SCANCODE_BACKSPACE), std::make_optional(Button::Select));
}

TEST(InputMap, GamepadDefaultsMapToButtons) {
    EXPECT_EQ(mapGamepadButton(SDL_GAMEPAD_BUTTON_DPAD_UP),    std::make_optional(Button::Up));
    EXPECT_EQ(mapGamepadButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN),  std::make_optional(Button::Down));
    EXPECT_EQ(mapGamepadButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT),  std::make_optional(Button::Left));
    EXPECT_EQ(mapGamepadButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), std::make_optional(Button::Right));
    EXPECT_EQ(mapGamepadButton(SDL_GAMEPAD_BUTTON_SOUTH),      std::make_optional(Button::A));
    EXPECT_EQ(mapGamepadButton(SDL_GAMEPAD_BUTTON_EAST),       std::make_optional(Button::B));
    EXPECT_EQ(mapGamepadButton(SDL_GAMEPAD_BUTTON_START),      std::make_optional(Button::Start));
    EXPECT_EQ(mapGamepadButton(SDL_GAMEPAD_BUTTON_BACK),       std::make_optional(Button::Select));
}

TEST(InputMap, UnmappedScancodeMapsToNothing) {
    // Keys outside the default bindings translate to nothing.
    EXPECT_FALSE(mapScancode(SDL_SCANCODE_Q).has_value());
    EXPECT_FALSE(mapScancode(SDL_SCANCODE_SPACE).has_value());
    EXPECT_FALSE(mapScancode(SDL_SCANCODE_A).has_value());  // 'A' the key, not Button::A
}

TEST(InputMap, UnmappedGamepadButtonMapsToNothing) {
    EXPECT_FALSE(mapGamepadButton(SDL_GAMEPAD_BUTTON_NORTH).has_value());
    EXPECT_FALSE(mapGamepadButton(SDL_GAMEPAD_BUTTON_WEST).has_value());
    EXPECT_FALSE(mapGamepadButton(SDL_GAMEPAD_BUTTON_GUIDE).has_value());
}

TEST(InputMap, KeyboardTableCoversAllEightButtonsExactlyOnce) {
    std::array<int, kButtonCount> seen{};
    for (const auto& row : kDefaultKeyMap) {
        ++seen[static_cast<std::size_t>(row.button)];
    }
    for (int count : seen) EXPECT_EQ(count, 1);
}

TEST(InputMap, GamepadTableCoversAllEightButtonsExactlyOnce) {
    std::array<int, kButtonCount> seen{};
    for (const auto& row : kDefaultGamepadMap) {
        ++seen[static_cast<std::size_t>(row.button)];
    }
    for (int count : seen) EXPECT_EQ(count, 1);
}

}  // namespace
}  // namespace gbcpp
