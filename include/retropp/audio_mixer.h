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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "retropp/audio_library.h"  // AudioType + RETROPP_AUDIO_BUSES — the buses a level applies to

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

// The mixer's level channels: every AudioType bus, plus Master (the global scalar — a sound never plays
// "on Master", but its level scales every bus). The bus channels take their values FROM AudioType, so the
// bus values have one source; Master is the one extra, appended last. The static_asserts below let the
// compiler enforce that binding — a bus whose value drifts from AudioType stops compiling. Value alignment
// with AudioType is also what lets an AudioType index the level storage directly.
enum class AudioLevelType : std::uint8_t {
    Music  = static_cast<std::uint8_t>(AudioType::Music),
    Sfx    = static_cast<std::uint8_t>(AudioType::Sfx),
    Vocals = static_cast<std::uint8_t>(AudioType::Vocals),
    Master,
};

static_assert(static_cast<std::uint8_t>(AudioLevelType::Music)  == static_cast<std::uint8_t>(AudioType::Music));
static_assert(static_cast<std::uint8_t>(AudioLevelType::Sfx)    == static_cast<std::uint8_t>(AudioType::Sfx));
static_assert(static_cast<std::uint8_t>(AudioLevelType::Vocals) == static_cast<std::uint8_t>(AudioType::Vocals));

inline constexpr std::size_t kAudioLevelTypeCount =
    static_cast<std::size_t>(AudioLevelType::Master) + 1;

// A batch of level changes handed to AudioMixer::levels() whole. Every field is optional: a field left
// unset (std::nullopt) leaves that channel's current level untouched, so AudioLevels{ .sfx = 100 } moves
// only Sfx and AudioLevels{} is a no-op. A designated-init literal names exactly the channels it changes.
struct AudioLevels {
    std::optional<std::uint8_t> master;
    std::optional<std::uint8_t> music;
    std::optional<std::uint8_t> sfx;
    std::optional<std::uint8_t> vocals;
};

class AudioMixer {
public:
    // The one mixer — a function-local static, materialized on first reference.
    static AudioMixer& instance();

    AudioMixer(const AudioMixer&)            = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // Set one or more levels at once (0 mutes, 255 is unity). Every engaged field of `levels` is written to
    // its channel; every unset field is left as it was — so levels(AudioLevels{ .sfx = 100 }) moves only
    // Sfx, and levels(AudioLevels{}) is a no-op. Main-thread side: recomputes the affected composed gains
    // and publishes them for the production threads to read.
    void levels(const AudioLevels& levels) noexcept;

    // Read one channel's stored slider position, by channel.
    [[nodiscard]] std::uint8_t levels(AudioLevelType type) const noexcept;

    // The composed Master-times-bus gain for `type`, as a Q16.16 multiplier: one relaxed atomic load, no
    // float. Read once per sample on the production thread. All levels at their 255 default return 1<<16.
    [[nodiscard]] std::uint32_t effectiveGain(AudioType type) const noexcept;

private:
    AudioMixer() { levels_.fill(255); }

    // Recompute every bus's composed gain (Master times that bus) from the stored levels and publish them.
    void recompute() noexcept;

    // Every channel's slider position, indexed by AudioLevelType and filled to 255 (unity) in the ctor. An
    // AudioType indexes it directly too — the buses share AudioLevelType's values (locked by static_assert).
    std::array<std::uint8_t, kAudioLevelTypeCount> levels_;

    // The published composed gains, one per bus — written on levels() (main thread), read per sample (each
    // production thread). Default to unity so a never-touched mixer is the exact identity.
    std::atomic<std::uint32_t> musicGain_{1u << 16};
    std::atomic<std::uint32_t> sfxGain_{1u << 16};
    std::atomic<std::uint32_t> vocalsGain_{1u << 16};
};

}  // namespace retropp
