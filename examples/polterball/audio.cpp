#include "audio.h"

#include "retropp/asset_policy.h"  // AssetPolicy::Embed
#include "retropp/isa.h"           // Isa::Sm83

namespace polter {

using namespace retropp;

namespace {

// SFX pool slots, in registration order below.
enum SfxSlot : std::size_t {
    SFX_PADDLE = 0,  // paddle bounce (also voices the serve)
    SFX_PELLET,      // the quiet eat tick
    SFX_BREAK,       // a soft wall crunched
    SFX_IGNITE,      // power pellet — the rising whoosh
    SFX_GHOST,       // a frightened ghost smashed
    SFX_LOST,        // the ball dropped OR swallowed — the one down-beat
    SFX_CLEAR,       // board won
};

}  // namespace

PolterAudio::PolterAudio() {
    // Registration lives on the single AudioLibrary (the catalog), NOT on a system. Each SFX by its
    // full project-relative LITERAL path, tagged Sfx, in the SM83 ISA, with an explicit per-call
    // AssetPolicy::Embed (the build assembles + bakes the bytes; the .asm never ships). ONE register
    // call per statement so the build scan keys each call cleanly. Registration order matches the
    // SfxSlot enum above.
    AudioLibrary& lib = AudioLibrary::instance();
    sfx_[SFX_PADDLE] = lib.registerAudio("examples/polterball/assets/sfx/paddle_bounce.asm", AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_PELLET] = lib.registerAudio("examples/polterball/assets/sfx/pellet_eat.asm",    AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_BREAK]  = lib.registerAudio("examples/polterball/assets/sfx/soft_break.asm",    AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_IGNITE] = lib.registerAudio("examples/polterball/assets/sfx/power_ignite.asm",  AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_GHOST]  = lib.registerAudio("examples/polterball/assets/sfx/ghost_down.asm",    AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_LOST]   = lib.registerAudio("examples/polterball/assets/sfx/ball_lost.asm",     AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[SFX_CLEAR]  = lib.registerAudio("examples/polterball/assets/sfx/level_clear.asm",   AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);

    // One SM83 system per SFX (the interim shape — see audio.h). Each hosts exactly one driver in
    // its own VM arena, so no single arena ever overflows.
    for (std::unique_ptr<AudioSystem>& s : systems_) {
        s = std::make_unique<AudioSystem>(AudioKind::Chiptune, VMPlatform::GameBoyColor);
    }
}

void PolterAudio::onEvent(GameEventKind kind) {
    // The 10 events → 7 voices. WallBounce stays silent on purpose: it is the single most frequent
    // event in a maze full of walls, and voicing it would bury every meaningful cue.
    std::size_t slot;
    switch (kind) {
        case GameEventKind::Serve:
        case GameEventKind::PaddleBounce:  slot = SFX_PADDLE; break;
        case GameEventKind::PelletEat:     slot = SFX_PELLET; break;
        case GameEventKind::SoftWallBreak: slot = SFX_BREAK; break;
        case GameEventKind::PowerIgnite:   slot = SFX_IGNITE; break;
        case GameEventKind::GhostDown:     slot = SFX_GHOST; break;
        case GameEventKind::BallSwallowed:
        case GameEventKind::BallLost:      slot = SFX_LOST; break;
        case GameEventKind::LevelClear:    slot = SFX_CLEAR; break;
        case GameEventKind::WallBounce:
        default:                           return;  // silent
    }
    systems_[slot]->play(sfx_[slot], CueMode::Retrigger);  // a repeat fire restarts the effect
}

}  // namespace polter
