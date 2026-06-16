#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "retropp/animation.h"  // PlaybackMode (reused verbatim; see §"PlaybackMode reuse" below)
#include "retropp/geometry.h"   // Vec2, Vec3, Vec4 — the lerp vocabulary
#include "retropp/timing.h"     // TimingProfile, ticksForDuration

namespace retropp {

// ── Value animation: Tween<T> + easing presets (ENG-2.J) ────────────────────────────────────────────
//
// The exact structural mirror of the ENG-2.H animation system, applied to a VALUE instead of a frame
// index. Where animation resolves elapsed ticks → which frame to show, this resolves elapsed ticks → a
// value of type T (a layer's alpha, a ColorModifier component, an effect parameter, a transform
// rotation — ANY time-varying draw-state value). The engine provides the PURE STATELESS resolver
// (tweenAt, the analogue of playbackAt); the game owns the cursor (TweenPlayer, the analogue of
// AnimationPlayer) and writes the resolved value into draw state each frame. The engine never ticks a
// tween into a draw-state field itself — exactly the immediate-mode relationship animation has (no
// engine state, no new render path, no tween field on any engine struct → Issue 14 is not reopened).
//
// Effect parameters are NOT special — they are one case of animating a draw-state value over time. This
// unit is deliberately decoupled from the effect library (ENG-2.I): it animates any value, and effect
// params are just one sink. The roadmap's transition effects (fade / iris / wipe — inherently a
// progress 0→1 over a duration) are its first real consumer (a wipe's progress is a Tween<float>).

// ── Easing presets ──────────────────────────────────────────────────────────────────────────────────

// The standard easing-curve set, vetted for photosensitivity (no high-frequency flicker): Linear, plus
// In / Out / InOut of each named family. Identity is the enumerator. ease(e, t) shapes a linear
// progress t ∈ [0,1] into the curve's progress. The Back family OVERSHOOTS (may briefly return < 0 or
// > 1) — intentional: a tweened value passes its target and settles back. Every NON-Linear preset
// satisfies ease(e, 0) == 0 and ease(e, 1) == 1 exactly (endpoints are pinned, so transcendental
// rounding never leaks a 0.9999998 out of an endpoint).
//
// Deferred: Elastic / Bounce — oscillatory (multi-cycle) curves. Excluded from v1 so the built-in set
// stays photosensitivity-vetted; add later behind this same enum if a consumer needs them.
enum class Easing : std::uint8_t {
    Linear,
    InQuad,  OutQuad,  InOutQuad,
    InCubic, OutCubic, InOutCubic,
    InQuart, OutQuart, InOutQuart,
    InQuint, OutQuint, InOutQuint,
    InSine,  OutSine,  InOutSine,
    InExpo,  OutExpo,  InOutExpo,
    InCirc,  OutCirc,  InOutCirc,
    InBack,  OutBack,  InOutBack,
};

// Shape a linear progress t into the curve `e`'s progress. t is clamped to [0,1] on entry. Declared
// here, DEFINED in tween.cpp — the curves use std::sin / std::pow / std::sqrt (not constexpr), exactly
// the playbackAt-in-animation.cpp split. Linear returns t; every other preset pins its endpoints to
// 0 and 1. The header templates below (tweenAt / valueAt) call this; it links from tween.cpp.
[[nodiscard]] float ease(Easing e, float t) noexcept;

// ── Interpolation over the engine's float vocabulary ────────────────────────────────────────────────

// lerp(a, b, t) = a + (b - a) * t, with t the EASED progress. constexpr (no transcendentals) so the
// non-eased path is usable in constant expressions. Float family only: a Tween<T> interpolates over
// float / Vec2 / Vec3 / Vec4. INTEGER draw-state sinks (LayerScroll, a Point centre) are animated by
// tweening a float / Vec2 and QUANTIZING at the write into draw state — which the game already owns
// ("the game writes the resolved value into draw state"). The resolver stays pure-float so the easing
// math is exact and free of integer-accumulation artifacts; there is deliberately NO int lerp (it would
// invite mid-track truncation).
[[nodiscard]] constexpr float lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }
[[nodiscard]] constexpr Vec2 lerp(Vec2 a, Vec2 b, float t) noexcept {
    return Vec2{lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
}
[[nodiscard]] constexpr Vec3 lerp(Vec3 a, Vec3 b, float t) noexcept {
    return Vec3{lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
}
[[nodiscard]] constexpr Vec4 lerp(Vec4 a, Vec4 b, float t) noexcept {
    return Vec4{lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)};
}

// ── The track ───────────────────────────────────────────────────────────────────────────────────────

// One timed, eased move toward `to`. The structural analogue of AnimationFrame (which value, for how
// long, under which curve). PURE DATA.
template <typename T>
struct TweenSegment {
    T                        to{};        // value reached at the END of this segment
    std::chrono::nanoseconds duration{};  // wall-time of this segment (resolved to ticks via the profile)
    Easing                   easing = Easing::InOutQuad;  // the curve over THIS segment
    [[nodiscard]] bool operator==(const TweenSegment&) const noexcept = default;
};

// A tween: a start anchor `from` plus an ordered list of timed, eased moves. The structural analogue of
// Animation's frame vector — chosen because it makes YOYO fall out for free (a ping-pong is a
// 2-segment looped track: of(0,1,d).then(0,d) under LoopIndefinitely), exactly as ENG-2.H made
// palette-cycling fall out of per-frame palette. `from` + segments (rather than a keyframe vector with
// an unused first-key duration) avoids the one structural wart while keeping the resolver/player shape
// identical to animation's. The named ctor of() + chainable then() are the authoring ergonomic (the
// Transform::then() idiom); aggregate init stays available.
template <typename T>
struct Tween {
    T                            from{};      // value at t = 0 (the start anchor; no ignored field)
    std::vector<TweenSegment<T>> segments;    // each appends a timed, eased move

    [[nodiscard]] std::size_t count() const noexcept { return segments.size(); }

    // Single-segment named ctor — the headline ergonomic. InOutQuad default curve.
    static Tween of(T from, T to, std::chrono::nanoseconds d, Easing e = Easing::InOutQuad) {
        return Tween{from, {TweenSegment<T>{to, d, e}}};
    }
    // Chainable multi-segment appender — the Transform::then() idiom. Tween<float>::of(0,1,300ms)
    // .then(0,300ms) IS a yoyo.
    Tween& then(T to, std::chrono::nanoseconds d, Easing e = Easing::InOutQuad) {
        segments.push_back(TweenSegment<T>{to, d, e});
        return *this;
    }
};

// ── The pure resolver ─────────────────────────────────────────────────────────────────────────────

// The output of the resolver: the value to use now + whether playback has ended (per mode).
template <typename T>
struct TweenSample {
    T    value{};            // the value to use now
    bool finished = false;   // playback ended (Single / LoopNTimes / PlayForDuration past their end)
    [[nodiscard]] bool operator==(const TweenSample&) const noexcept = default;
};

namespace detail {

// The value held once playback ends (Single / LoopNTimes / PlayForDuration): the chain's final value —
// the last segment's `to`, or `from` if there are no segments.
template <typename T>
T tweenRestingValue(const Tween<T>& tween) noexcept {
    return tween.segments.empty() ? tween.from : tween.segments.back().to;
}

// The interpolated value at position posInPass within ONE pass (posInPass ∈ [0, total)). Walks the
// segments accumulating their tick windows; the value chains from → seg[0].to → seg[1].to → …, and
// within the segment whose [cum, cum+ticks) window contains posInPass returns
// lerp(prevValue, seg.to, ease(seg.easing, localT)). A ZERO-tick segment has an empty window: it is
// skipped, but it still advances the chain (an instantaneous snap to its `to`) so it is never a
// resting value and never a loop stall (mirror of the zero-tick-frame skip). Precondition: total > 0
// and posInPass < total, so the loop always returns inside it.
template <typename T>
T tweenValueInPass(const Tween<T>& tween, const TimingProfile& profile,
                   std::uint64_t posInPass) noexcept {
    T             prev = tween.from;
    std::uint64_t cum  = 0;
    for (const TweenSegment<T>& seg : tween.segments) {
        const std::uint64_t t = profile.ticksForDuration(seg.duration);
        if (t == 0) {
            prev = seg.to;  // instantaneous snap; never occupies a tick window
            continue;
        }
        if (posInPass < cum + t) {
            const float localT =
                static_cast<float>(posInPass - cum) / static_cast<float>(t);
            return lerp(prev, seg.to, ease(seg.easing, localT));
        }
        cum += t;
        prev = seg.to;
    }
    return prev;  // unreachable given the precondition (posInPass == total → handled by the caller)
}

}  // namespace detail

// Length of ONE pass in TICKS: the sum of ticksForDuration(seg.duration) over the segments. 0 segments
// → 0. Pure (TimingProfile is a pass-by-value host config, NOT state).
template <typename T>
[[nodiscard]] std::uint64_t totalTicks(const Tween<T>& tween, const TimingProfile& profile) noexcept {
    std::uint64_t sum = 0;
    for (const TweenSegment<T>& seg : tween.segments) {
        sum += profile.ticksForDuration(seg.duration);
    }
    return sum;
}

// THE pure value resolver — the single source of value truth (TweenPlayer is stateful sugar over it).
// Resolves elapsed ticks under `mode`, value-typed, with the EXACT playbackAt contract:
//   LoopIndefinitely   → posInPass = elapsed modulo total; never finished. (Sawtooth: snaps end→from at
//                        the wrap — a yoyo is authored as a 2-segment track, not a mode.)
//   Single             → first pass, then hold the final segment's `to`; finished once elapsed ≥ total.
//   LoopNTimes(n)      → wrap for n passes, then hold the final `to`; finished once elapsed ≥ n·total.
//   PlayForDuration(d) → wrap (like LoopIndefinitely) until elapsed ≥ ticksForDuration(d), then hold the
//                        value shown at the cutoff; finished past d.
// Empty tween (no segments) → { from, true }. An all-instantaneous tween (every segment rounds to 0
// ticks) holds the resting value — a finite-policy mode is immediately finished, an indefinite loop
// just rests (never stalls). Durations resolve to ticks via the profile — tick-quantized resolution is
// the honest granularity for a fixed-step sim.
template <typename T>
[[nodiscard]] TweenSample<T> tweenAt(const Tween<T>& tween, std::uint64_t elapsedTicks,
                                     const TimingProfile& profile, PlaybackMode mode) noexcept {
    if (tween.segments.empty()) {
        return TweenSample<T>{tween.from, true};
    }
    const std::uint64_t total = totalTicks(tween, profile);
    if (total == 0) {
        const bool finished = mode.kind != PlaybackMode::Kind::LoopIndefinitely;
        return TweenSample<T>{detail::tweenRestingValue(tween), finished};
    }

    switch (mode.kind) {
        case PlaybackMode::Kind::LoopIndefinitely:
            return TweenSample<T>{detail::tweenValueInPass(tween, profile, elapsedTicks % total), false};

        case PlaybackMode::Kind::Single:
            if (elapsedTicks >= total) {
                return TweenSample<T>{detail::tweenRestingValue(tween), true};
            }
            return TweenSample<T>{detail::tweenValueInPass(tween, profile, elapsedTicks), false};

        case PlaybackMode::Kind::LoopNTimes: {
            const std::uint64_t endTick = total * mode.loopCount;  // n full passes
            if (mode.loopCount == 0 || elapsedTicks >= endTick) {
                return TweenSample<T>{detail::tweenRestingValue(tween), true};
            }
            return TweenSample<T>{detail::tweenValueInPass(tween, profile, elapsedTicks % total), false};
        }

        case PlaybackMode::Kind::PlayForDuration: {
            const std::uint64_t cut = profile.ticksForDuration(mode.duration);
            if (cut == 0 || elapsedTicks >= cut) {
                // Hold the value shown at the cutoff — the last tick that played was cut - 1.
                const std::uint64_t at = cut == 0 ? 0 : (cut - 1) % total;
                return TweenSample<T>{detail::tweenValueInPass(tween, profile, at), true};
            }
            return TweenSample<T>{detail::tweenValueInPass(tween, profile, elapsedTicks % total), false};
        }
    }
    return TweenSample<T>{detail::tweenRestingValue(tween), true};  // unreachable — all kinds handled
}

// Convenience: just the value (tweenAt(...).value).
template <typename T>
[[nodiscard]] T valueAt(const Tween<T>& tween, std::uint64_t elapsedTicks,
                        const TimingProfile& profile, PlaybackMode mode) noexcept {
    return tweenAt(tween, elapsedTicks, profile, mode).value;
}

// ── The game-owned cursor ───────────────────────────────────────────────────────────────────────────

// A game-owned playback cursor over a Tween<T>. STATE LIVES HERE, IN THE GAME'S OBJECT — not in the
// engine. The exact mirror of AnimationPlayer, generic and value-typed: it wraps elapsed-tick
// bookkeeping + play / pause / seek over the pure tweenAt resolver. The renderer never sees it; the
// game constructs it, calls advance() each tick, and writes value() into whatever draw-state sink it
// likes. Providing the TYPE while the game owns the INSTANCE (like std::vector) does not reopen
// Issue 14 — an engine-tracked or draw-state-keyed tween would.
template <typename T>
struct TweenPlayer {
    // The cadence a bare-constructed player resolves segment durations against. Mirrors
    // AnimationPlayer::defaultTiming: EngineConfig::setActive does NOT fan into it (Q-cadence — a
    // template static is out of scope for the config fan-out), so a game on a non-GBC cadence either
    // sets TweenPlayer<float>::defaultTiming once or passes .profile per player. Single process-wide
    // default per instantiation — legitimate: the engine is single-threaded and this is a config
    // default, not retained render state.
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    const Tween<T>* tween   = nullptr;          // game-owned; must outlive the player (span-style lifetime)
    TimingProfile   profile = defaultTiming;     // resolves durations→ticks; inherits the default above
    std::uint64_t   elapsedTicks = 0;
    bool            playing = true;
    TweenSample<T>  sample{};                     // cached by advance() so value()/finished() need no args

    // Each game tick: accrue elapsedTicks (ONLY while playing) and re-resolve via tweenAt under `mode`.
    // THIS IS THE "PLAY" — mode defaults to loopIndefinitely so a bare advance() just loops; pass
    // single() / loopNTimes(n) / playForDuration(d) for the other policies.
    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(),
                 std::uint64_t deltaTicks = 1) noexcept {
        if (tween == nullptr) return;
        if (playing) {
            elapsedTicks += deltaTicks;
        }
        sample = tweenAt(*tween, elapsedTicks, profile, mode);
    }

    [[nodiscard]] const T& value()    const noexcept { return sample.value; }
    [[nodiscard]] bool      finished() const noexcept { return sample.finished; }

    void play()  noexcept { playing = true; }   // resume
    void pause() noexcept { playing = false; }  // freeze at the current value
    void stop()  noexcept {                      // pause + rewind to the start anchor
        playing      = false;
        elapsedTicks = 0;
        sample       = TweenSample<T>{tween ? tween->from : T{}, false};
    }
    void restart() noexcept {                    // rewind + play
        playing      = true;
        elapsedTicks = 0;
        sample       = TweenSample<T>{tween ? tween->from : T{}, false};
    }
    // Jump to a wall-time offset (a tween has no frames — a frame index is meaningless), resolved to
    // ticks via the profile; keeps the playing / paused state. The re-resolve uses loopIndefinitely so
    // seek lands a value even past one pass; the next advance() applies the caller's real mode.
    void seek(std::chrono::nanoseconds at) noexcept {
        if (tween == nullptr) return;
        elapsedTicks = profile.ticksForDuration(at);
        sample       = tweenAt(*tween, elapsedTicks, profile, PlaybackMode::loopIndefinitely());
    }
};

}  // namespace retropp
