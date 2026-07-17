#include "retropp/animation.h"

#include <algorithm>

namespace retropp {

// ── Animation: programmatic symbolic access ─────────────────────────────────────────────────────────

std::optional<std::size_t> Animation::indexOf(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].label == name) return i;
    }
    return std::nullopt;
}

const AnimationFrame* Animation::find(std::string_view name) const noexcept {
    const std::optional<std::size_t> i = indexOf(name);
    return i ? &frames[*i] : nullptr;
}

// ── PlaybackMode named constructors ──────────────────────────────────────────────────────────────────

PlaybackMode PlaybackMode::single() {
    return PlaybackMode{Kind::Single, 0, std::chrono::nanoseconds{}};
}
PlaybackMode PlaybackMode::loopNTimes(std::uint32_t n) {
    return PlaybackMode{Kind::LoopNTimes, n, std::chrono::nanoseconds{}};
}
PlaybackMode PlaybackMode::loopIndefinitely() {
    return PlaybackMode{Kind::LoopIndefinitely, 0, std::chrono::nanoseconds{}};
}
PlaybackMode PlaybackMode::playForDuration(std::chrono::nanoseconds d) {
    return PlaybackMode{Kind::PlayForDuration, 0, d};
}

// ── The pure resolver ─────────────────────────────────────────────────────────────────────────────

namespace {

// The frame whose [cum, cum + ticks) window contains posInPass (posInPass ∈ [0, total)). A zero-tick
// frame has an empty window, so it is skipped — never the resting frame and never a stall point.
// Precondition: total > 0 and posInPass < total, so the loop always returns inside it.
std::size_t frameAtPosInPass(const Animation& anim, const TimingProfile& profile,
                             std::uint64_t posInPass) noexcept {
    std::uint64_t cum = 0;
    for (std::size_t i = 0; i < anim.frames.size(); ++i) {
        const std::uint64_t t = profile.ticksForDuration(anim.frames[i].duration);
        if (t == 0) continue;
        if (posInPass < cum + t) return i;
        cum += t;
    }
    return anim.frames.empty() ? 0 : anim.frames.size() - 1;  // unreachable given the precondition
}

// The frame an animation holds on once playback has ended (Single / LoopNTimes / PlayForDuration): the
// last frame with positive ticks, so a trailing zero-tick frame is never the resting frame. Falls back
// to the last index when no frame has positive ticks (the all-instantaneous degenerate case).
std::size_t restingFrame(const Animation& anim, const TimingProfile& profile) noexcept {
    for (std::size_t i = anim.frames.size(); i-- > 0;) {
        if (profile.ticksForDuration(anim.frames[i].duration) > 0) return i;
    }
    return anim.frames.empty() ? 0 : anim.frames.size() - 1;
}

}  // namespace

std::uint64_t totalTicks(const Animation& anim, const TimingProfile& profile) noexcept {
    std::uint64_t sum = 0;
    for (const AnimationFrame& f : anim.frames) {
        sum += profile.ticksForDuration(f.duration);
    }
    return sum;
}

PlaybackState sampleAnimation(const Animation& anim, std::uint64_t elapsedTicks,
                         const TimingProfile& profile, PlaybackMode mode) noexcept {
    if (anim.frames.empty()) {
        return PlaybackState{0, true};
    }
    const std::uint64_t total = totalTicks(anim, profile);
    if (total == 0) {
        // Every frame is instantaneous — no time progression is possible. Hold the resting frame;
        // a finite-policy mode is immediately finished, an indefinite loop just rests (never stalls).
        const bool finished = mode.kind != PlaybackMode::Kind::LoopIndefinitely;
        return PlaybackState{restingFrame(anim, profile), finished};
    }

    switch (mode.kind) {
        case PlaybackMode::Kind::LoopIndefinitely:
            return PlaybackState{frameAtPosInPass(anim, profile, elapsedTicks % total), false};

        case PlaybackMode::Kind::Single:
            if (elapsedTicks >= total) {
                return PlaybackState{restingFrame(anim, profile), true};
            }
            return PlaybackState{frameAtPosInPass(anim, profile, elapsedTicks), false};

        case PlaybackMode::Kind::LoopNTimes: {
            const std::uint64_t endTick = total * mode.loopCount;  // n full passes
            if (mode.loopCount == 0 || elapsedTicks >= endTick) {
                return PlaybackState{restingFrame(anim, profile), true};
            }
            return PlaybackState{frameAtPosInPass(anim, profile, elapsedTicks % total), false};
        }

        case PlaybackMode::Kind::PlayForDuration: {
            const std::uint64_t cut = profile.ticksForDuration(mode.duration);
            if (cut == 0 || elapsedTicks >= cut) {
                // Hold the frame shown at the cutoff — the last tick that played was cut - 1.
                const std::uint64_t at = cut == 0 ? 0 : (cut - 1) % total;
                return PlaybackState{frameAtPosInPass(anim, profile, at), true};
            }
            return PlaybackState{frameAtPosInPass(anim, profile, elapsedTicks % total), false};
        }
    }
    return PlaybackState{0, true};  // unreachable — all kinds handled
}

const AnimationFrame& sampleAnimationFrame(const Animation& anim, std::uint64_t elapsedTicks,
                              const TimingProfile& profile, PlaybackMode mode) noexcept {
    return anim.frames[sampleAnimation(anim, elapsedTicks, profile, mode).frameIndex];
}

// ── AnimationPlayer: the game-owned "just play it" wrapper ──────────────────────────────────────────

namespace {

// Cumulative tick offset of a frame's window start: the sum of all prior frames' ticks. Landing here
// puts a re-resolve exactly at that frame's window start regardless of unequal durations.
std::uint64_t frameStartTick(const Animation& anim, const TimingProfile& profile,
                             std::size_t frameIndex) noexcept {
    std::uint64_t cum = 0;
    const std::size_t upTo = std::min(frameIndex, anim.frames.size());
    for (std::size_t i = 0; i < upTo; ++i) {
        cum += profile.ticksForDuration(anim.frames[i].duration);
    }
    return cum;
}

}  // namespace

void AnimationPlayer::advance(PlaybackMode mode, std::uint64_t deltaTicks) noexcept {
    if (animation == nullptr) return;
    if (playing) {
        elapsedTicks += deltaTicks;
    }
    state = sampleAnimation(*animation, elapsedTicks, profile, mode);
}

const AnimationFrame& AnimationPlayer::current() const {
    return (*animation)[state.frameIndex];  // precondition: animation != nullptr && count() > 0
}

void AnimationPlayer::play() noexcept { playing = true; }

void AnimationPlayer::pause() noexcept { playing = false; }

void AnimationPlayer::stop() noexcept {
    playing      = false;
    elapsedTicks = 0;
    state        = PlaybackState{0, false};  // rewound to the first frame, not finished
}

void AnimationPlayer::restart() noexcept {
    playing      = true;
    elapsedTicks = 0;
    state        = PlaybackState{0, false};
}

void AnimationPlayer::seek(std::size_t frameIndex) noexcept {
    if (animation == nullptr || frameIndex >= animation->count()) return;
    elapsedTicks = frameStartTick(*animation, profile, frameIndex);
    state        = PlaybackState{frameIndex, false};  // honor the explicit seek target
}

void AnimationPlayer::seek(std::string_view label) noexcept {
    if (animation == nullptr) return;
    if (const std::optional<std::size_t> i = animation->indexOf(label)) {
        seek(*i);
    }
}

}  // namespace retropp
