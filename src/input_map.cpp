#include "retropp/input_map.h"

namespace retropp {

std::optional<Button> mapScancode(SDL_Scancode key) noexcept {
    for (const auto& row : kDefaultKeyMap) {
        if (row.key == key) return row.button;
    }
    return std::nullopt;
}

std::optional<Button> mapGamepadButton(SDL_GamepadButton pad) noexcept {
    for (const auto& row : kDefaultGamepadMap) {
        if (row.pad == pad) return row.button;
    }
    return std::nullopt;
}

ControllerType controllerTypeFrom(SDL_GamepadType type) noexcept {
    switch (type) {
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE:
            return ControllerType::Xbox;
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return ControllerType::PlayStation;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return ControllerType::Nintendo;
        case SDL_GAMEPAD_TYPE_STANDARD:
            return ControllerType::Standard;
        default:
            return ControllerType::Unknown;
    }
}

ControlBindings ControlBindings::defaults() {
    ControlBindings b;
    b.keys_ = kDefaultKeyMap;
    b.pads_ = kDefaultGamepadMap;
    return b;
}

ControlBindings ControlBindings::defaultsForGamepad(ControllerType type) {
    ControlBindings b = defaults();
    // SDL reports face buttons POSITIONALLY (SOUTH = bottom, EAST = right, …). Nintendo
    // transposes the labels vs the Xbox layout: the button LABELLED A sits at east (where
    // Xbox's B is), B at south, X at north, Y at west. Bind the engine's logical A/B/X/Y to
    // the Nintendo-labelled buttons so a Switch player's "A" confirms. Xbox / PlayStation /
    // Standard keep the positional layout (south = confirm, like Cross on a DualSense). The
    // keyboard half is family-independent. (Matching button GLYPHS are a separate glyph-layer
    // concern; this function only sets the mapping.)
    if (type == ControllerType::Nintendo) {
        b.bindGamepadButton(Button::A, SDL_GAMEPAD_BUTTON_EAST);
        b.bindGamepadButton(Button::B, SDL_GAMEPAD_BUTTON_SOUTH);
        b.bindGamepadButton(Button::X, SDL_GAMEPAD_BUTTON_NORTH);
        b.bindGamepadButton(Button::Y, SDL_GAMEPAD_BUTTON_WEST);
    }
    return b;
}

std::optional<Button> ControlBindings::fromScancode(SDL_Scancode key) const noexcept {
    for (const auto& row : keys_) {
        if (row.key == key) return row.button;
    }
    return std::nullopt;
}

std::optional<Button> ControlBindings::fromGamepadButton(SDL_GamepadButton pad) const noexcept {
    for (const auto& row : pads_) {
        if (row.pad == pad) return row.button;
    }
    return std::nullopt;
}

SDL_Scancode ControlBindings::keyFor(Button button) const noexcept {
    return keys_[static_cast<std::size_t>(button)].key;  // per-Button invariant
}

SDL_GamepadButton ControlBindings::gamepadButtonFor(Button button) const noexcept {
    return pads_[static_cast<std::size_t>(button)].pad;  // per-Button invariant
}

void ControlBindings::bindKey(Button button, SDL_Scancode key) noexcept {
    keys_[static_cast<std::size_t>(button)] = {button, key};
}

void ControlBindings::bindGamepadButton(Button button, SDL_GamepadButton pad) noexcept {
    pads_[static_cast<std::size_t>(button)] = {button, pad};
}

}  // namespace retropp
