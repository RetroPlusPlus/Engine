# Input

The action-based input system: a game declares its own input vocabulary as an enum, binds each
action to any number of physical sources in an `ActionMap` value, hands the map to the platform, and
reads per-tick action state — digital edges and analog values — keyed by its own enum. The engine
holds no action vocabulary of its own and never filters what a game maps.

```cpp
#include "retropp/input.h"          // ActionSet, InputState, InputSample, ActiveDevice, ControllerType
#include "retropp/input_actions.h"  // ActionMap, Source, PadButton, PadStick, onPad, presets
#include "retropp/analog_input.h"   // AnalogInput, MouseButton, Stick, Trigger (the raw pointer/analog surface)
```

`input.h` is the std-only read side (the run loop includes it); `input_actions.h` is the SDL-coupled
binding side.

## Contents

- [The action model](#the-action-model)
- [Declaring bindings: `ActionMap`](#declaring-bindings-actionmap)
- [Sources](#sources)
- [The pad vocabulary: `PadButton` and `PadStick`](#the-pad-vocabulary-padbutton-and-padstick)
- [Per-family rows: `onPad` and the suppression rule](#per-family-rows-onpad-and-the-suppression-rule)
- [Presets](#presets)
- [Reading input: `InputState`](#reading-input-inputstate)
- [Player slots](#player-slots)
- [The active-device signal](#the-active-device-signal)
- [Pointer & analog input](#pointer--analog-input)
- [Where to change things](#where-to-change-things)

## The action model

```cpp
enum class Action : std::uint8_t { Jump, Fire, Move, Pause };   // YOUR enum, your names
```

An **action** is whatever your game means — `Jump`, `Fire`, `Move`. The engine sees only the
enumerator's integer value (`ActionId`, the bit index into an `ActionSet`); every game-facing API is
templated on your enum (`ActionLike` — any enum or integer) and casts at the surface via
`actionId(a)`. Capacity is `kMaxActions` (64) simultaneous actions per map — a capacity, not a
filter.

```cpp
class ActionSet {   // the actions currently active, one bit per ActionId — a cheap value type
    constexpr void set(ActionId a, bool active) noexcept;
    constexpr bool test(ActionId a) const noexcept;
    constexpr std::uint64_t bits() const noexcept;
    // operator| / |= (union), operator==
};
```

An unmapped engine is silent: with no `ActionMap` handed to the platform, no action is ever
reported. There is no default vocabulary, no gate, and no engine-side input configuration — the map
is a value your game owns.

## Declaring bindings: `ActionMap`

```cpp
ActionMap map{
    {Action::Fire,  {SDL_SCANCODE_X, PadButton::FaceLabelA, MouseButton::Left}},
    {Action::Pause, {SDL_SCANCODE_ESCAPE, PadButton::Start}},
    {Action::Move,  {PadStick::Left}},
};
map.add(presets::directional(Action::Up, Action::Down, Action::Left, Action::Right));
platform.setActions(map);           // hand the value over; the game still owns its copy
```

A map is a flat list of rows, each `(action ← source)`. **Multi-source is multiple rows on one
action** — every listed source is simultaneously live, so keyboard and controller both work with no
mode switch. Build it declaratively (each row is your action plus a brace-list of sources), with
`bind`, or both:

```cpp
class ActionMap {
    ActionMap() = default;                            // empty — no action input
    ActionMap(std::initializer_list<ActionRow>);      // the declarative row form above
    void bind(A action, Source source);               // append one row
    void unbind(A action, Source source);             // remove rows matching (action, source) exactly
    void clearAction(A action);                       // remove every row for the action
    void add(const ActionMap& bundle);                // append another map's rows (presets merge in)
    std::span<const ActionBinding> rows() const noexcept;
};
```

**Updating the live bindings is resubmitting the value.** The platform keeps a replaceable copy of
the last submission; a rebind screen or a gameplay/menu context switch edits the game's own map (or
keeps two) and calls `setActions` again. The swap takes effect at the next event pump, and edges
stay honest across it: an action held through a resubmission that still binds it stays held (no
phantom `justPressed`); unbinding a held source reads `justReleased` on the next tick. Persistence
is game code — serialize your map however you like (see `SaveStore` in
[persistence.md](persistence.md)).

## Sources

A `Source` is one physical input, implicitly constructible from each payload type — which is what
makes the brace-list row form read as a plain list:

| Payload type | Kind | Example |
|---|---|---|
| `SDL_Scancode` | keyboard key | `SDL_SCANCODE_SPACE` |
| `PadButton` | a pad button (device-class) | `PadButton::FaceSouth` |
| `MouseButton` | a mouse button | `MouseButton::Left` |
| `PadStick` | a whole stick, as a 2-D vector | `PadStick::Right` |

Three builders refine a source per row:

```cpp
onPad(ControllerType::Nintendo, PadButton::FaceEast)   // family-qualified (see the suppression rule)
withThreshold(PadButton::TriggerR, 0.6f)               // override an analog source's digital threshold
asComponent(SDL_SCANCODE_W, Dir::Up)                   // a digital source's vector-read contribution
```

`asComponent` tags a digital source with the vector-read axis it contributes to:

```cpp
enum class Dir : std::uint8_t { None, Up, Down, Left, Right };   // Up is -y (screen space)
```

**Any source binds to any action; the read decides the interpretation.** A trigger bound to a
digital action crosses a threshold (`kTriggerThreshold`, 0.30); a stick on a digital read counts as
past-threshold deflection (`kStickDirThreshold`, 0.50); a key on a value read contributes 0/1. The
per-row `withThreshold` override wins over the per-kind default (`sourceThreshold` resolves it).

## The pad vocabulary: `PadButton` and `PadStick`

Bindings target device **classes**, never device instances — `PadButton::FaceSouth` means "a pad's
south button", so swapping controllers mid-game just works. Three naming layers cover every family:

- **Positional cardinals** — `FaceSouth` / `FaceEast` / `FaceWest` / `FaceNorth`. The ground truth;
  SDL reports positions, so these resolve identically on every pad.
- **Label aliases** — `FaceLabelA` / `FaceLabelB` / `FaceLabelX` / `FaceLabelY`: "the button
  **printed** that letter on the connected pad." The same four letters sit at different positions on
  Xbox vs Nintendo pads, so a label alias resolves per pad family at sample time (`resolvePadButton`).
  Families without letter labels (PlayStation, generic) resolve to the Xbox-convention position — the
  button a PlayStation player presses when a game says "Press A".
- **Sony symbol synonyms** — `FaceCross` / `FaceCircle` / `FaceSquare` / `FaceTriangle`: equal-value
  aliases of the cardinals (`FaceCross == FaceSouth`). Sony's symbols are positionally fixed, so
  these are pure spelling.

Everything else has one neutral name; per-family print is a glyph concern (see the active-device
signal):

| Engine name | Xbox | PlayStation | Nintendo |
|---|---|---|---|
| `ShoulderL` / `ShoulderR` | LB / RB | L1 / R1 | L / R |
| `TriggerL` / `TriggerR` (analog-backed) | LT / RT | L2 / R2 | ZL / ZR |
| `StickClickL` / `StickClickR` | LS / RS | L3 / R3 | stick click |
| `Start` / `Select` | Menu / View | Options / Share¹ | Plus / Minus |
| `Guide` | Xbox button | PS button | Home |
| `Share` | Share (Series) | Create / mic (PS5) | Capture |

¹ the PS4's Share sits where `Select` maps (SDL's Back); the PS5's Create is `Share` (SDL's MISC1).

`DpadUp/Down/Left/Right` are the d-pad; `LeftStickUp/Down/Left/Right` and `RightStickUp/…` are
**stick-direction pseudo-buttons** — digital sources that fire when the dead-zoned axis passes the
threshold. `PadStick::Left` / `PadStick::Right` are the whole-stick **vector** sources. The host OS
or Steam may intercept `Guide` before the engine sees it.

## Per-family rows: `onPad` and the suppression rule

A plain pad source applies on every family. A family-qualified source applies **only** on that
family — and it **suppresses** the action's unqualified pad rows there, so one action can name each
family's button explicitly without double-firing:

```cpp
ActionMap map{
    {Action::Confirm, {PadButton::FaceSouth,                              // Xbox / PS / generic
                       onPad(ControllerType::Nintendo, PadButton::FaceEast),  // Switch: printed A
                       SDL_SCANCODE_RETURN}},
    {Action::Cancel,  {PadButton::FaceEast,
                       onPad(ControllerType::Nintendo, PadButton::FaceSouth), // Switch: printed B
                       SDL_SCANCODE_BACKSPACE}},
};
```

On a Switch pad each physical button drives exactly one of the two actions; on every other family
the generic rows apply. The rule, precisely: for a given action on a pad of family F, if any
Pad/Stick row for that action is qualified with F, only those qualified rows sample on that pad;
unqualified Pad/Stick rows serve every family without qualified rows. Key/Mouse rows are never
affected. (`qualifiedFamilyMask` + `padRowAppliesTo` are the testable core of the rule.)

`FaceLabelA` is exactly shorthand for `{onPad(Nintendo, FaceEast), FaceSouth}` — use the alias when
the printed-letter convention is what you want, and explicit qualified rows when it isn't.

`onPad` has both a `PadButton` and a `PadStick` overload, so a whole-stick vector source qualifies to
one family too: `onPad(ControllerType::Nintendo, PadStick::Left)`.

## Presets

A preset is a merge-in bundle of conventional rows for actions **the caller names** — a preset
cannot know your vocabulary, so you supply the action side and it supplies the source side. The
result is ordinary rows you could have written by hand; edit them like any others.

```cpp
namespace presets {
ActionMap directional(A up, A down, A left, A right);  // arrows + WASD + d-pad, digital
ActionMap directionalVector(A move);                   // left stick + arrows/WASD/d-pad as ONE vector
}

map.add(presets::directional(Action::Up, Action::Down, Action::Left, Action::Right));
```

`directionalVector` binds `PadStick::Left` plus the twelve keys/d-pad rows component-tagged
(`asComponent`), so the stick gives proportional deflection and keys give unit steps through the
same `vector()` read. Presets never construct your map — you always construct it and `add` them in.

## Reading input: `InputState`

Your tick callback receives a `const InputState&`: per-tick action state plus edges relative to the
previous tick, and the analog/pointer surface sampled at the same tick.

```cpp
loop.setTick([&](const InputState& in) {
    if (in.isHeld(Action::Left))       hero.walk(-1);      // active this tick (honest level)
    if (in.justPressed(Action::Fire))  spawnBullet();      // pressed since the last tick
    if (in.justReleased(Action::Jump)) hero.cutJump();     // active → inactive this tick
    hero.steer(in.vector(Action::Move));                   // Vec2, [-1,1] per axis
    ship.throttle(in.axis(Action::Throttle));              // float — the value's x component
});
```

Edges are **sim-tick-keyed** — deterministic and frame-rate-independent, never sampled at render
cadence. An action already active on the very first tick reads as `justPressed`.

**A press is never dropped.** `justPressed` fires for any action that went down since the previous
tick — even a tap so quick it was already released by the time the tick sampled (a fast fire-tap
while a direction is held, on a display whose refresh outruns the tick rate). The host pushes the
platform's sample every frame and the run loop keeps the per-slot union of those frames until the
next tick consumes it. `isHeld` and `justReleased` stay honest level edges off the current tick, so
a release is never latched and nothing sticks.

**Value reads.** `vector(a)` returns the action's summed analog value, clamped to [-1, 1] per axis:
stick sources contribute their dead-zoned vectors, component-tagged digital sources ±1 on their
axis, triggers their pull on x. `axis(a)` is the value's x component — the natural read for a
trigger-bound action. Values are absolute (latest at the tick), like the cursor and sticks.

## Player slots

Every device feeds **player slot 0** by default, so single-player games never see slots — the
`InputState` methods above are the `player(0)` view with the index elided. Multiplayer opts in:

```cpp
for (const GamepadInfo& pad : platform.connectedGamepads())   // {id, family, slot}
    std::printf("pad %u (%d)\n", pad.id, static_cast<int>(pad.family));
platform.assignGamepad(padId, /*player=*/1);   // route one pad (by SDL instance id) to a slot
platform.assignKeyboard(1);                    // the keyboard+mouse unit is one assignable device

loop.setTick([&](const InputState& in) {
    if (in.player(0).isHeld(Action::Thrust)) ships[0].thrust();
    if (in.player(1).isHeld(Action::Thrust)) ships[1].thrust();
});
```

`kMaxPlayers` is 4. `player(n)` returns a lightweight const view exposing the same read surface —
digital, values, active device, and the analog/pointer reads — per slot; each slot edges on its own
history. One `ActionMap` serves every slot. Routing is runtime device state on the platform object;
a reconnected pad re-enters at slot 0.

## The active-device signal

```cpp
struct ActiveDevice { DeviceKind kind; ControllerType family; };   // per slot
enum class DeviceKind : std::uint8_t { None, KeyboardMouse, Gamepad };
enum class ControllerType : std::uint8_t { Unknown, Xbox, PlayStation, Nintendo, Standard };
```

`in.activeDevice()` (per slot via `player(n)`) reports which device most recently produced input —
the signal a glyph layer reads to flip "Press Ⓐ" / "Press ✕" / "Press [E]" prompts:

```cpp
const ActiveDevice dev = in.activeDevice();
if (dev.kind == DeviceKind::Gamepad) drawPadGlyph(dev.family, ...);
else                                 drawKeyGlyph(...);
```

It moves on real activity (a bound row landing; key/mouse events; pad buttons or past-dead-zone axis
motion) and persists otherwise. `ControllerType` is the detected physical pad family
(`controllerTypeFrom` collapses SDL's fine-grained type); SDL normalizes button *positions* across
families, so the family is never needed for input correctness — it drives glyphs and the
label-alias/family-qualified resolution.

## Pointer & analog input

Beside the action surface rides the **raw** analog/pointer surface (`analog_input.h`), for reads
that don't want to be actions — an absolute cursor, raw spinner motion, direct stick access:

```cpp
class InputState {   // also on player(n)
    // Pointer (mouse — on the slot the keyboard+mouse unit feeds)
    Vec2i cursor() const noexcept;          // ABSOLUTE position in VIEWPORT pixels
    bool  cursorOnScreen() const noexcept;  // pointer is over the drawn viewport (not a letterbox bar)
    Vec2i cursorDelta() const noexcept;     // viewport-pixel change since the last tick
    float rawDeltaX() const noexcept;       // raw device motion since the last tick (a spinner integrates this)
    float rawDeltaY() const noexcept;
    float wheel() const noexcept;           // wheel delta since the last tick
    bool  mouseHeld(MouseButton) const noexcept;
    bool  mouseJustPressed(MouseButton) const noexcept;   // edges mirror the digital ones
    bool  mouseJustReleased(MouseButton) const noexcept;
    // Gamepad analog (aggregated across the slot's pads, max magnitude per axis)
    Vec2  stick(Stick) const noexcept;      // {x, y} in [-1, 1], dead-zoned
    float trigger(Trigger) const noexcept;  // [0, 1], dead-zoned
};
```

The raw-surface selectors are small enums (`analog_input.h`):

```cpp
enum class MouseButton : std::uint8_t { Left, Right, Middle };
enum class Stick       : std::uint8_t { Left, Right };   // which gamepad stick
enum class Trigger     : std::uint8_t { Left, Right };   // which gamepad trigger
```

**The cursor is in VIEWPORT pixels.** The OS reports the mouse in window pixels; the engine renders
the internal viewport integer-scaled + letterboxed into the window, so the platform inverts that
blit (`windowToViewport` in `geometry.h`) to hand you a coordinate in the same space your sprites
and tiles live in. Gate a reticle on `cursorOnScreen()`.

**Absolute vs relative.** `cursor()` / `cursorDelta()` is the absolute pointer — menus, RTS
selection, a light-gun. `rawDeltaX()/rawDeltaY()` is raw device motion, the thing a rotary spinner
or mouse-look integrates (independent of output scale). Relative quantities (`rawDelta`, `wheel`)
accumulate across every frame between two ticks and reset on the tick, so a fast flick is never lost
even on a frame that produces no tick.

**Relative-capture (spinner / mouse-look).** `platform.setPointerCaptured(true)` hides + confines
the OS cursor and switches motion to relative-only; while captured there is no meaningful absolute
cursor (`cursorOnScreen()` reports false) — read `rawDeltaX()/Y()`. Orthogonal to
`setCursorVisible` (see [platform-and-windowing.md](platform-and-windowing.md)).

Under the hood the platform samples everything into one `InputSample` per pump (per-slot
`PlayerSample`s: the `ActionSet` level, per-action values, the `AnalogInput`, the `ActiveDevice`),
the host pushes it with `RunLoop::setRawInput`, and the loop accumulates per slot until the tick.
That sample is a plain value — tests (and anything else that wants to synthesize input) build one
and feed it directly; `tests/mock_platform.h` is the worked example.

**The live showcase is `examples/input_probe/`** — one block per digital action across every source
kind, the Move/Aim twin-stick vectors, the throttle axis, the active-device swatch, and console edge
prints. Run it with any controller to see the whole surface at once.

## Where to change things

- **Rebind / context-switch controls at runtime:** edit your map value (or keep one per context) and
  `platform.setActions(map)` again — takes effect at the next pump.
- **Persist bindings:** serialize your map yourself; write the bytes through `SaveStore`
  ([persistence.md](persistence.md)).
- **Per-family button choices:** `FaceLabel*` for the printed-letter convention; explicit
  `onPad(family, …)` rows when you want different positions per family.
- **Tune an analog-to-digital feel:** `withThreshold(source, t)` per row; the defaults are
  `kTriggerThreshold` (0.30) and `kStickDirThreshold` (0.50).
- **Add a pad control the vocabulary lacks** (Elite paddles, PS touchpad click): one `PadButton`
  enumerator + one `resolvePadButton` case — additive.
- **Multiplayer:** assign devices to slots (`assignGamepad` / `assignKeyboard`) and read
  `player(n)`; the map is shared.
