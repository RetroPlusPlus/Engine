#pragma once

// Polterball — the AUDIO layer. Registers the game's chiptune SFX on the single AudioLibrary and
// plays them as the sim emits GameEvents.
//
// INTERIM SHAPE — one AudioSystem per SFX (the bongusoid precedent, same rationale). Today an
// AudioSystem drives a single SM83 VM whose boot-safe code arena holds only ~one routine, and play()
// PLACES each distinct routine into it without reclaiming — cueing 7 distinct SFX through one system
// would overflow the arena and throw. So each SFX gets its OWN system/VM; when the typed,
// anti-channel-stealing AudioSystem lands, this collapses to ONE system and the per-SFX pool goes
// away. Side benefit meanwhile: each system has its own OS-mixed output stream, so SFX overlap — a
// pellet tick never cuts off an ignite whoosh.
//
// The event→sound mapping is deliberately smaller than the event set: WallBounce is silent (it
// fires constantly), Serve shares the paddle blip, and a swallow shares the lost-ball buzz — 7
// sounds voice 10 events.

#include <array>
#include <cstddef>
#include <memory>

#include "retropp/audio_library.h"  // AudioId
#include "retropp/audio_system.h"   // AudioSystem (also brings VMPlatform via vm.h)

#include "game.h"  // GameEventKind

namespace polter {

class PolterAudio {
public:
    // Registers the 7 SFX on AudioLibrary::instance() (all Embed) and constructs one AudioSystem
    // per SFX. Must be constructed AFTER the SdlPlatform (the auto-owned SdlAudioSinks need
    // SDL_INIT_AUDIO, which the platform performs).
    PolterAudio();

    // Cue the SFX for a game event on its own system (re-triggers if already sounding); events with
    // no voice are ignored. Production runs on each system's own thread — the game only cues.
    void onEvent(GameEventKind kind);

private:
    static constexpr std::size_t kSfxCount = 7;
    std::array<std::unique_ptr<retropp::AudioSystem>, kSfxCount> systems_;
    std::array<retropp::AudioId, kSfxCount>                      sfx_{};
};

}  // namespace polter
