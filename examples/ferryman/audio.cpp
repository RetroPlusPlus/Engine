#include "audio.h"

#include "retropp/asset_policy.h"  // AssetPolicy::Embed
#include "retropp/isa.h"           // Isa::Sm83

namespace ferryman {

using namespace retropp;

namespace {

// SFX pool slots, in registration order below.
enum SfxSlot : std::size_t {
    SFX_PEW = 0,   // a passenger's return bolt (quiet — the most frequent cue)
    SFX_PICKUP,    // a colonist boards
    SFX_BANK,      // the deck delivered — the rising sweep (also voices a crossing cleared)
    SFX_BEAM,      // the abductor's tractor beam lights — the funky, quiet warble
    SFX_MUTANT,    // a mutant arrives — the grumbly, crunchy monster growl
    SFX_FOIL,      // the abduction stopped — the springy save
    SFX_SPLAT,     // an enemy destroyed (also the dark echo of a colonist lost)
    SFX_DEATH,     // the ferry destroyed — the one long down-beat
};

}  // namespace

FerrymanAudio::FerrymanAudio() {
    // Registration lives on the single AudioLibrary (the catalog), NOT on a system. Each SFX by
    // its full project-relative LITERAL path, tagged Sfx, in the SM83 ISA, with an explicit
    // per-call AssetPolicy::Embed (the build assembles + bakes the bytes; the .asm never ships).
    // ONE register call per statement so the build scan keys each call cleanly. Registration
    // order matches the SfxSlot enum above.
    AudioLibrary& lib = AudioLibrary::instance();
    sfx_[SFX_PEW]    = lib.registerAudio("examples/ferryman/assets/sfx/pew.asm",    AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_PICKUP] = lib.registerAudio("examples/ferryman/assets/sfx/pickup.asm", AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_BANK]   = lib.registerAudio("examples/ferryman/assets/sfx/bank.asm",   AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_BEAM]   = lib.registerAudio("examples/ferryman/assets/sfx/beam.asm",   AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_MUTANT] = lib.registerAudio("examples/ferryman/assets/sfx/mutant.asm", AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_FOIL]   = lib.registerAudio("examples/ferryman/assets/sfx/foil.asm",   AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_SPLAT]  = lib.registerAudio("examples/ferryman/assets/sfx/splat.asm",  AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_DEATH]  = lib.registerAudio("examples/ferryman/assets/sfx/death.asm",  AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);

    // One SM83 system per SFX (the interim shape — see audio.h). Each hosts exactly one driver
    // in its own VM arena, so no single arena ever overflows.
    for (std::unique_ptr<AudioSystem>& s : systems_) {
        s = std::make_unique<AudioSystem>(AudioKind::Chiptune, VMPlatform::GameBoyColor);
    }
}

void FerrymanAudio::onEvent(GameEventKind kind) {
    // The 10 events → 8 voices. The EMOTION picks the sound, not the mechanism: the two triumphs
    // share the sweep, the splat voices both the kill and the loss that caused it, and the two
    // threats now have their OWN characters — the tractor beam warbles, the mutant growls. Enemy
    // fire is deliberately UNVOICED — a toned-down bullet hell telegraphs visually, and voicing
    // every hostile bolt would bury the cues that matter.
    std::size_t slot;
    switch (kind) {
        case GameEventKind::CargoFire:    slot = SFX_PEW; break;
        case GameEventKind::Pickup:       slot = SFX_PICKUP; break;
        case GameEventKind::Bank:
        case GameEventKind::WaveClear:    slot = SFX_BANK; break;
        case GameEventKind::BeamLock:     slot = SFX_BEAM; break;
        case GameEventKind::MutantSpawn:  slot = SFX_MUTANT; break;
        case GameEventKind::Foil:         slot = SFX_FOIL; break;
        case GameEventKind::EnemyDown:
        case GameEventKind::ColonistLost: slot = SFX_SPLAT; break;
        case GameEventKind::FerryDeath:   slot = SFX_DEATH; break;
        default:                          return;
    }
    systems_[slot]->play(sfx_[slot]);
}

}  // namespace ferryman
