#pragma once

// Game Boy / SM83 binding vocabulary — the platform-specific half of the VM host surface.
//
// vm.h is system-agnostic; this header supplies the Game Boy family's CPU register set as typed
// Location constants a routine binding names. The SM83 CPU is shared by the DMG and the CGB, so this
// vocabulary serves both (VMPlatform::GameBoy and ::GameBoyColor). A future system adds its own
// header (snes.h with the 65816's registers, …) — vm.h never changes.
//
// Use the constants directly in a binding:
//   RoutineBinding{ .inputs = {gb::A, gb::B}, .output = gb::A }
// Memory locations (HRAM seeds, IO registers like rDIV at 0xFF04) use the generic
// Location::memory(address) — they are plain addresses in the Game Boy memory map.

#include <cstdint>

#include "retropp/vm.h"

namespace retropp::gb {

// The SM83 register file: the 8-bit registers (A..L) followed by the 16-bit pairs (AF..PC). The
// enumerator order IS the backend register id — the SameBoy backend maps these ids to its register
// file and derives each register's width from this ordering (A..L are 8-bit; AF onward are 16-bit).
// Do not reorder.
enum class Reg : std::uint16_t { A, F, B, C, D, E, H, L, AF, BC, DE, HL, SP, PC };

// A register Location from an SM83 register.
constexpr Location reg(Reg r) noexcept { return Location::reg(static_cast<std::uint16_t>(r)); }

// Typed Location constants — the readable spelling at a binding site (`.output = gb::A`).
inline constexpr Location A  = reg(Reg::A);
inline constexpr Location F  = reg(Reg::F);
inline constexpr Location B  = reg(Reg::B);
inline constexpr Location C  = reg(Reg::C);
inline constexpr Location D  = reg(Reg::D);
inline constexpr Location E  = reg(Reg::E);
inline constexpr Location H  = reg(Reg::H);
inline constexpr Location L  = reg(Reg::L);
inline constexpr Location AF = reg(Reg::AF);
inline constexpr Location BC = reg(Reg::BC);
inline constexpr Location DE = reg(Reg::DE);
inline constexpr Location HL = reg(Reg::HL);
inline constexpr Location SP = reg(Reg::SP);
inline constexpr Location PC = reg(Reg::PC);

}  // namespace retropp::gb
