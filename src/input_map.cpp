#include "gbcpp/input_map.h"

namespace gbcpp {

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

ControlBindings ControlBindings::defaultsForGamepad(ControllerType /*type*/) {
    // SDL normalises face buttons across families, so the canonical gamepad layout is
    // correct for every family today. The parameter is the seam for family-specific
    // default tweaks (e.g. a future swap of A/B confirm-cancel convention) — applied
    // here in ENG-5, not now.
    return defaults();
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

}  // namespace gbcpp
