#pragma once

#include <array>
#include <optional>
#include <string_view>

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

// Default keyboard bindings: D-pad → arrows; the conventional SNES "Z/X + S/A + Q/W"
// face/shoulder layout — A → X, B → Z, X → S, Y → A, L → Q, R → W; Start → Return,
// Select → Backspace. Rows are in Button-enumerator order (index == button value — the
// per-Button invariant ControlBindings relies on). User rebinding layers on top in ENG-5.
inline constexpr std::array<KeyMapping, kButtonCount> kDefaultKeyMap{{
    { Button::Up,     SDL_SCANCODE_UP },
    { Button::Down,   SDL_SCANCODE_DOWN },
    { Button::Left,   SDL_SCANCODE_LEFT },
    { Button::Right,  SDL_SCANCODE_RIGHT },
    { Button::A,      SDL_SCANCODE_X },
    { Button::B,      SDL_SCANCODE_Z },
    { Button::X,      SDL_SCANCODE_S },
    { Button::Y,      SDL_SCANCODE_A },
    { Button::L,      SDL_SCANCODE_Q },
    { Button::R,      SDL_SCANCODE_W },
    { Button::Start,  SDL_SCANCODE_RETURN },
    { Button::Select, SDL_SCANCODE_BACKSPACE },
}};

// Default gamepad bindings: D-pad → gamepad d-pad; A → south, B → east, X → west,
// Y → north face buttons; L/R → left/right shoulders; Start → start, Select → back.
// SDL_Gamepad abstracts physical pads to this canonical POSITIONAL layout — the per-family
// label/position variation (e.g. Nintendo's labelled A sits in the east position) is a
// defaultsForGamepad(ControllerType) + glyph concern, not this table. Rows in enumerator order.
inline constexpr std::array<GamepadMapping, kButtonCount> kDefaultGamepadMap{{
    { Button::Up,     SDL_GAMEPAD_BUTTON_DPAD_UP },
    { Button::Down,   SDL_GAMEPAD_BUTTON_DPAD_DOWN },
    { Button::Left,   SDL_GAMEPAD_BUTTON_DPAD_LEFT },
    { Button::Right,  SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
    { Button::A,      SDL_GAMEPAD_BUTTON_SOUTH },
    { Button::B,      SDL_GAMEPAD_BUTTON_EAST },
    { Button::X,      SDL_GAMEPAD_BUTTON_WEST },
    { Button::Y,      SDL_GAMEPAD_BUTTON_NORTH },
    { Button::L,      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
    { Button::R,      SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
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

// A named TARGET-CONSOLE controller layout: which LOGICAL buttons a console's controller
// exposes, in the generalized names (A/B/X/Y/L/R/Start/Select). This is ONE of two
// orthogonal axes — keep it distinct from ControllerType above:
//   * InputProfile   = which buttons the TARGET console has (this type).
//   * ControllerType = which PHYSICAL pad family is plugged in — drives glyphs and the
//     per-family default mapping. The "Nintendo's A is the east button" SOUTH↔EAST swap
//     lives in defaultsForGamepad(ControllerType), never here.
// Per-button DISPLAY LABELS that vary by console/brand (Master System prints "1"/"2" for
// A/B and mounts Pause/Reset on the console; GB prints "A"/"B"; Nintendo swaps A/B vs Xbox)
// are presentation metadata keyed by (InputProfile, ControllerType) — glyph-layer work,
// deferred. The logical names never change: SMS 1/2 ARE A/B, Pause IS Start, Reset IS Select.
//
// Presets are static members (the self-type-constant idiom shared with ViewportResolution /
// TimingProfile); a NEW console is one additive inline-constexpr line below — never a reshape.
// Profiles may share an identical button MASK (GameBoy / Nes / MasterSystem are all the same
// 8); they differ only in `name` + (deferred) labels. The mask differs only when the button
// SET genuinely differs (SNES adds X/Y/L/R).
struct InputProfile {
    std::string_view name;       // identity, first member — e.g. "SNES"
    ButtonSet        buttons;    // exactly the LOGICAL buttons this controller has

    [[nodiscard]] constexpr bool has(Button b) const noexcept { return buttons.held(b); }

    // Drop every held button this profile does NOT expose. The platform applies this to its
    // sampled input so a profile only ever reports its own buttons (a Game Boy profile never
    // reports X/Y/L/R even on a pad that has them). Pure + constexpr over the existing ButtonSet
    // surface — no new ButtonSet API, no input.h change; trivially headless-testable.
    [[nodiscard]] constexpr ButtonSet mask(ButtonSet raw) const noexcept {
        ButtonSet out;
        for (int i = 0; i < kButtonCount; ++i) {
            const auto b = static_cast<Button>(i);
            if (raw.held(b) && has(b)) out.set(b, true);
        }
        return out;
    }

    static const InputProfile GameBoy;
    static const InputProfile Nes;
    static const InputProfile MasterSystem;
    static const InputProfile Snes;
    // add new console profiles here — purely additive (Genesis 3-/6-button adds C/Z/Mode; …)
};

inline constexpr InputProfile InputProfile::GameBoy{
    "Game Boy", makeButtonSet({Button::Up, Button::Down, Button::Left, Button::Right,
                               Button::A, Button::B, Button::Start, Button::Select})};
inline constexpr InputProfile InputProfile::Nes{
    "NES", makeButtonSet({Button::Up, Button::Down, Button::Left, Button::Right,
                          Button::A, Button::B, Button::Start, Button::Select})};
inline constexpr InputProfile InputProfile::MasterSystem{
    // 1→A, 2→B, Pause→Start, Reset→Select — the same 8 as GB/NES; "1"/"2"/Pause/Reset are labels.
    "Master System", makeButtonSet({Button::Up, Button::Down, Button::Left, Button::Right,
                                    Button::A, Button::B, Button::Start, Button::Select})};
inline constexpr InputProfile InputProfile::Snes{
    "SNES", makeButtonSet({Button::Up, Button::Down, Button::Left, Button::Right,
                           Button::A, Button::B, Button::X, Button::Y,
                           Button::L, Button::R, Button::Start, Button::Select})};

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
