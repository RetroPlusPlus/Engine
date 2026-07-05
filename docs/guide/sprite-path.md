# Sprite path

A helper for **driving a sprite along a curve while it rotates, scales, and animates** — one game-owned
cursor that composes movement, orientation, and the concurrent tracks off a single clock and writes the
result into a [`Sprite`](draw-state.md). Where a [path walker](path-walker.md) resolves elapsed ticks → a
position and a facing and nothing else, a **sprite path** takes that movement and composes it with rotation
and scale [tween](tween.md) tracks, a frame [animation](animation.md), and a facing policy, then writes the
whole thing into a sprite in one call. It is the orchestrator one level up from the path walker.

It stays in the same family shape: the content is pure data (`SpritePathNode`), the runtime state lives in
your object (`SpritePath`), and the engine adds **no state and no new render path** — a sprite path just
fills in a `Sprite` you submit, exactly as if you had written the fields by hand each frame.

```cpp
#include "retropp/sprite_path.h"   // SpritePathMove, FacingPolicy, SpritePathNode, SpritePathSample, SpritePath
using namespace std::chrono_literals;
```

## Contents

- [The family](#the-family)
- [The model](#the-model)
- [`SpritePathMove` — where a node travels](#spritepathmove--where-a-node-travels)
- [`FacingPolicy` — how travel orients the sprite](#facingpolicy--how-travel-orients-the-sprite)
- [`SpritePathNode` — the movement plus the tracks](#spritepathnode--the-movement-plus-the-tracks)
- [`SpritePath` — the game-owned cursor](#spritepath--the-game-owned-cursor)
  - [Timing default](#timing-default)
  - [Lazy bake and re-pathing](#lazy-bake-and-re-pathing)
- [Writing into a sprite — `applyTo`](#writing-into-a-sprite--applyto)
- [Reading the raw sample](#reading-the-raw-sample)
- [Where to change things](#where-to-change-things)

## The family

`SpritePath` sits atop the same players the rest of the guide describes — it resolves each of its tracks
through their pure resolvers off one clock:

| Data (pure, no clock) | Player (game-owned cursor) | Resolves elapsed ticks → |
|---|---|---|
| [`Tween<T>`](tween.md) | `TweenPlayer<T>` | a value |
| [`Animation`](animation.md) | `AnimationPlayer` | a frame |
| [`Curve`](curve.md) / `ArcLengthTable` | [`PathWalker`](path-walker.md) | a position + facing |
| `SpritePathNode` | **`SpritePath`** | a whole sprite (position + orientation + scale + frame) |

A path walker moves *something*; a sprite path moves *a sprite* — and rotates, scales, and animates it at
the same time, from one `advance()` call.

## The model

Without this layer, a game driving a walking, spinning, breathing sprite along a curve runs a walker, two
tween resolvers, an animation player, a tangent-to-angle conversion, and several field writes by hand every
tick. `SpritePath` is that whole bundle behind one cursor. Three pieces:

- **`SpritePathNode`** — pure data describing one leg of motion: where it travels (`SpritePathMove`), how
  fast ([`PathPacing`](path-walker.md#pathpacing--the-time--distance-driver)), which way it faces
  (`FacingPolicy`), and the optional rotation / scale / animation tracks that run alongside the move.
- **`SpritePathSample`** — the composed per-tick result as raw values (position, facing, total rotation in
  degrees, scale, flipX, the current frame, resolved distance, and whether the movement finished).
- **`SpritePath`** — the game-owned cursor: it holds the elapsed-tick counter and the playback controls, and
  each tick it composes the sample and lets you write it into a `Sprite`.

A path plays **one node**. Everything below composes that node.

## `SpritePathMove` — where a node travels

A movement spec, resolved to a [`Curve`](curve.md) when the node starts. It has four forms, each with a
named constructor (aggregate initialization works too and is interchangeable):

```cpp
struct SpritePathMove {
    enum class Kind : std::uint8_t { Line, ThroughPoints, Hermite, Curve };
    // …fields…
    static SpritePathMove to(Vec2 destination);                          // a straight line
    static SpritePathMove through(std::vector<Vec2> points);             // a Catmull-Rom through the points
    static SpritePathMove hermite(Vec2 destination, Vec2 originTangent, Vec2 destinationTangent);
    static SpritePathMove onCurve(Curve c);                              // a pre-authored curve, verbatim
    // …each (except onCurve) also has a leading-origin overload…
};
```

- **`to(destination)`** — a straight line from the origin to `destination`.
- **`through(points)`** — a smooth [Catmull-Rom](curve.md) curve travelling through the listed points in
  order, ending at the last one.
- **`hermite(destination, originTangent, destinationTangent)`** — a cubic leaving the origin along
  `originTangent` and arriving at `destination` along `destinationTangent` (a directional vector *is* a
  tangent).
- **`onCurve(curve)`** — travel a curve you already built (Bézier handles, a loop, anything a `Curve` can be).

**Origin defaulting.** The origin is only known when the node starts, so for `to` / `through` / `hermite` it
defaults to where the path begins (`SpritePath::start`). Pass an explicit origin with the leading-`Vec2`
overload (`to(origin, destination)`). `onCurve` is the exception — it starts wherever its own geometry
starts, so origin defaulting does not apply.

## `FacingPolicy` — how travel orients the sprite

```cpp
enum class FacingPolicy : std::uint8_t { None, FlipX, RotateToFacing };
```

- **`None`** (default) — travel does not touch the sprite's orientation.
- **`FlipX`** — the sprite mirrors (`flipX = true`) while it travels toward −x. It **holds** its previous
  mirror state while the horizontal component of travel is zero, so purely vertical motion never flip-flops a
  mirrored sprite.
- **`RotateToFacing`** — the sprite rotates to point along travel (art is assumed authored facing +x). This
  angle **sums** with the rotation track below, so a sprite can nose along its path *and* spin.

The raw `facing()` stays available whichever policy is set, so a game can always compute its own orientation.

## `SpritePathNode` — the movement plus the tracks

```cpp
struct SpritePathNode {
    std::string_view            label;                     // optional symbolic id
    SpritePathMove              move;                      // where it travels
    PathPacing                  pacing;                    // how fast (default: parked)
    FacingPolicy                facing = FacingPolicy::None;
    std::optional<Tween<float>> rotationDegrees;           // rotation track (absent = none)
    PlaybackMode                rotationMode = PlaybackMode::single();
    std::optional<Tween<Vec2>>  scale;                     // scale track (absent = none)
    PlaybackMode                scaleMode = PlaybackMode::single();
    const Animation*            animation = nullptr;       // frames track (game-owned)
    PlaybackMode                animationMode = PlaybackMode::loopIndefinitely();
    std::optional<Vec2>         pivot;                     // rotation/scale pivot (absent = sprite centre)
};
```

A node is **pure data** — copyable, self-contained, with no runtime state. The rotation and scale tracks are
held **by value** ([`Tween`](tween.md) values are small and copyable); the `Animation` is held **by pointer**
(a shared asset the game owns for the cursor's lifetime), as is a [`DistanceTween`](path-walker.md#pathpacing--the-time--distance-driver)
pacing profile.

Each track resolves against the *same* elapsed-tick clock as the movement, under its own
[`PlaybackMode`](animation.md#playbackmode--how-it-plays-chosen-when-you-play-it). An **absent** track (a
`std::nullopt` or a null `animation`) contributes identity — no rotation, unit scale, no frame change — which
is distinct from a present track resting at its final value. The track defaults suit the common case: the
tween tracks play once (`single()`), and the animation loops (`loopIndefinitely()`).

## `SpritePath` — the game-owned cursor

The "just play it" cursor over one node. **State lives here, in your object — not in the engine.** You
construct it, call `advance()` each sim tick, and either write it into a sprite with `applyTo` or read the
composed sample. The same control surface as the other players.

```cpp
struct SpritePath {
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    SpritePathNode   node;                   // the one node this path plays
    Vec2             start;                   // where the path begins when the move names no origin
    TimingProfile    profile = defaultTiming;
    std::uint64_t    elapsedTicks = 0;
    bool             playing = true;

    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(), std::uint64_t deltaTicks = 1);
    void applyTo(Sprite& s) const;

    Vec2  position() const;   Vec2  facing()  const;   float rotationDegrees() const;
    Vec2  scaleValue() const; bool  flipX()   const;   const AnimationFrame* frame() const;
    float distance() const;   bool  finished() const;

    void play();     void pause();    void stop();     void restart();
    void seek(std::chrono::nanoseconds at);
};
```

`advance()` accrues `elapsedTicks` **only while `playing`** and re-composes the sample under `mode` (default
`loopIndefinitely()`, so a bare `advance()` loops the *movement*; the tracks resolve at the same clock under
their own per-node modes). `stop()` pauses and rewinds to the node start; `restart()` rewinds and resumes;
`seek` jumps to a **wall-time offset** (resolved to ticks via the profile). `finished()` reports whether the
**movement** finished — the tracks are subordinate and never gate completion.

### Timing default

`SpritePath::defaultTiming` is the cadence a bare-constructed path resolves pacing and track durations
against. [`EngineConfig::setActive`](platform-and-windowing.md) fans the configured cadence into it at
startup, alongside the other players' `defaultTiming`, so a bare path inherits the engine cadence with
nothing extra to type. Assign it directly at any time, or override one path via `.profile`.

### Lazy bake and re-pathing

The movement spec is baked to an [`ArcLengthTable`](curve.md#arclengthtable) **lazily on the first
`advance()`** (and re-baked by `stop()` / `restart()`), because designated initialization cannot run a bake
and the start anchor is only known then. To send a mover down a different path, assign a new node and
`restart()`:

```cpp
mover.node = SpritePathNode{.move = SpritePathMove::to({120.0f, 40.0f}), .facing = FacingPolicy::RotateToFacing};
mover.restart();   // re-bakes the new node's geometry
```

## Writing into a sprite — `applyTo`

`applyTo(sprite)` writes exactly the fields the node declares, and leaves everything else as the game set it:

| Sprite field | Written when | Value |
|---|---|---|
| `x` / `y` | always | `std::lround(position)` — the one quantize point (the path's math is float end-to-end) |
| `atlas` / `tile` / `size` / `palette` | an animation track is present | the current frame's art |
| `transform` | a rotation track, a scale track, or `RotateToFacing` is declared | scale, then rotation, about the pivot (`node.pivot`, or the sprite's centre using the size after the frame write) |
| `flipX` | `FacingPolicy::FlipX` | the mirror state from travel |
| `key`, `alpha`, `flipY`, the 90° texture `rotation` | never | left untouched — the game's |

So a sprite already carrying a `key`, an `alpha`, and its own art keeps all of them; `applyTo` only moves,
orients, and (if the node animates) re-arts it.

```cpp
SpritePath mover{.node = {.move   = SpritePathMove::to({140.0f, 40.0f}),
                          .pacing = PathPacing::speed(30.0f),
                          .facing = FacingPolicy::RotateToFacing},
                 .start = {20.0f, 40.0f}};

loop.setTick([&](const InputState&) { mover.advance(); });

loop.setRender([&]() {
    Sprite s{.key = "mover"};
    s.atlas = arrowAtlas;   // art the node doesn't animate is set by the game
    s.palette = arrowPalette;
    mover.applyTo(s);       // writes position + the RotateToFacing transform
    // … submit s in a sprite layer …
});
```

Interpolation (default on) composes for free: `applyTo` writes into a game-keyed sprite each tick and the
interpolator eases between ticks by [`ObjectKey`](draw-state.md).

## Reading the raw sample

When you want to place the result yourself — a shadow offset from the mover, a trail, a HUD readout — read
the composed values directly instead of calling `applyTo`:

```cpp
const Vec2 p = mover.position();       // float position
Sprite shadow{.key = "moverShadow"};
shadow.atlas = shadowAtlas;
shadow.palette = shadowPalette;
shadow.x = int(std::lround(p.x)) + 2;  // your own offset + quantize
shadow.y = int(std::lround(p.y)) + 3;
```

`applyTo` and the getters agree on every shared value — `applyTo` is the common-case write, the getters are
the composed values when you want to place them differently. The sample carries raw values (no pre-composed
`Transform`) because the rotation/scale pivot defaults to the sprite's centre, and the sprite's size is only
known at the write.

## Where to change things

- **Walk a sprite along a line / smooth path / arc:** a `SpritePath` whose node's `move` is
  `to` / `through` / `hermite`, `advance()` it each tick, `applyTo` a sprite.
- **Ride a pre-built curve:** `SpritePathMove::onCurve(myCurve)`.
- **Pace it — constant speed, eased, or a reversing profile:** the node's
  [`PathPacing`](path-walker.md#pathpacing--the-time--distance-driver), exactly as a path walker.
- **Face travel:** set `FacingPolicy::FlipX` (mirror) or `RotateToFacing` (rotate).
- **Spin or scale it while it moves:** give the node a `rotationDegrees` and/or `scale`
  [`Tween`](tween.md) track, each with its own `PlaybackMode`; pick the pivot with `node.pivot` (default is
  the sprite's centre).
- **Animate its art as it moves:** point `node.animation` at an [`Animation`](animation.md).
- **Send it down a new path:** assign a new `node` and `restart()`.
- **Use a non-GBC cadence:** `EngineConfig::setActive` seeds it at startup, or set `SpritePath::defaultTiming`,
  or one path's `.profile`.

A worked example — seven movers each exercising one part of the orchestrator — is in
[`examples/sprite_path_demo/`](../../examples/sprite_path_demo/main.cpp).

> **Photosensitivity:** keep movers slow and their motion monotonic. Pace them in seconds, not frames, so
> they drift rather than jump.
