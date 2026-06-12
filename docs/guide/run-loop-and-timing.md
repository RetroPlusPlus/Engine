# Run loop & timing

The fixed-step simulation loop, the injectable clock it reads, the sim/render decoupling that
lets you interpolate the rendered frame, and the host-selected timing profile that sets the
cadence.

```cpp
#include "gbcpp/run_loop.h"   // RunLoop, kMaxFrameTime
#include "gbcpp/clock.h"      // Clock, SteadyClock
#include "gbcpp/double_buffer.h"  // DoubleBuffer<T>
#include "gbcpp/timing.h"     // TimingProfile, TickPeriodNs, CpuTiming
```

## The model

The simulation runs at a **fixed tick rate**, decoupled from however fast the host presents
frames. Each tick is one logical step of the game; the loop runs as many whole ticks as real
time has accumulated, then renders **once** with an interpolation factor `alpha ∈ [0, 1)` for how
far between the last two ticks the render moment falls. This keeps game logic deterministic and
frame-rate-independent while letting rendering run smooth at the display's refresh.

## `RunLoop`

```cpp
class RunLoop {
public:
    using TickCallback   = std::function<void(const InputState&)>;
    using RenderCallback = std::function<void(float)>;  // receives alpha ∈ [0,1)

    explicit RunLoop(Clock& clock, TimingProfile timing = TimingProfile::GameBoyColor) noexcept;

    void setTick(TickCallback cb);       // your game's one logical step
    void setRender(RenderCallback cb);   // your draw, given the interpolation alpha
    void setRawInput(ButtonSet raw);     // host pushes the latest held buttons

    void advance();   // the steppable core — run pending ticks, render once
    void run();       // thin blocking driver: advance() until stop()
    void stop() noexcept;

    [[nodiscard]] std::uint64_t tickCount() const noexcept;
    [[nodiscard]] const TimingProfile& timing() const noexcept;
    [[nodiscard]] std::chrono::nanoseconds tickPeriod() const noexcept;
};
```

You wire two callbacks:

- **Tick** receives an `InputState` (the per-tick held/edge view — see [input.md](input.md)) and
  advances your game one logical step. Input is sampled at the head of each tick, so when one
  `advance()` runs several catch-up ticks they all observe the same raw state and edges fire only
  on the batch's first tick.
- **Render** receives `alpha` and draws. `alpha` is how far between the previous and current tick
  the render falls — use it to interpolate positions so motion is smooth between ticks. If you
  don't interpolate, ignore it and you get tick-quantized rendering.

`advance()` is the testable core: it reads the clock once, runs the right number of fixed ticks
for the elapsed (clamped) time, and renders once. `run()` is a thin single-threaded loop over
`advance()` until `stop()`. There are no internal threads and no atomics — everything runs on the
one platform thread. In a real windowed program you typically don't call `run()`; the windowed
host (see [platform-and-windowing.md](platform-and-windowing.md)) interleaves the OS event pump
between `advance()` calls for you.

### Spiral-of-death clamp

`kMaxFrameTime` (250 ms) caps the elapsed time fed into the accumulator each frame, so a stalled
host frame (a debugger break, a long pause) can never trigger an unbounded catch-up burst — the
sim slows down instead of freezing while it tries to "catch up" forever. It's a host-safety bound,
not a cadence target, so it's the same regardless of timing profile.

## The clock seam

```cpp
class Clock { virtual std::chrono::nanoseconds now() const noexcept = 0; };
class SteadyClock final : public Clock { /* std::chrono::steady_clock */ };
```

`RunLoop` reads time through a `Clock` rather than calling the wall clock directly. Production
code passes a `SteadyClock` (monotonic). Tests pass a deterministic clock so they can drive the
loop tick-by-tick with no real waits. This is the same injectable-seam discipline applied to the
platform boundary (a `Platform` interface, a `MockPlatform` for tests).

## Interpolation: `DoubleBuffer<T>`

The engine supplies `alpha`; **the game owns the snapshots and the blend.** `DoubleBuffer<T>` is
the opt-in helper:

```cpp
template <typename T>
class DoubleBuffer {
    T&       current();        // mutate this during a tick
    const T& previous() const; // the prior tick's state — the interpolation source
    void     advance();        // call once per tick, BEFORE mutating current()
};
```

Each tick, call `advance()` (current becomes previous), then update `current()`. In your render
callback, lerp `previous()` → `current()` by `alpha`. The lerp itself is game-specific (positions,
camera, whatever you render) and lives in your code — the engine deliberately doesn't impose a
renderable-state type. `DoubleBuffer` isn't required by the engine; it's shipped because the first
consumer needs exactly this shape.

## Timing profile

The cadence is **host-selected**, not a baked constant. `RunLoop`'s second constructor argument is
a `TimingProfile`; it defaults to `TimingProfile::GameBoyColor`, so `RunLoop loop{clock};`
reproduces the exact Game Boy Color cadence with no extra code. A game in any other retro idiom
sets its own period — a clean `TickPeriodNs::Hz60`, an NTSC-NES ~60.1 Hz, or any raw nanosecond
period via `static_cast<TickPeriodNs>(...)`. The Game Boy presets are defaults, not the only
choice; more console presets are added to the enum as needed.

```cpp
enum class TickPeriodNs : std::int64_t {
    GameBoy      = 16'742'706,  // 59.7275 Hz — one real GB frame (70'224 cycles @ 4'194'304 Hz)
    GameBoyColor = 16'742'706,  // identical refresh
    Hz60         = 16'666'667,  // a clean 60 Hz for a game that just wants a round rate
};

struct CpuTiming {              // OPTIONAL — for the future SM83 VM; omit if you have no CPU model
    std::uint32_t cpuClockHz;
    std::uint32_t cyclesPerFrame;
    std::uint32_t doubleSpeedCyclesPerFrame;
};

struct TimingProfile {
    TickPeriodNs             tickPeriodNs = TickPeriodNs::GameBoyColor;  // the render cadence
    std::optional<CpuTiming> cpu{};                                     // the VM's cycle budget

    std::chrono::nanoseconds tickPeriod() const noexcept;  // what RunLoop schedules on

    static const TimingProfile GameBoy;        // 59.7275 Hz + GB CPU block
    static const TimingProfile GameBoyColor;   // 59.7275 Hz + GBC CPU block
};
```

Key points:

- **The value is a *period in nanoseconds*, not a rate.** A frequency like 59.7275 Hz is
  fractional and can't be an exact enum value, but its period (16'742'706 ns) is an exact integer.
  Pass a preset or a raw period interchangeably: `static_cast<TickPeriodNs>(16'700'000)`.
- **The tick is one real GB frame** (70'224 SM83 cycles at the 4'194'304 Hz CPU clock), which is
  59.7275 Hz — *not* a flat 60 Hz. Anchoring to a real frame keeps the cadence hardware-faithful.
- **GBC "double speed" is a CPU cycle budget, not a faster refresh.** The display still refreshes
  at 59.7275 Hz; double speed only doubles the per-frame CPU cycle budget (the `cpu` block).
  Frame rate does not change.
- The presets are **static members of the type** (`TimingProfile::GameBoyColor`), the
  self-type-constant idiom — fully usable in `constexpr` contexts including the `RunLoop` default.
- The optional `cpu` block is carried for the future SM83 VM and is unused by the render loop. An
  original game with no CPU model leaves it empty.

A default-constructed `TimingProfile` is the GBC cadence; a game targeting a round 60 Hz passes
`TimingProfile{TickPeriodNs::Hz60}`.
