# Input

The canonical button surface every consuming port speaks, the per-tick held/edge view the simulation
reads, the default key/pad maps, target-console controller profiles, physical-controller-family
detection, and the runtime-rebindable bindings.

```cpp
#include "gbcpp/input.h"      // Button, ButtonSet, InputState, InputProfile-friendly makeButtonSet
#include "gbcpp/input_map.h"  // default maps, translators, ControllerType, InputProfile, ControlBindings
```

## The buttons

```cpp
enum class Button : std::uint8_t {
    Up, Down, Left, Right,   // 0..3  — d-pad
    A, B,                    // 4,5   — GB / NES / SNES / Genesis / SMS (labelled 1,2)
    X, Y,                    // 6,7   — SNES (and Genesis 6-button)
    L, R,                    // 8,9   — SNES shoulders
    Start, Select,           // 10,11 — GB / NES / SNES (SMS Pause / Reset)
};
inline constexpr int kButtonCount = 12;   // shipped count; bump when appending
```

These are the **logical** buttons a consumer speaks, regardless of the physical device the platform
maps from. The set covers Game Boy / NES (Up..Select) and SNES (adds X, Y, L, R). New buttons
(Genesis C/Z/Mode, …) are **appended** at the end — appending is purely additive (`ButtonSet`'s 32-bit
storage already has room), so adding a button never reshapes a type or breaks ABI; bump `kButtonCount`
when you do. Console-specific *labels* (Master System's "1"/"2" for A/B, etc.) are a glyph-layer
concern — the logical names are fixed (SMS "1" *is* A, Pause *is* Start). The enumerator value is the
bit index in `ButtonSet`.

## `ButtonSet` — a held-state snapshot

```cpp
class ButtonSet {
    constexpr void set(Button b, bool held) noexcept;
    constexpr bool held(Button b) const noexcept;
    constexpr std::uint32_t bits() const noexcept;   // one bit per button
};
constexpr ButtonSet makeButtonSet(std::initializer_list<Button>) noexcept;  // readable mask builder
```

A cheap value type packing the held flags into 32-bit storage — copy it, compare it, snapshot it
freely. The 32 slots are why appending buttons/profiles is forever additive. The platform layer
produces a `ButtonSet` each pump; the host pushes it into the run loop with `RunLoop::setRawInput`.

## `InputState` — what a tick reads

```cpp
class InputState {
    bool isHeld(Button b) const noexcept;        // held this tick
    bool justPressed(Button b) const noexcept;   // released→held this tick (a press edge)
    bool justReleased(Button b) const noexcept;  // held→released this tick (a release edge)
};
```

Your tick callback receives a `const InputState&`: held state plus **edges** relative to the previous
tick. Edges are **sim-tick-keyed** — deterministic and frame-rate-independent, never sampled at render
cadence. A button already held on the very first tick reads as `justPressed` (the correct edge for a
press that landed before tick 1). Use `justPressed` / `justReleased` for menus and "on press" actions;
`isHeld` for movement and held actions.

## Target-console profiles: `InputProfile`

```cpp
struct InputProfile {
    std::string_view name;       // identity, first member — e.g. "SNES"
    ButtonSet        buttons;    // exactly the LOGICAL buttons this controller exposes
    bool      has(Button) const noexcept;
    ButtonSet mask(ButtonSet raw) const noexcept;   // drop buttons this profile doesn't expose

    static const InputProfile GameBoy, Nes, MasterSystem, Snes;
};
```

An `InputProfile` is **which logical buttons the target console's controller has**. The platform
masks its sampled input by the active profile, so a profile only ever reports its own buttons — a
Game Boy profile never reports X/Y/L/R even on a pad that has them. Set it via
`EngineConfig::inputProfile` at startup, or `SdlPlatform::setActiveProfile` at runtime. Presets are
static members (the self-type-constant idiom shared with `ViewportResolution` / `TimingProfile`); a
new console is one additive line. Profiles may share a button *mask* (GameBoy / Nes / MasterSystem are
all the same 8) and differ only in `name` (+ deferred labels); the mask differs only when the button
*set* genuinely differs (SNES adds X/Y/L/R).

> **Two orthogonal axes — don't conflate them.** `InputProfile` = which buttons the *target console*
> has. `ControllerType` (below) = which *physical pad family* is plugged in. They are independent.

## Default bindings

The default keyboard map (the conventional SNES "Z/X + A/S + Q/W" layout) and gamepad map:

| Button | Keyboard | Gamepad (positional) |
|---|---|---|
| Up / Down / Left / Right | Arrow keys | D-pad |
| A | X | South |
| B | Z | East |
| X | S | West |
| Y | A | North |
| L | Q | Left shoulder |
| R | W | Right shoulder |
| Start | Return | Start |
| Select | Backspace | Back |

```cpp
struct KeyMapping     { Button button; SDL_Scancode      key; };  // identity (button) first
struct GamepadMapping { Button button; SDL_GamepadButton pad; };
inline constexpr std::array<KeyMapping, kButtonCount>     kDefaultKeyMap{ ... };
inline constexpr std::array<GamepadMapping, kButtonCount> kDefaultGamepadMap{ ... };

std::optional<Button> mapScancode(SDL_Scancode) noexcept;        // pure translators over the
std::optional<Button> mapGamepadButton(SDL_GamepadButton) noexcept;  //   default tables
```

Each row names its target `Button` as the first member — identity is a field, never an array
position. The translators take SDL enum values directly (not a live device), so they're testable with
no window or gamepad. `nullopt` means the source isn't bound.

## Controller family: `ControllerType`

```cpp
enum class ControllerType { Unknown, Xbox, PlayStation, Nintendo, Standard };
ControllerType controllerTypeFrom(SDL_GamepadType) noexcept;
```

The detected **physical** pad family. SDL already normalizes face buttons across families (South is
Cross on a DualSense, A on an Xbox pad), so this is **not** needed for input correctness — it drives
button-**glyph** selection ("press Ⓐ" vs "press ✕") and the per-family default mapping. `Standard` is
SDL's generic well-mapped pad; `Unknown` is no pad / unrecognized.

## Runtime-rebindable bindings: `ControlBindings`

```cpp
class ControlBindings {
    static ControlBindings defaults();
    static ControlBindings defaultsForGamepad(ControllerType type);

    std::optional<Button> fromScancode(SDL_Scancode) const noexcept;
    std::optional<Button> fromGamepadButton(SDL_GamepadButton) const noexcept;
    SDL_Scancode      keyFor(Button) const noexcept;
    SDL_GamepadButton gamepadButtonFor(Button) const noexcept;
    void bindKey(Button, SDL_Scancode) noexcept;
    void bindGamepadButton(Button, SDL_GamepadButton) noexcept;
};
```

`ControlBindings` is the mutable, per-instance form of the maps — the "configurable controls"
surface. The input path (`SdlPlatform`) consults a live `ControlBindings` instead of the fixed
tables, so a host can replace bindings wholesale (`SdlPlatform::setBindings`) or rebind individual
buttons. Seed it from `defaults()` or `defaultsForGamepad(type)`.

**`defaultsForGamepad(Nintendo)` swaps the face buttons:** a Nintendo pad labels its buttons
transposed vs Xbox — the button **labelled A** sits at the *east* position (where Xbox's B is). So for
a Nintendo family the engine binds logical A→east, B→south, X→north, Y→west, so a Switch player's
labelled **A** confirms. Xbox / PlayStation / Standard keep the positional layout (south = confirm).
The keyboard half is family-independent. The platform applies this **on pad connect**, *unless* the
host has called `setBindings()` (which marks the bindings customized and suppresses the auto-apply, so
a user rebind is never clobbered); a disconnect reverts.

The rebinding UI, config-file load/save, and live remapping flow are planned; they layer on
top of this surface without reshaping it.

## Where to change things

- **Target a different console's button set:** `EngineConfig::inputProfile` (or
  `SdlPlatform::setActiveProfile`).
- **Change a default binding:** edit `kDefaultKeyMap` / `kDefaultGamepadMap`, or at runtime build a
  `ControlBindings` and `setBindings` it.
- **Add a new button (e.g. Genesis C):** append to `Button`, bump `kButtonCount`, add its default-map
  rows, and add it to the profiles that expose it — all additive.
- **Per-family glyphs / labels:** that's the deferred glyph layer keyed by `(InputProfile,
  ControllerType)`.
