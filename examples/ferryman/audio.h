#pragma once

// Ferryman — the AUDIO layer. Registers the game's chiptune SFX on the single AudioLibrary and
// plays them as the sim emits GameEvents.
//
// INTERIM SHAPE — one AudioSystem per SFX (the bongusoid/polterball/kessler precedent, same
// rationale). Today an AudioSystem drives a single SM83 VM whose boot-safe code arena holds only
// ~one routine, and play() PLACES each distinct routine into it without reclaiming — cueing 7
// distinct SFX through one system would overflow the arena and throw. So each SFX gets its OWN
// system/VM; when the typed, anti-channel-stealing AudioSystem lands, this collapses to ONE
// system and the per-SFX pool goes away. Side benefit meanwhile: each system has its own OS-mixed
// output stream, so SFX overlap — a hop tick never cuts off a payout sweep.
//
// The event→sound mapping is deliberately smaller than the event set: 11 events → 7 voices.
// Pickup and Drop share the clip-aboard blip (emotional twins), Bank and WaveClear share the
// rising sweep, BeamLock and MutantSpawn share the klaxon (the "you are in danger" pair), and
// MutantSplat and ColonistLost share the crunch (the splat and its dark echo).

#include <array>
#include <cstddef>
#include <memory>

#include "retropp/audio_library.h"  // AudioId
#include "retropp/audio_system.h"   // AudioSystem (also brings VMPlatform via vm.h)

#include "game.h"  // GameEventKind

namespace ferryman {

class FerrymanAudio {
public:
    // Registers the 7 SFX on AudioLibrary::instance() (all Embed) and constructs one AudioSystem
    // per SFX. Must be constructed AFTER the SdlPlatform (the auto-owned SdlAudioSinks need
    // SDL_INIT_AUDIO, which the platform performs).
    FerrymanAudio();

    // Cue the SFX for a game event on its own system (re-triggers if already sounding).
    // Production runs on each system's own thread — the game only cues.
    void onEvent(GameEventKind kind);

private:
    static constexpr std::size_t kSfxCount = 7;
    std::array<std::unique_ptr<retropp::AudioSystem>, kSfxCount> systems_;
    std::array<retropp::AudioId, kSfxCount>                      sfx_{};
};

}  // namespace ferryman
