#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

#include "retropp/clock.h"
#include "retropp/input.h"
#include "retropp/timing.h"

namespace retropp {

// ── Sim cadence ──────────────────────────────────────────────────────────────────
// The tick period comes from the host-selected TimingProfile (timing.h), defaulting to
// TimingProfile::GameBoyColor: one real Game Boy frame, 70'224 SM83 cycles at the
// 4'194'304 Hz CPU clock ⇒ 16'742'706 ns (59.7275 Hz, not a flat 60). The simulation is
// tick-indexed, so determinism is unaffected by the wall-clock→tick rounding.

// Spiral-of-death clamp: advance() caps the per-frame elapsed it accumulates, so a stalled
// host frame (debugger break, long GC pause) can't trigger an unbounded catch-up burst — the
// sim slows instead of freezing. A console-independent host-safety bound, not a cadence target,
// so it is not part of the timing profile. Caps catch-up at ⌊0.25 s / tickPeriod⌋ (14 ticks per
// advance() at the GBC period).
inline constexpr std::chrono::nanoseconds kMaxFrameTime{250'000'000};

// Fixed-step run loop: a fixed-rate simulation decoupled from an interpolated render. advance()
// is the testable core (reads the injected clock, runs the due number of fixed ticks, renders
// once with an interpolation factor); run() is a thin single-threaded driver over it. advance()
// publishes that factor and whether a tick committed this iteration on the frame-timing channel
// (frame_timing.h); the renderer reads it to interpolate each object between its previous and current
// tick state automatically, so the game's render callback stays renderFrame(frame).
class RunLoop {
public:
    using TickCallback   = std::function<void(const InputState&)>;
    using RenderCallback = std::function<void(float)>;  // receives alpha ∈ [0, 1)

    // The settable default cadence: a bare `RunLoop loop{clock};` uses this, so the host can set
    // the cadence once via EngineConfig::setActive() instead of threading a profile to every
    // construction. Initializes to GameBoyColor until setActive() changes it.
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    // Pass a profile to select the cadence per construction; it defaults to defaultTiming. The
    // loop schedules on the profile's tick period; the optional CPU-timing block is carried for
    // the SM83 VM and is unused here.
    explicit RunLoop(Clock& clock, TimingProfile timing = defaultTiming) noexcept
        : clock_(clock), timing_(timing), tickPeriod_(timing.tickPeriod()) {}

    [[nodiscard]] const TimingProfile& timing() const noexcept { return timing_; }
    [[nodiscard]] std::chrono::nanoseconds tickPeriod() const noexcept { return tickPeriod_; }

    void setTick(TickCallback cb)     { tick_ = std::move(cb); }
    void setRender(RenderCallback cb) { render_ = std::move(cb); }

    // Alpha is OPTIONAL: a render callback that takes no argument is accepted too, for the common case
    // where the game lets the engine own interpolation (it submits the latest state and the engine blends
    // between submissions) and so never reads the factor. An overload so such a callback need not declare an
    // unused `float` — `setRender([&]{ ... })`. A game that OWNS its interpolation takes the alpha via the
    // void(float) overload above. Stored as the float form (alpha discarded); unambiguous at the call site
    // because a no-arg lambda isn't callable with a float and vice-versa.
    void setRender(std::function<void()> cb) {
        render_ = [cb = std::move(cb)](float) { cb(); };
    }

    // Host pushes the platform's latest input sample; the loop samples it at the head of each
    // simulation tick. Per player slot: the latest action level is the tick's held state, and every
    // pushed level since the last tick is also OR-accumulated into heldUnion_, so an action pressed
    // and released between two ticks (or pushed on a host frame that produces zero ticks — the
    // display rate need not match the tick rate) still registers as a press rather than being
    // dropped. The analog relatives (rawDelta, wheel) sum across all frames between ticks (a fast
    // spinner flick on a zero-tick frame is not lost); absolutes (cursor, sticks, per-action values,
    // active device) take the latest. Multiple ticks in one catch-up batch share one sample, so the
    // press edge fires once.
    void setRawInput(const InputSample& raw) noexcept {
        latest_ = raw;
        for (int i = 0; i < kMaxPlayers; ++i) {
            heldUnion_[static_cast<std::size_t>(i)] |=
                raw.players[static_cast<std::size_t>(i)].held;
            pendingAnalog_[static_cast<std::size_t>(i)].accumulateFrom(
                raw.players[static_cast<std::size_t>(i)].analog);
        }
    }

    // The steppable core. Reads the clock once, advances the simulation by whole ticks for the
    // elapsed (clamped) time, then renders once with the residual interpolation factor. A windowed
    // host interleaves its event pump between advance() calls.
    void advance();

    // Thin blocking driver: advance() repeatedly until stop(). Single-threaded — no internal
    // threads, no atomics.
    void run();
    void stop() noexcept { running_ = false; }

    [[nodiscard]] std::uint64_t tickCount() const noexcept { return tickCount_; }

private:
    Clock& clock_;
    TimingProfile            timing_;       // host-selected cadence + optional CPU block
    std::chrono::nanoseconds tickPeriod_;   // resolved once from timing_ (the fixed step)
    TickCallback   tick_;
    RenderCallback render_;
    InputSample latest_;  // latest pushed sample — the tick's held state / values / active device
    std::array<ActionSet, kMaxPlayers> heldUnion_{};  // per-slot OR of every level pushed since the
                                                      // last tick (press buffering)
    std::array<AnalogInput, kMaxPlayers> pendingAnalog_{};  // per-slot per-tick analog accumulator
                                                            // (relatives sum, absolutes latest)
    InputState  input_;

    std::chrono::nanoseconds last_{};
    std::chrono::nanoseconds accumulator_{};
    std::uint64_t tickCount_ = 0;
    bool started_ = false;
    bool running_ = false;
};

}  // namespace retropp
