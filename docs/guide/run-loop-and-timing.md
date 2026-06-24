# Run loop & timing

`RunLoop` runs your game logic at a fixed rate and your rendering at the display's rate, decoupled. You
give it two callbacks — a **tick** (one logical step) and a **render** — and it calls them at the right
cadence.

```cpp
#include "retropp/run_loop.h"       // RunLoop, kMaxFrameTime
#include "retropp/clock.h"          // Clock, SteadyClock
#include "retropp/timing.h"         // TimingProfile, TickPeriodNs, CpuTiming
#include "retropp/double_buffer.h"  // DoubleBuffer<T>  (optional interpolation helper)
#include "retropp/pacing.h"         // FrameDeadline, nextFrameDeadline  (used by WindowedHost)
```

## Contents

- [Model](#model)
- [`RunLoop`](#runloop)
  - [Callbacks](#callbacks)
  - [Input each tick](#input-each-tick)
  - [Frame-time clamp](#frame-time-clamp)
- [The clock — `Clock` / `SteadyClock`](#the-clock--clock--steadyclock)
- [Interpolation — `DoubleBuffer<T>`](#interpolation--doublebuffert)
- [Frame pacing](#frame-pacing)
- [Timing profile](#timing-profile)
- [Where to change things](#where-to-change-things)

## Model

The simulation advances in fixed **ticks**; rendering happens once per displayed frame. Each call to
`advance()`:

1. reads the clock,
2. runs as many whole ticks as the elapsed time covers (each at the fixed period),
3. renders once, passing `alpha ∈ [0, 1)` — the fraction of a tick between the last tick and now.

Because ticks are fixed-rate, game logic is deterministic and independent of display refresh. `alpha`
lets you interpolate rendered positions between ticks for smooth motion; ignore it for tick-quantized
rendering.

In a window you don't call `run()` — [`WindowedHost`](platform-and-windowing.md) owns the loop and calls
`advance()` for you. `run()` is the headless driver for tests and tools.

## `RunLoop`

```cpp
class RunLoop {
public:
    using TickCallback   = std::function<void(const InputState&)>;
    using RenderCallback = std::function<void(float)>;   // alpha ∈ [0, 1)

    static inline TimingProfile defaultTiming;           // GameBoyColor unless EngineConfig sets it

    explicit RunLoop(Clock& clock, TimingProfile timing = defaultTiming) noexcept;

    void setTick(TickCallback cb);                       // one logical step, given the tick's input
    void setRender(RenderCallback cb);                   // draw, given alpha
    void setRender(std::function<void()> cb);            // draw, ignoring alpha

    void setRawInput(ButtonSet raw) noexcept;            // latest held buttons (push each host frame)
    void setRawAnalog(const AnalogInput& frame) noexcept;// latest pointer/analog (push each host frame)

    void advance();                                      // run due ticks, render once
    void run();                                          // call advance() until stop()
    void stop() noexcept;

    std::uint64_t            tickCount()  const noexcept;
    const TimingProfile&     timing()     const noexcept;
    std::chrono::nanoseconds tickPeriod() const noexcept;
};
```

### Callbacks

- **`setTick`** takes `void(const InputState&)` — your logical step. The argument is the per-tick
  input view (held state + press/release edges + pointer/analog; see [input.md](input.md)).
- **`setRender`** has two forms. Take `void(float alpha)` to interpolate; take `void()` when you don't
  use `alpha`. Pick one.

A bare `RunLoop{clock}` uses `defaultTiming` (`TimingProfile::GameBoyColor`, or whatever
`EngineConfig::setActive` set — see [platform-and-windowing.md](platform-and-windowing.md)). Pass a
profile to override it: `RunLoop{clock, TimingProfile{TickPeriodNs::Hz60}}`.

### Input each tick

Push the latest device state every host frame; the loop samples it at the start of each tick:

- `setRawInput(buttons)` — the loop reports this as the tick's held state, and reports a `justPressed`
  for any button that went down since the previous tick, **even one already released by tick time**
  (a tap shorter than a tick, or input from a host frame that ran no tick, still registers one press).
- `setRawAnalog(frame)` — relative quantities (raw mouse delta, wheel) **sum** across host frames
  between ticks; absolute quantities (cursor, sticks, triggers) take the latest.

When one `advance()` runs several catch-up ticks, they share one input sample, so each press edge fires
once. See [input.md](input.md) for the full input surface.

### Frame-time clamp

```cpp
inline constexpr std::chrono::nanoseconds kMaxFrameTime{250'000'000};  // 250 ms
```

`advance()` caps the elapsed time it feeds the accumulator at `kMaxFrameTime`. If a host frame takes
longer (a breakpoint, a long stall), the sim runs at most that many ticks instead of an unbounded
catch-up burst — it slows rather than freezes. This is independent of the timing profile.

## The clock — `Clock` / `SteadyClock`

```cpp
class Clock      { virtual std::chrono::nanoseconds now() const noexcept = 0; };
class SteadyClock final : public Clock;   // monotonic (std::chrono::steady_clock)
```

`RunLoop` reads time through a `Clock`. Use `SteadyClock` in a real program; pass your own `Clock` in
tests to drive the loop tick-by-tick with no real waiting.

## Interpolation — `DoubleBuffer<T>`

`RunLoop` gives you `alpha`; you own the state snapshots and the blend. `DoubleBuffer<T>` holds the
previous and current copy of your renderable state:

```cpp
template <typename T>
class DoubleBuffer {
public:
    T&       current();         // write this tick's state here
    const T& current() const;
    const T& previous() const;  // last tick's state — the interpolation source
    void     advance();         // call once per tick, before writing current()
};
```

```cpp
DoubleBuffer<World> world;

loop.setTick([&](const InputState& in) {
    world.advance();              // current becomes previous
    step(world.current(), in);    // write this tick's state
});

loop.setRender([&](float alpha) {
    draw(lerp(world.previous(), world.current(), alpha));   // lerp is yours
});
```

The blend (`lerp`) is whatever your renderable state needs and lives in your code. `DoubleBuffer` is
optional — render from `current()` alone with the no-argument `setRender` for tick-quantized output.

## Frame pacing

`RunLoop` decides when ticks are due; pacing the host iteration to the display is `WindowedHost`'s job.
It paces each iteration to a monotonic deadline spaced by the display's current refresh period, so the
loop runs at the display's cadence:

```cpp
struct FrameDeadline {
    std::chrono::nanoseconds nextDeadline;   // carry to the next iteration
    std::chrono::nanoseconds sleepFor;       // time to sleep now (never negative)
};

constexpr FrameDeadline nextFrameDeadline(std::chrono::nanoseconds prevDeadline,
                                          std::chrono::nanoseconds period,
                                          std::chrono::nanoseconds now,
                                          int maxLagPeriods = 4) noexcept;
```

`nextFrameDeadline` is pure (unit-testable, no clock, no sleep). After each present, `WindowedHost`
sleeps `sleepFor` and carries `nextDeadline` forward. If the loop falls more than `maxLagPeriods` behind,
the deadline resyncs to now and the backlog is dropped, so recovery from a stall doesn't fast-forward.
The OS time / refresh / sleep primitives are the `Platform` pacing seam (`nowMonotonic`,
`displayRefreshPeriod`, `sleepPrecise`); `RunLoop` has no SDL dependency. See
[platform-and-windowing.md](platform-and-windowing.md).

## Timing profile

`TimingProfile` sets the cadence. It carries a tick **period** (always) and an optional CPU-timing block
(for the [VM](vm-and-routines.md)).

```cpp
enum class TickPeriodNs : std::int64_t {
    GameBoy      = 16'742'706,   // 59.7275 Hz — one Game Boy frame (70'224 cycles @ 4'194'304 Hz)
    GameBoyColor = 16'742'706,   // same period
    Hz60         = 16'666'667,   // a round 60 Hz
};

struct CpuTiming {                // optional; for the SM83 VM
    std::uint32_t cpuClockHz;
    std::uint32_t cyclesPerFrame;
    std::uint32_t doubleSpeedCyclesPerFrame;
};

struct TimingProfile {
    TickPeriodNs             tickPeriodNs = TickPeriodNs::GameBoyColor;
    std::optional<CpuTiming> cpu{};

    std::chrono::nanoseconds tickPeriod()       const noexcept;  // what RunLoop schedules on
    std::uint32_t            cpuCyclesPerTick() const noexcept;  // cpu cycles per tick (0 if no cpu block)
    std::uint64_t            ticksForDuration(std::chrono::nanoseconds) const noexcept;

    static const TimingProfile GameBoy;
    static const TimingProfile GameBoyColor;
};
```

- The enum value is a **period in nanoseconds**, not a rate (59.7275 Hz isn't an exact integer; its
  period is). Pass a preset or a raw period: `static_cast<TickPeriodNs>(16'700'000)`.
- The Game Boy tick is one hardware frame ⇒ **59.7275 Hz, not 60**.
- GBC double speed is a CPU cycle budget (`doubleSpeedCyclesPerFrame`); the display rate is unchanged.
- `cpuCyclesPerTick()` is the amount to advance the VM's divider per tick (see
  [vm-and-routines.md](vm-and-routines.md)). `ticksForDuration(std::chrono::seconds{2})` converts a
  wall-clock interval to a tick count.
- The presets are static members, usable in `constexpr` contexts (including the `RunLoop` default).

## Where to change things

- **Cadence:** pass a `TimingProfile` to `RunLoop`, set `EngineConfig::timing`, or assign
  `RunLoop::defaultTiming`.
- **Smooth motion:** interpolate by `alpha` (`DoubleBuffer`); or ignore it for tick-quantized rendering.
- **Run in a window:** `WindowedHost{loop, platform}.run()`
  ([platform-and-windowing.md](platform-and-windowing.md)).
- **Deterministic tests:** inject your own `Clock` (and a `MockPlatform` for pacing).
