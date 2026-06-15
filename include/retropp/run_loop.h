#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

#include "retropp/clock.h"
#include "retropp/input.h"
#include "retropp/timing.h"

namespace retropp {

// ── Sim cadence (ENG-1 PLAN Decision #3/#4 + A1; lifted into TimingProfile) ──────
// The tick period is no longer a hardcoded global — it comes from the host-selected
// TimingProfile (timing.h), defaulting to TimingProfile::GameBoyColor: one real Game
// Boy frame, 70 224 SM83 cycles at the 4 194 304 Hz CPU clock ⇒ 16 742 706 ns
// (59.7275 Hz), NOT a flat 60 Hz. Anchoring the tick to a real frame keeps the sim
// cadence hardware-faithful and makes the future VM cycle budget per tick exactly the
// profile's cyclesPerFrame (ENG-3/ENG-4). The simulation is tick-indexed, so
// determinism is unaffected by the wall-clock→tick rounding.

// Spiral-of-death clamp: per-frame elapsed is capped before accumulation so a stalled
// host frame (debugger break, GC pause on a future binding) can never trigger an
// unbounded catch-up burst — the sim slows instead of freezing (Decision #5). Console-
// independent host-safety bound, NOT a cadence target, so it is not part of the profile.
// Caps catch-up at ⌊0.25 s / tickPeriod⌋ (14 ticks per advance() at the GBC period).
inline constexpr std::chrono::nanoseconds kMaxFrameTime{250'000'000};

// Fixed-step run loop: a 60-frames-≈-a-second simulation decoupled from an interpolated
// render. advance() is the testable core (reads the injected clock, runs the right
// number of fixed ticks, renders once with an interpolation factor); run() is a thin
// single-threaded driver over it. The engine supplies alpha; the game owns its
// renderable snapshots and the blend (it is game-agnostic — see DoubleBuffer / §6).
class RunLoop {
public:
    using TickCallback   = std::function<void(const InputState&)>;
    using RenderCallback = std::function<void(float)>;  // receives alpha ∈ [0, 1)

    // The host selects the timing profile at construction; it defaults to
    // TimingProfile::GameBoyColor so an existing `RunLoop loop{clock};` keeps the exact
    // ENG-1 GBC cadence with no edit. The loop schedules on the profile's tick period; the
    // optional CPU-timing block is carried for the future SM83 VM (ENG-3), unused here.
    explicit RunLoop(Clock& clock, TimingProfile timing = TimingProfile::GameBoyColor) noexcept
        : clock_(clock), timing_(timing), tickPeriod_(timing.tickPeriod()) {}

    [[nodiscard]] const TimingProfile& timing() const noexcept { return timing_; }
    [[nodiscard]] std::chrono::nanoseconds tickPeriod() const noexcept { return tickPeriod_; }

    void setTick(TickCallback cb)     { tick_ = std::move(cb); }
    void setRender(RenderCallback cb) { render_ = std::move(cb); }

    // Host pushes the latest device button state; the loop samples it at the head of
    // each simulation tick (Decision #13). Multiple ticks in one catch-up batch all
    // observe the same raw state, so edges fire only on the batch's first tick.
    void setRawInput(ButtonSet raw) noexcept { rawInput_ = raw; }

    // The steppable core. Reads the clock once, advances the simulation by whole ticks
    // for the elapsed (clamped) time, then renders once with the residual interpolation
    // factor. A real host interleaves its event pump between advance() calls (ENG-2).
    void advance();

    // Thin blocking driver: advance() repeatedly until stop(). Single-threaded
    // (Decision #14) — no internal threads, no atomics.
    void run();
    void stop() noexcept { running_ = false; }

    [[nodiscard]] std::uint64_t tickCount() const noexcept { return tickCount_; }

private:
    Clock& clock_;
    TimingProfile            timing_;       // host-selected cadence + optional CPU block
    std::chrono::nanoseconds tickPeriod_;   // resolved once from timing_ (the fixed step)
    TickCallback   tick_;
    RenderCallback render_;
    ButtonSet  rawInput_;
    InputState input_;

    std::chrono::nanoseconds last_{};
    std::chrono::nanoseconds accumulator_{};
    std::uint64_t tickCount_ = 0;
    bool started_ = false;
    bool running_ = false;
};

}  // namespace retropp
