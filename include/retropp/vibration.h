#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "retropp/animation.h"  // PlaybackMode — reused verbatim as the vibration playback policy
#include "retropp/timing.h"     // TimingProfile, ticksForDuration

namespace retropp {

// ── Controller vibration: the gamepad OUTPUT surface ────────────────────────────────────────────────
//
// Every other input-seam feature reads FROM the pad; vibration is the first channel that sends TO it.
// A game declares the pad's motor state one tick at a time through platform.gamepad(player).vibration(),
// exactly as it declares a frame through renderer.renderFrame() — strictly declarative, resubmitted
// every tick, and a tick with no call is silence. This header is the std-only half: the motor-state
// value type (MotorLevels), an Animation-shaped pattern grammar for authored buzzes, a pure stateless
// resolver, and a game-owned "just play it" cursor. It introduces NO engine playback state (the game
// owns the cursor), mirroring the relationship AnimationPlayer has to the draw model.

// The motor state for one tick — the value passed to vibration(), held by a pattern frame, and returned
// by the resolver; the same type at each step, with no conversion at any of them. Amplitudes are
// 0–255 (255 = full), matching the audio mixer's uint8 levels and Rgba8: every OUTPUT intensity in the
// engine is a byte, while input signals (sticks, triggers) are [0,1] floats — the split is deliberate.
// The platform scales each level to SDL's Uint16 exactly via level × 257. Every motor is optional and
// defaults to 0, so a partial buzz is a designated-init of just the motors it drives.
struct MotorLevels {
    std::uint8_t low         = 0;  // the big / heavy (low-frequency) motor
    std::uint8_t high        = 0;  // the small / light ("fine", high-frequency) motor
    std::uint8_t triggerLeft = 0;  // left-trigger motor (Xbox One+ and pads SDL reports it for)
    std::uint8_t triggerRight = 0;  // right-trigger motor
    [[nodiscard]] bool operator==(const MotorLevels&) const noexcept = default;
};

// One unit of a vibration pattern: a motor state to hold, for how long, under an optional symbolic
// label. PURE DATA, matching the AnimationFrame grammar (label first — identity; the payload; duration
// last). Duration is authored in real time and resolved to ticks via a TimingProfile at play. A frame
// with a duration that rounds to zero ticks is skipped over (never the resting frame, never a stall) —
// so a frame carrying levels but no duration silently does nothing.
struct VibrationFrame {
    std::string_view         label{};      // optional symbolic id (empty = unnamed); identity, first member
    MotorLevels              levels{};      // the motor state this frame holds
    std::chrono::nanoseconds duration{};    // how long this frame holds (real time; resolved to ticks)
    [[nodiscard]] bool operator==(const VibrationFrame&) const noexcept = default;
};

// An ordered list of vibration frames. PURE DATA + pure symbolic access; NO playback STATE and NO loop
// POLICY live here — HOW a pattern plays (once / N times / forever / for a duration) is supplied at play
// time via PlaybackMode, NOT baked into the asset, so the same pattern plays once in one place and loops
// in another. The game owns the elapsed-tick clock (VibrationPlayer, or its own).
struct VibrationPattern {
    std::vector<VibrationFrame> frames;

    // The first frame whose label == name (labels should be unique within a pattern). indexOf → its
    // index (nullopt if absent); find → a pointer to it (nullptr if absent).
    [[nodiscard]] std::optional<std::size_t> indexOf(std::string_view name) const noexcept;
    [[nodiscard]] const VibrationFrame*      find(std::string_view name) const noexcept;
};

// The output of the pure resolver: the motor state to send now + whether playback has ended.
struct VibrationState {
    MotorLevels levels{};           // the motors to drive this tick
    bool        finished = false;   // playback ended (Single / LoopNTimes / PlayForDuration past their end)
    [[nodiscard]] bool operator==(const VibrationState&) const noexcept = default;
};

// Length of ONE pass in TICKS: the sum of ticksForDuration(frame.duration) over the frames. 0 frames
// → 0. Pure (TimingProfile is a pass-by-value host config, NOT state).
[[nodiscard]] std::uint64_t totalTicks(const VibrationPattern& pattern, const TimingProfile& profile) noexcept;

// THE pure playback resolver — the single source of playback truth (VibrationPlayer is the stateful
// wrapper over it). Same argument + return shape as animation's sampleAnimation, resolving elapsed ticks
// under `mode`:
//   LoopIndefinitely   → elapsed modulo totalTicks; the frame's levels; never finished.
//   Single             → the frame's levels within the first pass; SILENCE once elapsed ≥ totalTicks.
//   LoopNTimes(n)      → wrap for n passes, then SILENCE; finished once elapsed ≥ n·totalTicks.
//   PlayForDuration(d) → wrap until elapsed ≥ ticksForDuration(d), then SILENCE; finished past d.
// Empty pattern → { {}, true }. A zero-tick frame (duration rounds to 0) is skipped so it cannot stall.
//
// The one deliberate divergence from sampleAnimation: past a finite mode's end this returns SILENCE
// ({ levels = {}, finished = true }), not the clamped last frame. Endless rumble always reads as a
// broken game, so silence is the safe haptic default — feeding a finished player's levels() to
// vibration() auto-stops, no finished() check required. The rule lives HERE, in the resolver, so the
// player is a thin cache over one source of truth.
[[nodiscard]] VibrationState sampleVibration(const VibrationPattern& pattern, std::uint64_t elapsedTicks,
                                             const TimingProfile& profile, PlaybackMode mode) noexcept;

// A game-owned playback cursor over a VibrationPattern. STATE LIVES HERE, IN THE GAME'S OBJECT — not in
// the engine. Wraps elapsed-tick bookkeeping + play / pause / seek over the pure sampleVibration
// resolver. The game constructs it, calls advance() each tick, and passes levels() to
// gamepad(player).vibration(). Mirrors AnimationPlayer field-for-field, so the two read as one grammar.
struct VibrationPlayer {
    // The cadence a bare-constructed player resolves frame durations against. EngineConfig::setActive
    // fans the configured cadence into it (one startup call, alongside AnimationPlayer::defaultTiming),
    // or assign it directly anytime. Override one player by setting `.profile`. A single process-wide
    // default is legitimate: the engine is single-threaded and this is a config default, not state.
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    const VibrationPattern* pattern = nullptr;   // game-owned; must outlive the player (span-style lifetime)
    TimingProfile           profile = defaultTiming;  // resolves durations → ticks; inherits the default above
    std::uint64_t           elapsedTicks = 0;
    bool                    playing = true;
    VibrationState          state{};             // cached by advance() so levels()/finished() need no args

    // Each game tick: accrue elapsedTicks (ONLY while playing) and re-resolve via sampleVibration under
    // `mode`. THIS IS THE "PLAY" — mode defaults to loopIndefinitely so a bare advance() just loops;
    // pass single() / loopNTimes(n) / playForDuration(d) for the other policies.
    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(),
                 std::uint64_t deltaTicks = 1) noexcept;

    // The cached motor state — {} (silence) once a non-looping pattern is finished (the resolver's
    // finished→silence rule), so passing this to vibration() every tick auto-stops at the end.
    [[nodiscard]] MotorLevels levels()   const noexcept { return state.levels; }
    [[nodiscard]] bool        finished() const noexcept { return state.finished; }

    void play()    noexcept;  // resume
    void pause()   noexcept;  // freeze on the current levels
    void stop()    noexcept;  // pause + rewind to the first frame
    void restart() noexcept;  // rewind + play
    void seek(std::size_t frameIndex) noexcept;  // jump to a frame's start (keeps playing/paused)
    void seek(std::string_view label) noexcept;  // jump by symbolic label (no-op if absent)
};

}  // namespace retropp
