#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

#include "gbcpp/clock.h"
#include "gbcpp/input.h"

namespace gbcpp {

// ── Sim cadence constants (ENG-1 PLAN Decision #3/#4 + Amendment A1) ────────────
// The tick is one real Game Boy frame, NOT a flat 60 Hz: 70 224 SM83 cycles at the
// 4 194 304 Hz CPU clock ⇒ 59.7275 Hz. Anchoring the tick to a real frame makes the
// sim cadence hardware-faithful AND makes the future VM cycle budget per tick exactly
// kCyclesPerFrame (ENG-3/ENG-4) — no sim-tick↔frame mapping to maintain. kTickPeriod
// derives from the machine constants; only the wall-clock→tick mapping rounds (the
// simulation is tick-indexed, so determinism is unaffected).
inline constexpr std::uint32_t kCpuClockHz     = 4'194'304;  // GB CPU clock (2^22 Hz)
inline constexpr std::uint32_t kCyclesPerFrame = 70'224;     // SM83 cycles per GB frame
inline constexpr std::chrono::nanoseconds kTickPeriod{
    std::int64_t{1'000'000'000} * kCyclesPerFrame / kCpuClockHz};  // ⌊…⌋ = 16'742'706 ns

// Spiral-of-death clamp: per-frame elapsed is capped before accumulation so a stalled
// host frame (debugger break, GC pause on a future binding) can never trigger an
// unbounded catch-up burst — the sim slows instead of freezing (Decision #5). Caps
// catch-up at ⌊0.25 s / kTickPeriod⌋ = 14 ticks per advance().
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

    explicit RunLoop(Clock& clock) noexcept : clock_(clock) {}

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

}  // namespace gbcpp
