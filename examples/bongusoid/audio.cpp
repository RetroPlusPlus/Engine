#include "audio.h"

#include "retropp/asset_policy.h"  // AssetPolicy::Embed
#include "retropp/isa.h"           // Isa::Sm83

namespace bong {

using namespace retropp;

BongAudio::BongAudio() {
    // Registration lives on the single AudioLibrary (the catalog), NOT on a system. Each SFX by its full
    // project-relative LITERAL path, tagged Sfx, in the SM83 ISA, with an explicit per-call
    // AssetPolicy::Embed (the build assembles + bakes the bytes; the .asm never ships). Bongusoid sets no
    // EngineConfig default and relies on none. ONE register call per statement (the build scan keys each
    // call to its own `;`). Registration order matches the GameEventKind enum, so sfx_[ord] / systems_[ord]
    // is that event's sound.
    AudioLibrary& lib = AudioLibrary::instance();
    sfx_[0] = lib.registerAudio("examples/bongusoid/assets/sfx/serve.asm",         AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[1] = lib.registerAudio("examples/bongusoid/assets/sfx/paddle_bounce.asm", AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[2] = lib.registerAudio("examples/bongusoid/assets/sfx/wall_bounce.asm",   AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[3] = lib.registerAudio("examples/bongusoid/assets/sfx/brick_hit.asm",     AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[4] = lib.registerAudio("examples/bongusoid/assets/sfx/brick_break.asm",   AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[5] = lib.registerAudio("examples/bongusoid/assets/sfx/ball_lost.asm",     AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    sfx_[6] = lib.registerAudio("examples/bongusoid/assets/sfx/level_clear.asm",   AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);

    // One SM83 system per SFX (the interim shape — see audio.h). Each hosts exactly one driver in its own
    // VM arena, so no single arena ever overflows. Collapses to one typed system when the audio rework lands.
    for (std::unique_ptr<AudioSystem>& s : systems_) {
        s = std::make_unique<AudioSystem>(AudioKind::Chiptune, VMPlatform::GameBoyColor);
    }
}

void BongAudio::onEvent(GameEventKind kind) {
    const auto i = static_cast<std::size_t>(kind);
    systems_[i]->play(sfx_[i], CueMode::Retrigger);  // a repeat fire restarts the effect
}

}  // namespace bong
