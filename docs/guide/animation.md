# Animation

A small helper layer for playing **frame-based animations** — walk cycles, palette cycling, effect
flipbooks — over the engine's immediate-mode draw model. It adds three things: an `Animation` value
type, a **pure stateless resolver** that turns elapsed ticks into the current frame, and a game-owned
`AnimationPlayer` cursor that does the bookkeeping for you. It introduces **no engine state and no new
render path** — a frame still reaches the screen the same way any sprite or tile does, by being threaded
into `FrameDrawState` each tick.

The art a frame points at comes from [images-and-transparency.md](images-and-transparency.md) (an atlas
+ slots); the colour it carries is a palette from [tiles-and-colour.md](tiles-and-colour.md); the
submission types it feeds (`Sprite`, `TileCell`) are in [draw-state.md](draw-state.md). For tweening a
*value* over time (a fade, a ramp) rather than stepping through *frames*, see [tween.md](tween.md).

```cpp
#include "retropp/animation.h"   // Animation, AnimationFrame, PlaybackMode, AnimationPlayer
using namespace std::chrono_literals;
```

## The model

The engine recomputes `FrameDrawState` whole every tick, so animation is *already* possible by hand:
resubmit a fresh frame each tick with a different `Sprite::tile` or `TileCell::palette`. This page's API
removes the bookkeeping — it does not add a capability. Two animation styles fall out of **one** frame
shape:

- **Frame animation** — vary the *art* across frames (a walk cycle).
- **Palette cycling** — hold the art constant and vary the *palette* (a shimmer). Palette is an
  independent per-frame field, so this is not a separate mechanism — just a different field changing.
- **Both at once** — vary art *and* palette per frame.

## `AnimationFrame` — one unit of an animation

```cpp
struct AnimationFrame {
    std::string_view         label;      // optional symbolic id (empty = unnamed); identity, first member
    AtlasId                  atlas;      // which uploaded atlas this frame's art lives in
    AssetSlot                slot;       // { tile, dimensions } — feed to Sprite or TileCell
    PaletteId                palette;    // this frame's palette → enables palette cycling
    std::chrono::nanoseconds duration;   // how long this frame shows (real time; resolved to ticks)
};
```

Pure data — it references renderer resources by handle and carries no draw-state logic. The art reference
is an `AtlasId` + an `AssetSlot` (the `{ tile, dimensions }` pair a slicer produces). **You rarely write
`atlas` and `slot` by hand** — the normal way to build a frame is `AtlasManifest::frame`, which takes a
sheet + a cell index and fills both for you (see **Building the frames** below). Because each frame names
its own atlas, different frames may live in different atlases — frames compose freely from a sliced sheet
*and* arbitrary one-off images. Because each frame names its own palette, palette cycling is the same type.

Durations are written in real time (`std::chrono`) and resolved to whole sim ticks at playback against
the timing profile — tick-quantized playback is the honest granularity for a fixed-step sim, and the
engine never stores ticks itself.

## `Animation` — the ordered frame list

```cpp
struct Animation {
    std::vector<AnimationFrame> frames;

    std::size_t           count() const;
    const AnimationFrame& operator[](std::size_t i) const;          // raw index

    std::optional<std::size_t> indexOf(std::string_view name) const; // by label; nullopt if absent
    const AnimationFrame*      find(std::string_view name) const;    // by label; nullptr if absent
};
```

Pure data plus access. **No playback state and no loop policy live here** — *how* an animation plays is
chosen at play time (next section), never baked into the asset, so the same `Animation` plays once in one
place and loops in another. Labels are programmatic ids; keep them unique within an animation (like a
`LayerId` within a frame). Both ways to obtain a frame — time-driven playback and direct selection
(`operator[]` / `find`) — resolve to the same `AnimationFrame`.

### Building the frames

`AtlasManifest::frame` is the way you build a frame: **assign a sheet, give a cell index**, and the
frame's `atlas` and `slot` fill in from `sheet[index]` automatically. You set only the index, a palette, a
duration, and an optional label:

```cpp
const Animation walk{{ sheet.frame(0, pal, 120ms, "step0"),
                       sheet.frame(1, pal, 120ms, "step1") }};
```

`sheet.frame(cell, …)` is just shorthand for an explicit `AnimationFrame` literal — it sets `.atlas` to the
sheet's and `.slot` to `sheet[cell]`. Written by hand, the same `walk` is:

```cpp
const Animation walk{{
    {.label = "step0", .atlas = sheet.atlas, .slot = sheet[0], .palette = pal, .duration = 120ms},
    {.label = "step1", .atlas = sheet.atlas, .slot = sheet[1], .palette = pal, .duration = 120ms},
}};
```

The explicit literal earns its keep when **frames come from different atlases** — each frame names its own
`.atlas`, so a sliced walk sheet and a separate effect sheet live in one animation:

```cpp
const Animation mixed{{
    {.label = "walk",  .atlas = walkSheet,  .slot = walkSheet[0],  .palette = pal, .duration = 120ms},
    {.label = "flash", .atlas = flashSheet, .slot = flashSheet[0], .palette = pal, .duration =  80ms},
}};
```

You can reach the same per-sheet result with the shorthand — `walkSheet.frame(0, pal, 120ms)`,
`flashSheet.frame(0, pal, 80ms)` — since each manifest fills its own atlas. The raw literal is the only way
when you hold an `AtlasId` + `AssetSlot` that didn't come from a manifest at all.

A sheet that holds **several animations** (e.g. one row per facing direction) loads with
`ContentKind::AnimationSeries` and a `framesPerAnimation` count; the returned `AtlasManifest` then
partitions its slots into per-animation runs, and `manifest.group(g)` hands you the `g`-th animation's
slots in read order — feed each run (with the atlas, a palette, and a duration) into an `Animation`. See
the slicer in [images-and-transparency.md](images-and-transparency.md#slicing).

## `PlaybackMode` — how it plays, chosen when you play it

```cpp
struct PlaybackMode {
    enum class Kind { Single, LoopNTimes, LoopIndefinitely, PlayForDuration };
    Kind                     kind = Kind::LoopIndefinitely;   // identity, first member
    std::uint32_t            loopCount;                       // LoopNTimes: number of full passes
    std::chrono::nanoseconds duration;                        // PlayForDuration: total wall-time

    static PlaybackMode single();                                    // play once, hold the final frame
    static PlaybackMode loopNTimes(std::uint32_t n);                 // loop n passes, then hold final
    static PlaybackMode loopIndefinitely();                          // loop forever (default)
    static PlaybackMode playForDuration(std::chrono::nanoseconds d); // loop for d, then stop
};
```

The playback policy is supplied at play time, not stored on the asset (a policy on the asset would be a
second source of truth). The two data-bearing modes carry their payload (`loopCount`, `duration`) via the
named constructors. `loopIndefinitely()` is the default.

## The pure resolver

```cpp
std::uint64_t  totalTicks(const Animation&, const TimingProfile&);
PlaybackState  playbackAt(const Animation&, std::uint64_t elapsedTicks,
                          const TimingProfile&, PlaybackMode);
const AnimationFrame& frameAt(const Animation&, std::uint64_t elapsedTicks,
                              const TimingProfile&, PlaybackMode);   // precondition: count() > 0

struct PlaybackState {
    std::size_t frameIndex = 0;     // the frame to show now
    bool        finished   = false; // a finite mode has reached its end
};
```

`playbackAt` is **the single source of playback truth** — `AnimationPlayer` is stateful sugar over it.
Given elapsed ticks and a mode it returns which frame to show now and whether playback has ended:

| Mode | Behavior |
|---|---|
| `LoopIndefinitely` | `elapsed` modulo one pass; never `finished`. |
| `Single` | First pass, then holds the final frame; `finished` once `elapsed ≥ totalTicks`. |
| `LoopNTimes(n)` | Wraps for `n` passes, then holds the final frame; `finished` once `elapsed ≥ n·totalTicks`. |
| `PlayForDuration(d)` | Wraps until `elapsed ≥ ticksForDuration(d)`, then holds the frame shown at the cutoff; `finished` past `d`. |

`totalTicks` is the length of one pass — the sum of each frame's duration resolved to ticks (`0` frames →
`0`). Two edge cases are handled so playback never stalls: a **zero-tick frame** (a duration that rounds
to 0 ticks) is skipped over — never the resting frame and never a stall point — and an **empty animation**
resolves to `{ frameIndex 0, finished true }`. If *every* frame is instantaneous (total length rounds to
0 ticks), a finite mode is immediately `finished` and an indefinite loop simply rests on the last
positive-length frame.

`TimingProfile` is a pass-by-value host config (see
[run-loop-and-timing.md](run-loop-and-timing.md)), so the resolver is pure — same inputs, same frame.

## `AnimationPlayer` — the game-owned cursor

The "just play it" wrapper. **State lives here, in your object — not in the engine.** You construct it,
call `advance()` each sim tick, and thread `current()` into draw state. The engine provides the type; you
own the instance, exactly like a `std::vector`. The renderer never sees it.

```cpp
struct AnimationPlayer {
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    const Animation* animation = nullptr;        // game-owned; must outlive the player
    TimingProfile    profile   = defaultTiming;  // resolves durations → ticks
    std::uint64_t    elapsedTicks = 0;
    bool             playing = true;

    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(),
                 std::uint64_t deltaTicks = 1);  // accrues ticks (only while playing) + re-resolves

    const AnimationFrame& current() const;       // (*animation)[current frame]; precondition: count() > 0
    std::size_t           currentIndex() const;
    bool                  finished() const;

    void play();                          // resume
    void pause();                         // freeze on the current frame
    void stop();                          // pause + rewind to frame 0
    void restart();                       // rewind + play
    void seek(std::size_t frameIndex);    // jump to a frame's start (no-op if out of range)
    void seek(std::string_view label);    // jump by label (no-op if absent)
};
```

`advance()` accrues `elapsedTicks` **only while `playing`** and re-resolves through `playbackAt` under
`mode` (default `loopIndefinitely()`, so a bare `advance()` just loops — pass `single()` / `loopNTimes(n)`
/ `playForDuration(d)` for the others). A null `animation` makes `advance()` a no-op. `stop()` pauses and
rewinds to frame 0 (not finished); `restart()` rewinds and resumes.

### Timing default

`AnimationPlayer::defaultTiming` is the cadence a bare-constructed player resolves durations against.
`EngineConfig::setActive(config)` at startup fans the configured cadence into it (alongside
`RunLoop::defaultTiming` and `Renderer::defaultViewport`), so a bare `AnimationPlayer{.animation = &a}`
inherits the engine cadence with nothing extra to type. You can also assign it directly at any time
(`AnimationPlayer::defaultTiming = loop.timing();`), or override a single player by setting its `.profile`
field. It is a single process-wide default — legitimate here because the engine is single-threaded by
design and this is a config default, not retained render state.

### Threading a frame into draw state

The player resolves *which* frame; you write that frame into your `Sprite` (or `TileCell`) each render:

```cpp
AnimationPlayer p{.animation = &walk};   // bare — inherits defaultTiming

loop.setTick([&](const InputState&) { p.advance(); });          // loops by default

loop.setRender([&](float) {
    const AnimationFrame& f = p.current();
    sprite.tile    = f.slot.tile;        // the frame's art
    sprite.size    = f.slot.dimensions;
    palSet[0]      = f.palette;          // the frame's palette into the layer's set
    sprite.palette = 0;
    // … submit the layer …
});
```

To **pin** a frame with no playback at all, index the animation directly — `walk[2]` or
`*walk.find("hurt")` — and thread that into draw state the same way. Want the pure form without the
wrapper? Call `frameAt(walk, elapsedTicks, profile, mode)` and own the tick counter yourself; both ship.

A worked example — one button per playback mode — is in
[`examples/animation_demo.cpp`](../../examples/animation_demo.cpp).

> **Photosensitivity:** keep frame and palette-cycle steps slow, and avoid high-contrast flicker between
> adjacent frames.

## Where to change things

- **Play a sprite/tile animation:** build an `Animation` of `AnimationFrame`s, hold an `AnimationPlayer`,
  `advance()` it each tick, thread `current()` into a `Sprite`/`TileCell`.
- **Palette cycling (shimmer, day/night flicker):** vary `AnimationFrame::palette` across frames and hold
  `slot` constant — same type, no new art.
- **Change how a clip plays (once vs loop vs N times vs for a duration):** pass a different `PlaybackMode`
  to `advance()` — it is not stored on the `Animation`.
- **One sheet, many clips (per-facing rows):** load with `ContentKind::AnimationSeries` +
  `framesPerAnimation` and build each clip from `manifest.group(g)`.
- **Drive playback without the cursor object:** call the pure `playbackAt` / `frameAt` and own the
  elapsed-tick counter yourself.
- **Animate a *value* instead of frames (fade, ramp, transition):** that's a tween, not an animation —
  see [tween.md](tween.md).
