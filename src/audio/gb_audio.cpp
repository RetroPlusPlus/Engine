// ENG-4.A — Game Boy audio presets. Console-specific conveniences over the generic AudioSystem; the
// Game Boy specifics (the wave-channel tone source) live here, never on the generic surface.
#include "gbcpp/gb_audio.h"

#include <string>

// The directory holding the Game Boy routine .asm files (the built-in diagnostic tone lives here),
// baked in at build time — the same define the gb_routines presets use.
#ifndef GBCPP_VM_GAMEBOY_ROUTINES_DIR
#error "GBCPP_VM_GAMEBOY_ROUTINES_DIR must be defined (the Game Boy routine .asm directory)"
#endif

namespace gbcpp::sameboy {

AudioId diagnosticTone(AudioSystem& audio, AudioType type) {
    return audio.registerAudio(std::string(GBCPP_VM_GAMEBOY_ROUTINES_DIR) + "/tone.asm", type);
}

}  // namespace gbcpp::sameboy
