#include "retropp/input.h"

namespace retropp {

bool InputState::isHeld(Button b) const noexcept {
    return current_.held(b);
}

bool InputState::justPressed(Button b) const noexcept {
    return current_.held(b) && !previous_.held(b);
}

bool InputState::justReleased(Button b) const noexcept {
    return !current_.held(b) && previous_.held(b);
}

void InputState::sampleTick(ButtonSet raw) noexcept {
    previous_ = current_;
    current_ = raw;
}

}  // namespace retropp
