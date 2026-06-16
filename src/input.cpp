#include "retropp/input.h"

namespace retropp {

bool InputState::isHeld(Button b) const noexcept {
    return current_.held(b);
}

bool InputState::justPressed(Button b) const noexcept {
    // Fires for a press observed since the previous tick that was not already held then — including a
    // sub-tick tap whose button is no longer held now (current_ released). A button held continuously
    // is in pressed_ AND in previous_, so it fires only on the first tick. This is the press-buffering
    // that stops a quick fire-tap (while a direction is held) from being dropped — see input.h.
    return pressed_.held(b) && !previous_.held(b);
}

bool InputState::justReleased(Button b) const noexcept {
    // Honest level falling edge off the current tick — a release is reported when the level shows
    // released, never latched, so a direction can't stick.
    return !current_.held(b) && previous_.held(b);
}

Vec2i InputState::cursor() const noexcept {
    return analog_.cursor;
}

bool InputState::cursorOnScreen() const noexcept {
    return analog_.cursorOnScreen;
}

Vec2i InputState::cursorDelta() const noexcept {
    // Per-tick viewport-space motion: the difference of two latest-at-tick absolute positions. (Raw
    // device delta — rawDeltaX/Y — is the accumulated relative measure a spinner integrates instead.)
    return Vec2i{analog_.cursor.x - analogPrev_.cursor.x,
                 analog_.cursor.y - analogPrev_.cursor.y};
}

float InputState::rawDeltaX() const noexcept { return analog_.rawDeltaX; }
float InputState::rawDeltaY() const noexcept { return analog_.rawDeltaY; }
float InputState::wheel() const noexcept { return analog_.wheel; }

bool InputState::mouseHeld(MouseButton b) const noexcept {
    return analog_.mouseDown(b);
}

bool InputState::mouseJustPressed(MouseButton b) const noexcept {
    return analog_.mouseDown(b) && !analogPrev_.mouseDown(b);
}

bool InputState::mouseJustReleased(MouseButton b) const noexcept {
    return !analog_.mouseDown(b) && analogPrev_.mouseDown(b);
}

Vec2 InputState::stick(Stick s) const noexcept {
    return s == Stick::Left ? Vec2{analog_.leftX, analog_.leftY}
                            : Vec2{analog_.rightX, analog_.rightY};
}

float InputState::trigger(Trigger t) const noexcept {
    return t == Trigger::Left ? analog_.triggerL : analog_.triggerR;
}

void InputState::sampleTick(ButtonSet held, ButtonSet pressedSinceTick,
                            const AnalogInput& analog) noexcept {
    previous_   = current_;
    current_    = held;
    pressed_    = pressedSinceTick;
    analogPrev_ = analog_;
    analog_     = analog;
}

}  // namespace retropp
