// ENG-4.B — the engine's Game Boy VM routine presets, EMBEDDED as compile-time bytecode.
//
// Each preset's .asm source (exposed by the generated gb_routine_sources.h) is assembled BY THE
// COMPILER via the constexpr SM83 assembler and baked into the binary as a literal byte array. There
// is no runtime .asm file read and nothing to ship beside the binary — the routine's machine code is
// part of the executable. gb_routines.cpp registers these byte spans through Vm::registerRoutine.
//
// The size-then-array two phase is the standard constexpr idiom: a constexpr std::vector (what
// assembleSm83 returns) cannot persist to runtime, so its size seeds a fixed std::array the bytes are
// copied into.
//
// INTERNAL — under src/vm/, never include/retropp/.
#ifndef RETROPP_SRC_VM_GAMEBOY_GB_ROUTINE_BYTECODE_H
#define RETROPP_SRC_VM_GAMEBOY_GB_ROUTINE_BYTECODE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "retropp/generated/gb_routine_sources.h"  // routinesrc::{divRng,dualSeedRng,tone}Source
#include "src/vm/assembler.h"
#include "src/vm/gameboy/gb_symbols.h"
#include "src/vm/gameboy/sm83_assembler.h"

namespace retropp::sameboy::routinebytes {

// Assemble `src` (with the Game Boy hardware symbols) into a fixed N-byte array at compile time.
template <std::size_t N>
constexpr std::array<std::uint8_t, N> bake(std::string_view src) {
    std::array<std::uint8_t, N> out{};
    const vm::AssembledRoutine   r = vm::assembleSm83(src, vm::gbHardwareSymbols());
    for (std::size_t i = 0; i < N && i < r.bytes.size(); ++i) out[i] = r.bytes[i];
    return out;
}

inline constexpr std::size_t kDivRngSize =
    vm::assembleSm83(routinesrc::divRngSource, vm::gbHardwareSymbols()).bytes.size();
inline constexpr std::array<std::uint8_t, kDivRngSize> kDivRng =
    bake<kDivRngSize>(routinesrc::divRngSource);

inline constexpr std::size_t kDualSeedRngSize =
    vm::assembleSm83(routinesrc::dualSeedRngSource, vm::gbHardwareSymbols()).bytes.size();
inline constexpr std::array<std::uint8_t, kDualSeedRngSize> kDualSeedRng =
    bake<kDualSeedRngSize>(routinesrc::dualSeedRngSource);

inline constexpr std::size_t kToneSize =
    vm::assembleSm83(routinesrc::toneSource, vm::gbHardwareSymbols()).bytes.size();
inline constexpr std::array<std::uint8_t, kToneSize> kTone = bake<kToneSize>(routinesrc::toneSource);

}  // namespace retropp::sameboy::routinebytes

#endif  // RETROPP_SRC_VM_GAMEBOY_GB_ROUTINE_BYTECODE_H
