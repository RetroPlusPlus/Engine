#pragma once

// The pure mix-step helpers, factored out so the production loop AND a device-free unit test drive the
// SAME arithmetic (the auto_close.h precedent: the decision is pure, the thread/device is integration).
//
// An AudioSystem produces MANY voices at once — each cued sound is its own voice, and play() never
// preempts a playing one. Every voice contributes one post-gain frame per output frame; the system's
// output is the saturating sum of the contributions. With a single voice the sum is the exact identity
// (the int32 accumulator holds one int16 value, and the clamp cannot fire), so a lone sound reaches the
// ring bit-for-bit as produced — mixing costs nothing until a second voice actually plays.
//
// INTERNAL — under src/audio/, never include/retropp/. Header-only.
#ifndef RETROPP_SRC_AUDIO_PRODUCE_STEP_H
#define RETROPP_SRC_AUDIO_PRODUCE_STEP_H

#include <cstdint>
#include <span>

#include "retropp/audio.h"  // AudioFrame

namespace retropp::detail {

// Saturate a mixed int32 accumulation back to the 16-bit sample range. A single int16 contribution is
// always in range, so the clamp is the identity for one voice; it only engages when simultaneous voices
// genuinely sum past full scale.
[[nodiscard]] constexpr std::int16_t clampMixedSample(std::int32_t s) noexcept {
    if (s > 32767) {
        return 32767;
    }
    if (s < -32768) {
        return -32768;
    }
    return static_cast<std::int16_t>(s);
}

// One mixed output frame: the saturating sum of every active voice's post-gain frame at the same
// position. Empty input mixes to silence.
[[nodiscard]] constexpr AudioFrame mixFrames(std::span<const AudioFrame> perVoice) noexcept {
    std::int32_t left  = 0;
    std::int32_t right = 0;
    for (const AudioFrame& f : perVoice) {
        left += f.left;
        right += f.right;
    }
    return AudioFrame{clampMixedSample(left), clampMixedSample(right)};
}

// The release ramp: scale a frame by remaining/total, the linear fade a closing voice's tail rides so a
// sound never truncates at amplitude (an instant step to silence is an audible click). `remaining >=
// total` (or a zero total) is the identity — a voice not in release passes through untouched.
[[nodiscard]] constexpr AudioFrame rampFrame(AudioFrame f, std::size_t remaining,
                                             std::size_t total) noexcept {
    if (total == 0 || remaining >= total) {
        return f;
    }
    const auto num = static_cast<std::int32_t>(remaining);
    const auto den = static_cast<std::int32_t>(total);
    return AudioFrame{static_cast<std::int16_t>(f.left * num / den),
                      static_cast<std::int16_t>(f.right * num / den)};
}

}  // namespace retropp::detail

#endif  // RETROPP_SRC_AUDIO_PRODUCE_STEP_H
