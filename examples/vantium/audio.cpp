#include "audio.h"

#include "retropp/asset_policy.h"
#include "retropp/isa.h"

namespace vant {

using namespace retropp;

namespace {

enum SfxSlot : std::size_t {
    SFX_FIRE = 0,   // the bolt — quiet and short, it is the most frequent cue
    SFX_ENEMY,      // a fighter (or mine) destroyed
    SFX_POD,        // a fuel pod popped
    SFX_WAVE,       // full-squadron bonus
    SFX_DEATH,      // the Manta lost
    SFX_LAND,       // touchdown on the strip
    SFX_DESTRUCT,   // the dreadnought scuttling itself
};

}  // namespace

VantAudio::VantAudio() {
    // Registration on the single AudioLibrary catalog; every SFX by its full project-relative
    // LITERAL path, tagged Sfx, SM83, an explicit per-call AssetPolicy::Embed — the build
    // assembles + bakes the bytes, the .asm never ships. One register call per statement.
    AudioLibrary& lib = AudioLibrary::instance();
    sfx_[SFX_FIRE]     = lib.registerAudio("examples/vantium/assets/sfx/fire.asm",          AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_ENEMY]    = lib.registerAudio("examples/vantium/assets/sfx/enemy_down.asm",    AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_POD]      = lib.registerAudio("examples/vantium/assets/sfx/pod_hit.asm",       AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_WAVE]     = lib.registerAudio("examples/vantium/assets/sfx/wave_bonus.asm",    AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_DEATH]    = lib.registerAudio("examples/vantium/assets/sfx/player_death.asm",  AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_LAND]     = lib.registerAudio("examples/vantium/assets/sfx/touchdown.asm",     AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_DESTRUCT] = lib.registerAudio("examples/vantium/assets/sfx/ship_destruct.asm", AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);

    for (std::unique_ptr<AudioSystem>& s : systems_) {
        s = std::make_unique<AudioSystem>(AudioKind::Chiptune, VMPlatform::GameBoyColor);
    }
}

void VantAudio::onEvent(GameEventKind kind) {
    std::size_t slot;
    switch (kind) {
        case GameEventKind::Fire:          slot = SFX_FIRE; break;
        case GameEventKind::EnemyDown:
        case GameEventKind::MineDown:      slot = SFX_ENEMY; break;
        case GameEventKind::PodHit:        slot = SFX_POD; break;
        case GameEventKind::WaveBonus:     slot = SFX_WAVE; break;
        case GameEventKind::PlayerDeath:   slot = SFX_DEATH; break;
        case GameEventKind::Touchdown:     slot = SFX_LAND; break;
        case GameEventKind::ShipDestroyed: slot = SFX_DESTRUCT; break;
        case GameEventKind::LandNow:
        default:                           return;  // the HUD pulse is LandNow's cue
    }
    systems_[slot]->play(sfx_[slot], CueMode::Retrigger);  // a repeat fire restarts the effect
}

}  // namespace vant
