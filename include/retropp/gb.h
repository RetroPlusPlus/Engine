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

// ── Driver-hosting hardware vocabulary ──────────────────────────────────────────────────────────
// A bank-qualified ROM base for a placed driver image: the switchable-window address `addr16`
// (0x4000–0x7FFF) as seen from CPU bank `bank`. It folds the bank into the high bits of the 32-bit base
// value (the backend decodes it to a physical cartridge offset). Use it in a DriverImage.base for banked
// placement (Pokémon Crystal's $3A engine + its $07/$33/$3B/$3C/$3D/$5E data banks); a flat driver
// (Tetris) uses a plain address in the low 32 KiB. `bank` must be ≥ 1 and `addr16` in 0x4000–0x7FFF —
// bank 0 (the fixed ROM region, 0x0000–0x3FFF) is a plain address, not a banked one.
constexpr std::uint32_t banked(unsigned bank, std::uint16_t addr16) noexcept {
    return (static_cast<std::uint32_t>(bank) << 16) | addr16;
}

// The MBC3 cartridge mapper (MBC3+TIMER+RAM+BATTERY, cartridge-type byte $10) — Pokémon Crystal's, the
// main consumer's. Declare it in a DriverBinding.mapper for banked placement; the backend sizes the
// synthetic cartridge to the highest placed bank and lets SameBoy's own MBC3 model make the driver's
// bank switching authentic. A flat driver leaves DriverBinding.mapper at its default (none).
inline constexpr Mapper Mbc3 = Mapper::fromId(0x10);

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

// ── The machine's own memories ──────────────────────────────────────────────────────────────────
// Each is a MemoryRegion exactly as gb::A is a Location: the same value a game fills in for its own
// content, filled in here for the hardware. Read or write one straight away —
//
//   const std::vector<std::uint8_t> tiles = vm.read(gb::VRam);
//
// — or name a piece of one by building a MemoryRegion at that address instead.
//
// These are the areas that are the same size on every cartridge, so they can be constants. The
// cartridge itself is not among them: its size is whatever image is hosted, so name a place inside
// it with an address (gb::banked for a higher bank) rather than reaching for a constant.
//
// `count` is 1 on all of them — a whole memory is the degenerate case of an array with one entry —
// so read(…) with no index hands back the entire area.
//
// VRam and Oam are the banked/mapped views the CPU sees. On the Color the second VRAM bank is
// reached the way the machine reaches it, not by a wider constant.
inline constexpr MemoryRegion VRam    = {.at = 0x8000, .size = 0x2000};  // tile data + maps
inline constexpr MemoryRegion WorkRam = {.at = 0xC000, .size = 0x2000};
inline constexpr MemoryRegion Oam     = {.at = 0xFE00, .size = 0x00A0};  // 40 sprite entries
inline constexpr MemoryRegion Hram    = {.at = 0xFF80, .size = 0x007F};

// The hardware registers as STORED — which for several of them is not what the CPU reads back. rDIV
// (0xFF04) is answered from the divider counter when a routine reads it, and others return bits that
// always read high; neither is in the stored byte. Read a synthesized register through a routine
// (ldh a,[reg]) rather than through this. The RAM areas above have no such gap.
inline constexpr MemoryRegion Io      = {.at = 0xFF00, .size = 0x0080};

}  // namespace retropp::gb
