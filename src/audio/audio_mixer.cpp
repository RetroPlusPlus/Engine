// AudioMixer implementation: store the slider levels, and on every change recompute the composed Q16.16
// gain each bus publishes to its production thread. All the fixed-point arithmetic lives here; the hot
// path only loads a published gain and multiplies (retropp/audio_mixer.h applyGain).
#include "retropp/audio_mixer.h"

#include <cmath>
#include <cstdint>

namespace retropp {

namespace {
// The taper's one tunable. Perceived half-loudness is about -10 dB (amplitude ~0.316), and 0.5^1.66 is
// ~0.316, so the slider's midpoint lands there — half the slider sounds like half. Isolated to this one
// function, so the curve can be nudged by ear without touching the mixer or any call site.
constexpr double kPerceptualExponent = 1.66;

// Compose two Q16.16 multipliers into one (a Master gain times a bus gain).
[[nodiscard]] std::uint32_t compose(std::uint32_t a, std::uint32_t b) noexcept {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b)) >> 16);
}
}  // namespace

std::uint32_t perceptualGain(std::uint8_t level) noexcept {
    if (level == 0) {
        return 0;  // exact mute
    }
    if (level == 255) {
        return 1u << 16;  // exact unity — the default, a bit-identical passthrough independent of the curve
    }
    const double fraction = static_cast<double>(level) / 255.0;
    const double gain     = std::pow(fraction, kPerceptualExponent);
    return static_cast<std::uint32_t>(gain * 65536.0 + 0.5);  // round to the nearest Q16.16 step
}

AudioMixer& AudioMixer::instance() {
    static AudioMixer mixer;
    return mixer;
}

void AudioMixer::master(std::uint8_t level) noexcept {
    master_ = level;
    recompute();
}

void AudioMixer::music(std::uint8_t level) noexcept {
    music_ = level;
    recompute();
}

void AudioMixer::sfx(std::uint8_t level) noexcept {
    sfx_ = level;
    recompute();
}

void AudioMixer::vocals(std::uint8_t level) noexcept {
    vocals_ = level;
    recompute();
}

std::uint32_t AudioMixer::effectiveGain(AudioType type) const noexcept {
    switch (type) {
        case AudioType::Music:
            return musicGain_.load(std::memory_order_relaxed);
        case AudioType::Sfx:
            return sfxGain_.load(std::memory_order_relaxed);
        case AudioType::Vocals:
            return vocalsGain_.load(std::memory_order_relaxed);
    }
    return 1u << 16;  // total switch; unity keeps an unknown type audible rather than silent
}

void AudioMixer::recompute() noexcept {
    const std::uint32_t masterGain = perceptualGain(master_);
    musicGain_.store(compose(masterGain, perceptualGain(music_)), std::memory_order_relaxed);
    sfxGain_.store(compose(masterGain, perceptualGain(sfx_)), std::memory_order_relaxed);
    vocalsGain_.store(compose(masterGain, perceptualGain(vocals_)), std::memory_order_relaxed);
}

}  // namespace retropp
