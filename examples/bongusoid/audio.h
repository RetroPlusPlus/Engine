#pragma once

// Bongusoid — the AUDIO layer (S2). Registers the game's chiptune SFX on the single AudioLibrary and
// plays them as the sim emits GameEvents.
//
// INTERIM SHAPE — one AudioSystem per SFX. This is NOT the engine's intended audio architecture; it is a
// workaround for the CURRENT (pre-typing) AudioSystem. Today an AudioSystem drives a single SM83 VM whose
// boot-safe code arena (0x0160–0x0200, ~160 bytes) holds only ~one routine, and play() PLACES each
// distinct routine into it without reclaiming — so cueing 7 distinct SFX through one system overflows the
// arena and throws. As a stopgap each SFX gets its OWN SM83 system/VM, so no single arena ever fills. This
// uses the "multiple AudioSystems" capability the engine already exposes (the audio_keyboard_demo
// precedent) — intended there for co-existing audio TYPES + anti-channel-stealing, used here just to get a
// workable game.
//
// THE PROPER DESIGN — queued as the next engine step right after this demo detour — is a TYPED
// AudioSystem (SM83 / PCM / …, configured at construction; loading the wrong type throws at runtime) whose
// SM83 type internally routes sounds across up to 3 APU VMs by a music/sfx sound-type system
// (anti-channel-stealing). When that lands, this layer collapses to ONE AudioSystem voicing all the SFX
// and these per-SFX systems go away.
//
// Side benefit of the interim: each system has its own output stream (OS-mixed), so SFX can overlap — a
// bounce won't cut off a break's ring.

#include <array>
#include <cstddef>
#include <memory>

#include "retropp/audio_library.h"  // AudioId
#include "retropp/audio_system.h"   // AudioSystem (also brings VMPlatform via vm.h)

#include "game.h"  // GameEventKind (the 7 events this layer voices)

namespace bong {

class BongAudio {
public:
    // Registers the 7 SFX on AudioLibrary::instance() (all Embed) and constructs one AudioSystem per SFX.
    // Must be constructed AFTER the SdlPlatform (the auto-owned SdlAudioSinks need SDL_INIT_AUDIO, which
    // the platform performs).
    BongAudio();

    // Cue the SFX for a game event on its own system (re-triggers if already sounding). Production runs
    // on each system's own thread (ENG-4.D.1) — the game cues and never steps audio.
    void onEvent(GameEventKind kind);

private:
    // Indexed by the GameEventKind ordinal: system i plays SFX i (registration order matches the enum).
    std::array<std::unique_ptr<retropp::AudioSystem>, 7> systems_;
    std::array<retropp::AudioId, 7>                       sfx_{};
};

}  // namespace bong
