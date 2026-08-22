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

// A registered exit guard's per-boundary answer to a pending exit (see exitAction / exitRequest).
enum class ExitVerdict {
    Proceed,   // close-out is done — tear the program down now (the loop stops)
    NotYet,    // still closing out — keep running, ask again at the next frame boundary
    Veto,      // abandon the exit — clear the pending request and resume normal running
};

class RunLoop {
public:
    using TickCallback   = std::function<void(const InputState&)>;
    using RenderCallback = std::function<void()>;
    using ExitGuard      = std::function<ExitVerdict()>;  // the close-out guard the engine drives to resolve an exit

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

    void simTick(TickCallback cb)     { tick_ = std::move(cb); }

    // The render callback takes no argument. Interpolation is the renderer's: it eases each object
    // between the two submissions it holds, matched by key, using the blend factor advance() publishes
    // on the frame-timing channel. A game submits its latest state and the easing happens beneath it.
    void renderLoop(RenderCallback cb) { render_ = std::move(cb); }

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

    // ── Application exit — a game-facing quit + a declarative close-out guard ─────────────
    // A game ends its own run (a title "Esc quits", a pause-menu "Quit") by submitting exit intent;
    // the engine then drives a registered guard to completion at the frame boundary, so a resume
    // snapshot / save / fade-out runs before teardown. Every exit source — programmatic here, the OS
    // window-close in WindowedHost, a headless run() — routes through the one guard.

    // Register the close-out guard (the simTick / renderLoop registration idiom). While an exit is
    // pending, the engine calls it once per frame boundary and acts on the ExitVerdict it returns.
    // With no guard registered, a pending exit Proceeds immediately.
    void exitAction(ExitGuard fn) { exitGuard_ = std::move(fn); }

    // Submit exit intent — call from a sim tick. It raises the pending state and returns; it never runs
    // the guard or tears down mid-tick. Repeated calls while pending are ignored, and it does nothing
    // once an exit has resolved.
    void exitRequest() noexcept { if (!exitResolved_) exitPending_ = true; }

    // Whether an exit is currently being processed — raised but not yet resolved or vetoed. The
    // windowed host reads it to detect a Veto (pending cleared without resolving) so it can clear the
    // OS quit latch; a game never polls it.
    [[nodiscard]] bool exitPending() const noexcept { return exitPending_; }

    // Whether a guard has resolved a pending exit to Proceed — terminal. run() and WindowedHost stop
    // once this is true, and advance() pulls no further ticks.
    [[nodiscard]] bool exitResolved() const noexcept { return exitResolved_; }

    [[nodiscard]] std::uint64_t tickCount() const noexcept { return tickCount_; }

private:
    Clock& clock_;
    TimingProfile            timing_;       // host-selected cadence + optional CPU block
    std::chrono::nanoseconds tickPeriod_;   // resolved once from timing_ (the fixed step)
    // Consult the registered guard for a pending exit at the frame boundary and act on the verdict.
    // Called once per advance() (both the baseline frame and every subsequent one), after the render.
    void resolveExitAtBoundary();

    TickCallback   tick_;
    RenderCallback render_;
    ExitGuard      exitGuard_;              // the registered close-out guard (empty = proceed on request)
    bool           exitPending_  = false;   // an exit is raised and being processed
    bool           exitResolved_ = false;   // a guard resolved the exit to Proceed (terminal)
    InputSample latest_;  // latest pushed sample — the tick's held state / values / active device
    std::array<ActionSet, kMaxPlayers> heldUnion_{};  // per-slot OR of every level pushed since the
                                                      // last tick (press buffering)
    std::array<AnalogInput, kMaxPlayers> pendingAnalog_{};  // per-slot per-tick analog accumulator
                                                            // (relatives sum, absolutes latest)
    InputState  input_;

    std::chrono::nanoseconds last_{};
    std::chrono::nanoseconds accumulator_{};
    std::uint64_t tickCount_ = 0;
    // How many ticks the most recent commit ran — the number of fixed steps the renderer's retained
    // mirror spans between its previous and current state. Retained across iterations that commit no
    // tick, which are still easing across that same interval. 1 until the first commit.
    int  commitSpan_ = 1;
    bool started_ = false;
    bool running_ = false;
};

}  // namespace retropp
