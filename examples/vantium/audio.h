#pragma once

// Vantium — the AUDIO layer. Registers the game's chiptune SFX on the single AudioLibrary and
// plays them as the sim emits GameEvents. One AudioSystem per SFX — the established interim shape
// (a single system's VM arena holds ~one routine; the pool collapses to one typed system when the
// anti-channel-stealing rework lands). Side benefit: SFX overlap via OS mixing — the fire tick
// never cuts off a destruct rumble.
//
// The mapping is smaller than the event set: EnemyDown also voices MineDown, and LandNow is
// silent (the HUD pulse is its cue) — 7 sounds voice 9 events.

#include <array>
#include <cstddef>
#include <memory>

#include "retropp/audio_library.h"
#include "retropp/audio_system.h"

#include "layout.h"

namespace vant {

class VantAudio {
public:
    // Registers the 7 SFX (all Embed) and constructs one AudioSystem per SFX. Construct AFTER the
    // SdlPlatform (the auto-owned SdlAudioSinks need SDL_INIT_AUDIO, which the platform performs).
    VantAudio();

    void onEvent(GameEventKind kind);

private:
    static constexpr std::size_t kSfxCount = 7;
    std::array<std::unique_ptr<retropp::AudioSystem>, kSfxCount> systems_;
    std::array<retropp::AudioId, kSfxCount>                      sfx_{};
};

}  // namespace vant
