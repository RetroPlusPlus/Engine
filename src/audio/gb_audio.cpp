// ENG-4.A / ENG-4.B — Game Boy audio presets. Console-specific conveniences over the generic AudioSystem;
// the Game Boy specifics (the wave-channel tone source) live here, never on the generic surface.
//
// The diagnostic tone uploads its driver through the RAW door (uploadAudio(bytecode)): its .asm is
// assembled to bytecode AT COMPILE TIME by the constexpr SM83 assembler (gb_routine_bytecode.h), so the
// preset hands over baked bytes — no runtime .asm read, nothing beside the binary. kTone is odr-used only
// here, so the tone is dropped from the binary entirely if no game calls diagnosticTone (lean-binary).
#include "retropp/gb_audio.h"

#include <cstdint>
#include <span>

#include "retropp/audio_library.h"               // the single catalog the tone registers on
#include "src/vm/gameboy/gb_routine_bytecode.h"  // routinebytes::kTone — compile-time-baked tone bytecode

namespace retropp::sameboy {

AudioId diagnosticTone(AudioType type) {
    // Register the baked tone on the single catalog as an SM83 chiptune (the RAW door — ready bytes). Any
    // Game Boy AudioSystem can then cue the returned handle; play() verifies the SM83 ISA. kTone is
    // odr-used only here, so the tone drops from the binary entirely if no game calls diagnosticTone.
    return AudioLibrary::instance().uploadAudio(std::span<const std::uint8_t>(routinebytes::kTone),
                                                type, Isa::Sm83);
}

}  // namespace retropp::sameboy
