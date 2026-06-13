#pragma once

// Game Boy / SameBoy routine presets (ENG-3.B) — the GB-family catalogue of standard original-
// hardware routines with a fixed I/O convention, so a consumer registers one BY MEANING with zero
// binding boilerplate:
//
//     gbcpp::Vm vm{gbcpp::VMPlatform::GameBoyColor};
//     auto rng = gbcpp::sameboy::dualSeedRng(vm);   // no bytes, no address, no register — just the Vm
//     std::uint8_t roll = rng();
//
// These are KNOWN, STANDARD routines, so the ENGINE OWNS THEM: each preset EMBEDS ITS OWN routine
// bytes (the engine's own assembly of the publicly-documented algorithm — the engine repo ships no
// copyrighted game code), places them in the VM's code space, seals the entry, and builds the
// canonical binding. The consumer supplies nothing but the Vm&. A consumer hosting their OWN
// extracted routine passes its embedded bytes to the generic Vm::registerRoutine instead.
//
// Honesty bounds: dualSeedRng is a general-purpose hardware-entropy RNG whose algorithm is an
// implementation of a specific disassembled routine (see its NOTE); divRng is the *common rDIV-read
// technique* — common, not universal. Both are byte-faithful because their instruction sequence
// (hence the rDIV evolution) matches the documented routine. Software-LFSR RNGs that read no hardware
// register are intentionally NOT here: they are byte-reproducible in native C++ and so native-ported,
// not a VM case.

#include <array>
#include <cstdint>
#include <span>

#include "gbcpp/gb.h"
#include "gbcpp/vm.h"

namespace gbcpp::sameboy {

// divRng — the common Game Boy entropy source: a direct read of the free-running DIV register
// (rDIV, $FF04) as the random byte. The minimal rDIV-dependent RNG, widely used across the library.
// No inputs; returns the rDIV byte in A. Its rDIV-dependence is exactly what makes it a VM case.
//
//   ldh a, [rDIV]   ; F0 04
//   ret             ; C9
inline Routine<std::uint8_t()> divRng(Vm& vm) {
    static constexpr std::array<std::uint8_t, 3> kBytes{0xF0, 0x04, 0xC9};
    return vm.registerRoutine<std::uint8_t()>(
        std::span<const std::uint8_t>(kBytes),
        RoutineBinding{.output = gb::A, .throttle = Throttle::HostSpeed});
}

// dualSeedRng — a general-purpose hardware-entropy RNG: folds the free-running divider (rDIV, $FF04)
// into a dual, carry-chained HRAM seed ($FFE1 / $FFE2) and returns a byte in A. No inputs; the seed
// persists across calls, so the stream mixes well even when rDIV is momentarily steady (unlike the
// stateless divRng). Suitable as the RNG for any Game Boy-family game.
//
// NOTE: this is an implementation of the disassembled Pokémon Gen 1/2 `_Random` algorithm
// (hRandomAdd / hRandomSub). The engine ships its OWN assembly of the publicly-documented routine;
// no copyrighted game code is included. The name is mechanism-descriptive (dual carry-chained seed)
// rather than title-specific.
//
//   ldh a, [seedAdd]   ; F0 E1
//   ld  b, a           ; 47
//   ldh a, [rDIV]      ; F0 04
//   adc b              ; 88
//   ldh [seedAdd], a   ; E0 E1
//   ldh a, [seedSub]   ; F0 E2
//   ld  b, a           ; 47
//   ldh a, [rDIV]      ; F0 04
//   sbc b              ; 98
//   ldh [seedSub], a   ; E0 E2
//   ret                ; C9
inline Routine<std::uint8_t()> dualSeedRng(Vm& vm) {
    static constexpr std::array<std::uint8_t, 17> kBytes{
        0xF0, 0xE1, 0x47, 0xF0, 0x04, 0x88, 0xE0, 0xE1,
        0xF0, 0xE2, 0x47, 0xF0, 0x04, 0x98, 0xE0, 0xE2, 0xC9};
    return vm.registerRoutine<std::uint8_t()>(
        std::span<const std::uint8_t>(kBytes),
        RoutineBinding{.output = gb::A, .throttle = Throttle::HostSpeed});
}

}  // namespace gbcpp::sameboy
