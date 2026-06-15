// ENG-4.A — Game Boy audio presets. Console-specific conveniences over the generic AudioSystem; the
// Game Boy specifics (the wave-channel tone source) live here, never on the generic surface.
#include "retropp/gb_audio.h"

#include <string>

// The directory holding the Game Boy routine .asm files (the built-in diagnostic tone lives here),
// baked in at build time — the same define the gb_routines presets use.
#ifndef RETROPP_VM_GAMEBOY_ROUTINES_DIR
#error "RETROPP_VM_GAMEBOY_ROUTINES_DIR must be defined (the Game Boy routine .asm directory)"
#endif

namespace retropp::sameboy {

AudioId diagnosticTone(AudioSystem& audio, AudioType type) {
    return audio.registerAudio(std::string(RETROPP_VM_GAMEBOY_ROUTINES_DIR) + "/tone.asm", type);
}

}  // namespace retropp::sameboy
