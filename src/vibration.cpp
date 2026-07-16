#include "retropp/vibration.h"

#include <algorithm>

namespace retropp {

// ── VibrationPattern: programmatic symbolic access ──────────────────────────────────────────────────

std::optional<std::size_t> VibrationPattern::indexOf(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].label == name) return i;
    }
    return std::nullopt;
}

const VibrationFrame* VibrationPattern::find(std::string_view name) const noexcept {
    const std::optional<std::size_t> i = indexOf(name);
    return i ? &frames[*i] : nullptr;
}

// ── The pure resolver ───────────────────────────────────────────────────────────────────────────────

namespace {

// The levels of the frame whose [cum, cum + ticks) window contains posInPass (posInPass ∈ [0, total)).
// A zero-tick frame has an empty window, so it is skipped — never the resting frame, never a stall.
// Precondition: total > 0 and posInPass < total, so the loop always returns inside it.
MotorLevels levelsAtPosInPass(const VibrationPattern& pattern, const TimingProfile& profile,
                              std::uint64_t posInPass) noexcept {
    std::uint64_t cum = 0;
    for (const VibrationFrame& f : pattern.frames) {
        const std::uint64_t t = profile.ticksForDuration(f.duration);
        if (t == 0) continue;
        if (posInPass < cum + t) return f.levels;
        cum += t;
    }
    // Unreachable given the precondition. Fall back to the last positive-tick frame's levels.
    for (std::size_t i = pattern.frames.size(); i-- > 0;) {
        if (profile.ticksForDuration(pattern.frames[i].duration) > 0) return pattern.frames[i].levels;
    }
    return MotorLevels{};
}

}  // namespace

std::uint64_t totalTicks(const VibrationPattern& pattern, const TimingProfile& profile) noexcept {
    std::uint64_t sum = 0;
    for (const VibrationFrame& f : pattern.frames) {
        sum += profile.ticksForDuration(f.duration);
    }
    return sum;
}

VibrationState sampleVibration(const VibrationPattern& pattern, std::uint64_t elapsedTicks,
                               const TimingProfile& profile, PlaybackMode mode) noexcept {
    if (pattern.frames.empty()) {
        return VibrationState{MotorLevels{}, true};
    }
    const std::uint64_t total = totalTicks(pattern, profile);
    if (total == 0) {
        // Every frame is instantaneous — no time can progress and there is nothing to hold. A finite
        // mode is immediately finished; an indefinite loop has nothing to play. Silence either way
        // (the finished→silence rule), which also means it never stalls on a resting frame.
        const bool finished = mode.kind != PlaybackMode::Kind::LoopIndefinitely;
        return VibrationState{MotorLevels{}, finished};
    }

    switch (mode.kind) {
        case PlaybackMode::Kind::LoopIndefinitely:
            return VibrationState{levelsAtPosInPass(pattern, profile, elapsedTicks % total), false};

        case PlaybackMode::Kind::Single:
            if (elapsedTicks >= total) {
                return VibrationState{MotorLevels{}, true};  // finished → silence (not the last frame)
            }
            return VibrationState{levelsAtPosInPass(pattern, profile, elapsedTicks), false};

        case PlaybackMode::Kind::LoopNTimes: {
            const std::uint64_t endTick = total * mode.loopCount;  // n full passes
            if (mode.loopCount == 0 || elapsedTicks >= endTick) {
                return VibrationState{MotorLevels{}, true};
            }
            return VibrationState{levelsAtPosInPass(pattern, profile, elapsedTicks % total), false};
        }

        case PlaybackMode::Kind::PlayForDuration: {
            const std::uint64_t cut = profile.ticksForDuration(mode.duration);
            if (cut == 0 || elapsedTicks >= cut) {
                return VibrationState{MotorLevels{}, true};
            }
            return VibrationState{levelsAtPosInPass(pattern, profile, elapsedTicks % total), false};
        }
    }
    return VibrationState{MotorLevels{}, true};  // unreachable — all kinds handled
}

// ── VibrationPlayer: the game-owned "just play it" cursor ────────────────────────────────────────────

namespace {

// Cumulative tick offset of a frame's window start: the sum of all prior frames' ticks. Landing here
// puts a re-resolve exactly at that frame's window start regardless of unequal durations.
std::uint64_t frameStartTick(const VibrationPattern& pattern, const TimingProfile& profile,
                             std::size_t frameIndex) noexcept {
    std::uint64_t cum = 0;
    const std::size_t upTo = std::min(frameIndex, pattern.frames.size());
    for (std::size_t i = 0; i < upTo; ++i) {
        cum += profile.ticksForDuration(pattern.frames[i].duration);
    }
    return cum;
}

// The motor state a rewound player rests on: the first frame's levels, or silence for an empty pattern.
MotorLevels startLevels(const VibrationPattern* pattern) noexcept {
    return (pattern && !pattern->frames.empty()) ? pattern->frames.front().levels : MotorLevels{};
}

}  // namespace

void VibrationPlayer::advance(PlaybackMode mode, std::uint64_t deltaTicks) noexcept {
    if (pattern == nullptr) return;  // no pattern → state stays the silent default
    if (playing) {
        elapsedTicks += deltaTicks;
    }
    state = sampleVibration(*pattern, elapsedTicks, profile, mode);
}

void VibrationPlayer::play() noexcept { playing = true; }

void VibrationPlayer::pause() noexcept { playing = false; }

void VibrationPlayer::stop() noexcept {
    playing      = false;
    elapsedTicks = 0;
    state        = VibrationState{startLevels(pattern), false};  // rewound to the first frame, not finished
}

void VibrationPlayer::restart() noexcept {
    playing      = true;
    elapsedTicks = 0;
    state        = VibrationState{startLevels(pattern), false};
}

void VibrationPlayer::seek(std::size_t frameIndex) noexcept {
    if (pattern == nullptr || frameIndex >= pattern->frames.size()) return;
    elapsedTicks = frameStartTick(*pattern, profile, frameIndex);
    state        = VibrationState{pattern->frames[frameIndex].levels, false};  // honor the explicit seek target
}

void VibrationPlayer::seek(std::string_view label) noexcept {
    if (pattern == nullptr) return;
    if (const std::optional<std::size_t> i = pattern->indexOf(label)) {
        seek(*i);
    }
}

}  // namespace retropp
