#pragma once

// Game Boy / SameBoy routine presets (ENG-3.B/C) — the GB-family catalogue of standard original-
// hardware routines with a fixed I/O convention, so a consumer registers one BY MEANING with zero
// binding boilerplate:
//
//     gbcpp::Vm vm{gbcpp::VMPlatform::GameBoyColor};
//     auto rng = gbcpp::sameboy::dualSeedRng(vm);   // no bytes, no address, no register — just the Vm
//     std::uint8_t roll = rng();
//
// These are KNOWN, STANDARD routines, so the ENGINE OWNS THEM: each preset is authored as a real
// SM83 assembly FILE under src/vm/routines/ (the engine's own implementation of the publicly-
// documented algorithm; the engine repo ships no copyrighted game code), which the VM reads and
// assembles in-process at registration via Vm::registerRoutine's .asm-file form, then places, seals
// the entry, and binds. The consumer supplies nothing but the Vm&. A consumer hosting their OWN
// routine points Vm::registerRoutine at their own .asm file (or passes pre-assembled bytes).
//
// Honesty bounds: dualSeedRng is a general-purpose hardware-entropy RNG whose algorithm is an
// implementation of a specific disassembled routine (see its .asm NOTE); divRng is the *common
// rDIV-read technique* — common, not universal. Both are byte-faithful because their instruction
// sequence (hence the rDIV evolution) matches the documented routine. Software-LFSR RNGs that read
// no hardware register are intentionally NOT here: they are byte-reproducible in native C++ and so
// native-ported, not a VM case.

#include <cstdint>

#include "gbcpp/vm.h"

namespace gbcpp::sameboy {

// divRng — the common Game Boy entropy source: a direct read of the free-running DIV register
// (rDIV, $FF04) as the random byte. The minimal rDIV-dependent RNG, widely used across the library.
// No inputs; returns the rDIV byte in A. Source: src/vm/routines/div_rng.asm.
Routine<std::uint8_t()> divRng(Vm& vm);

// dualSeedRng — a general-purpose hardware-entropy RNG: folds the free-running divider (rDIV, $FF04)
// into a dual, carry-chained HRAM seed ($FFE1 / $FFE2) and returns a byte in A. No inputs; the seed
// persists across calls, so the stream mixes well even when rDIV is momentarily steady (unlike the
// stateless divRng). Suitable as the RNG for any Game Boy-family game.
// Source: src/vm/routines/dual_seed_rng.asm (carries the trademark-safe-naming NOTE).
Routine<std::uint8_t()> dualSeedRng(Vm& vm);

// NOTE: there is intentionally no audio routine preset here. A game configures audio through the
// AudioSystem (gbcpp/audio_system.h): it registers its sound driver and sounds THERE, in audio terms,
// and the AudioSystem hosts them on its own internal VM. What the AudioSystem hides is the VM/Routine/
// throttle layer — not the act of registering audio (that is the game's job and fully exposed). The
// diagnostic square-wave driver (src/vm/gameboy/routines/tone.asm) is registered inside the
// AudioSystem, not exposed as a VM preset.

}  // namespace gbcpp::sameboy
