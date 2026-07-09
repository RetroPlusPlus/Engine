#pragma once

// The AudioMixer: the program-wide user volume levels every AudioSystem scales its output by.
//
// SINGLE INSTANCE BY CONSTRUCTION, like AudioLibrary. There is one AudioMixer per program, reached through
// AudioMixer::instance(); the constructor is private and copying is deleted. "Audio settings" is one thing
// for the whole program, so the mixer is one object — a settings UI binds its sliders to this instance, and
// every AudioSystem reads it on the production side.
//
// FOUR LEVELS. A Master level scales everything; three bus levels — Music, Sfx, Vocals — scale their own
// AudioType. Each is a std::uint8_t slider position: 0 mutes, 255 is unity (0 dB), and the values between
// follow a perceptual taper (perceptualGain, below) so half the slider sounds like half. A source on bus T
// is scaled by Master composed with bus T. The levels are the setting surface; there is no float on it.
//
// DEFAULT UNITY. Every level defaults to 255, which composes to an exact 1<<16 Q16.16 multiplier, and
// scaling a sample by 1<<16 is the exact identity — so a fresh mixer, or one a game never touches,
// reproduces the produced stream sample for sample.
//
// CROSS-THREAD. Levels are set on the main thread (the settings UI); the composed gain is read once per
// sample on each AudioSystem's production thread. The read crosses threads, so the mixer publishes each
// bus's composed gain through a relaxed atomic — no lock on the audio path. A level change is
// presentation-only and never feeds the simulation, so a read that lands one sample after a write is
// inaudible and races benignly.

#include <atomic>
#include <cstdint>

#include "retropp/audio_library.h"  // AudioType — the bus a level applies to

namespace retropp {

// Map a slider position to a Q16.16 fixed-point gain multiplier (1<<16 == unity). 0 returns 0 (mute) and
// 255 returns 1<<16 (exact unity) regardless of the curve; between them the gain follows a perceptual
// half-loudness power law, so the slider's midpoint lands near -10 dB — audibly half, the point of a
// perceptual fader. Pure and cheap, but it runs only when a level is set (the mixer caches the result),
// never per sample.
[[nodiscard]] std::uint32_t perceptualGain(std::uint8_t level) noexcept;

// Scale one 16-bit sample by a Q16.16 gain and clamp to the 16-bit range. At unity (gain == 1<<16) this is
// the exact identity for every sample — (s * 65536) >> 16 == s — so a mixer at its defaults passes the
// input through bit for bit. A gain above unity can exceed the range, so the result is clamped.
[[nodiscard]] inline std::int16_t applyGain(std::int16_t sample, std::uint32_t gain) noexcept {
    const std::int64_t scaled =
        (static_cast<std::int64_t>(sample) * static_cast<std::int64_t>(gain)) >> 16;
    if (scaled > 32767) {
        return 32767;
    }
    if (scaled < -32768) {
        return -32768;
    }
    return static_cast<std::int16_t>(scaled);
}

class AudioMixer {
public:
    // The one mixer — a function-local static, materialized on first reference.
    static AudioMixer& instance();

    AudioMixer(const AudioMixer&)            = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // Set a level (0 mutes, 255 is unity). Main-thread side: recomputes the affected composed gains and
    // publishes them for the production threads to read.
    void setMaster(std::uint8_t level) noexcept;
    void setMusic(std::uint8_t level) noexcept;
    void setSfx(std::uint8_t level) noexcept;
    void setVocals(std::uint8_t level) noexcept;

    // The stored slider positions.
    [[nodiscard]] std::uint8_t master() const noexcept { return master_; }
    [[nodiscard]] std::uint8_t music() const noexcept { return music_; }
    [[nodiscard]] std::uint8_t sfx() const noexcept { return sfx_; }
    [[nodiscard]] std::uint8_t vocals() const noexcept { return vocals_; }

    // The composed Master-times-bus gain for `type`, as a Q16.16 multiplier: one relaxed atomic load, no
    // float. Read once per sample on the production thread. All levels at their 255 default return 1<<16.
    [[nodiscard]] std::uint32_t effectiveGain(AudioType type) const noexcept;

private:
    AudioMixer() = default;

    // Recompute every bus's composed gain (Master times that bus) from the stored levels and publish them.
    void recompute() noexcept;

    std::uint8_t master_ = 255;
    std::uint8_t music_  = 255;
    std::uint8_t sfx_    = 255;
    std::uint8_t vocals_ = 255;

    // The published composed gains, one per bus — written on set* (main thread), read per sample (each
    // production thread). Default to unity so a never-touched mixer is the exact identity.
    std::atomic<std::uint32_t> musicGain_{1u << 16};
    std::atomic<std::uint32_t> sfxGain_{1u << 16};
    std::atomic<std::uint32_t> vocalsGain_{1u << 16};
};

}  // namespace retropp
