#pragma once

#include <array>
#include <optional>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include "gbcpp/input.h"

namespace gbcpp {

// One row of the default keyboard / gamepad bindings. Each row names its target
// Button as a typed first member — identity is a field, never an implicit position
// or a trailing comment. The physical source (an SDL scancode or gamepad button) is
// the second member.
struct KeyMapping {
    Button        button;
    SDL_Scancode  key;
};
struct GamepadMapping {
    Button            button;
    SDL_GamepadButton pad;
};

// Default keyboard bindings (Decision #9): D-pad → arrow keys, A → X, B → Z,
// Start → Return, Select → Backspace. Conventional emulator defaults; user rebinding
// layers on top in ENG-5 without changing this surface.
inline constexpr std::array<KeyMapping, kButtonCount> kDefaultKeyMap{{
    { Button::Up,     SDL_SCANCODE_UP },
    { Button::Down,   SDL_SCANCODE_DOWN },
    { Button::Left,   SDL_SCANCODE_LEFT },
    { Button::Right,  SDL_SCANCODE_RIGHT },
    { Button::A,      SDL_SCANCODE_X },
    { Button::B,      SDL_SCANCODE_Z },
    { Button::Start,  SDL_SCANCODE_RETURN },
    { Button::Select, SDL_SCANCODE_BACKSPACE },
}};

// Default gamepad bindings (Decision #10): D-pad → gamepad d-pad, A → south face,
// B → east face, Start → start, Select → back. SDL_Gamepad abstracts physical pads
// to this canonical layout.
inline constexpr std::array<GamepadMapping, kButtonCount> kDefaultGamepadMap{{
    { Button::Up,     SDL_GAMEPAD_BUTTON_DPAD_UP },
    { Button::Down,   SDL_GAMEPAD_BUTTON_DPAD_DOWN },
    { Button::Left,   SDL_GAMEPAD_BUTTON_DPAD_LEFT },
    { Button::Right,  SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
    { Button::A,      SDL_GAMEPAD_BUTTON_SOUTH },
    { Button::B,      SDL_GAMEPAD_BUTTON_EAST },
    { Button::Start,  SDL_GAMEPAD_BUTTON_START },
    { Button::Select, SDL_GAMEPAD_BUTTON_BACK },
}};

// Pure translators over the default tables: return the mapped Button, or nothing for
// an unmapped source. They take SDL enum values directly (not a live device), so the
// mapping suite tests them with no window or gamepad present. These describe the
// fixed default profile; ControlBindings below is the runtime, rebindable form.
[[nodiscard]] std::optional<Button> mapScancode(SDL_Scancode key) noexcept;
[[nodiscard]] std::optional<Button> mapGamepadButton(SDL_GamepadButton pad) noexcept;

// The detected physical-controller family. SDL_Gamepad already normalises face
// buttons across families (South is Cross on a DualSense, A on an Xbox pad), so this
// is NOT needed for input correctness — it drives button-glyph / prompt selection and
// the per-family default profile (defaultsForGamepad below). "Standard" is SDL's
// generic well-mapped pad; "Unknown" is no pad / unrecognised.
enum class ControllerType { Unknown, Xbox, PlayStation, Nintendo, Standard };

// Pure translator: collapse SDL's fine-grained gamepad type into the engine family.
// Tested against SDL enum values with no live device.
[[nodiscard]] ControllerType controllerTypeFrom(SDL_GamepadType type) noexcept;

// Runtime, mutable physical-input → Button bindings for both devices — the
// "configurable controls" surface. Seeded from the canonical defaults; the input path
// (SdlPlatform) consults an instance of this instead of the fixed tables, so the live
// bindings can be replaced wholesale. This is the skeleton: ENG-5 layers the rebinding
// UI, config-file load/save, and live remapping on top of this shape without
// reshaping the input path. The per-Button invariant holds: keys_[i].button and
// pads_[i].button are both static_cast<Button>(i).
class ControlBindings {
public:
    // The canonical default profile (Decisions #9/#10).
    [[nodiscard]] static ControlBindings defaults();

    // The default profile for a detected controller family. SDL normalises the layout,
    // so today every family returns the same canonical gamepad bindings — this is the
    // seam where family-specific default tweaks attach later (ENG-5). The keyboard
    // half is unaffected.
    [[nodiscard]] static ControlBindings defaultsForGamepad(ControllerType type);

    [[nodiscard]] std::optional<Button> fromScancode(SDL_Scancode key) const noexcept;
    [[nodiscard]] std::optional<Button> fromGamepadButton(SDL_GamepadButton pad) const noexcept;

    [[nodiscard]] SDL_Scancode      keyFor(Button button) const noexcept;
    [[nodiscard]] SDL_GamepadButton gamepadButtonFor(Button button) const noexcept;

    void bindKey(Button button, SDL_Scancode key) noexcept;
    void bindGamepadButton(Button button, SDL_GamepadButton pad) noexcept;

private:
    std::array<KeyMapping, kButtonCount>     keys_{};
    std::array<GamepadMapping, kButtonCount> pads_{};
};

}  // namespace gbcpp
