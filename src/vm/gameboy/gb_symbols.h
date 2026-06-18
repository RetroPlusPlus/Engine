// Game Boy hardware-register symbol table (ENG-3.C / ENG-4.B) — the predefined names a routine's SM83
// source resolves (`rDIV` → $FF04) instead of magic addresses. Keys are lowercase: the assembler
// lowercases symbol tokens before lookup. The standard DMG/CGB I/O map (hardware.inc); routine-local
// HRAM cells are written as raw addresses in the source since they are not hardware names.
//
// `constexpr` and SHARED — this is the ONE definition used both at RUNTIME (SameBoyBackend::assemble)
// and at COMPILE time (baking a preset routine's bytecode, ENG-4.B). A single source of truth, so the
// build-time and runtime assembles can never resolve a name differently.
//
// INTERNAL — under src/vm/, never include/retropp/.
#ifndef RETROPP_SRC_VM_GAMEBOY_GB_SYMBOLS_H
#define RETROPP_SRC_VM_GAMEBOY_GB_SYMBOLS_H

#include "src/vm/assembler.h"  // SymbolTable (a flat constexpr map)

namespace retropp::vm {

// The Game Boy hardware-register addresses, predefined so routine source reads with names. Returned by
// value (constructed fresh per call) — a constexpr SymbolTable cannot be a namespace-scope constant
// (its std::vector storage may not persist past constant evaluation), and assembles are rare, so the
// few-dozen-entry build is immaterial.
constexpr SymbolTable gbHardwareSymbols() {
    return SymbolTable{
        {"rjoyp", 0xFF00}, {"rp1", 0xFF00},   {"rsb", 0xFF01},    {"rsc", 0xFF02},
        {"rdiv", 0xFF04},  {"rtima", 0xFF05}, {"rtma", 0xFF06},   {"rtac", 0xFF07},
        {"rif", 0xFF0F},   {"rlcdc", 0xFF40}, {"rstat", 0xFF41},  {"rscy", 0xFF42},
        {"rscx", 0xFF43},  {"rly", 0xFF44},   {"rlyc", 0xFF45},   {"rdma", 0xFF46},
        {"rbgp", 0xFF47},  {"robp0", 0xFF48}, {"robp1", 0xFF49},  {"rwy", 0xFF4A},
        {"rwx", 0xFF4B},   {"rkey1", 0xFF4D}, {"rvbk", 0xFF4F},   {"rhdma1", 0xFF51},
        {"rhdma2", 0xFF52},{"rhdma3", 0xFF53},{"rhdma4", 0xFF54}, {"rhdma5", 0xFF55},
        {"rrp", 0xFF56},   {"rbcps", 0xFF68}, {"rbcpd", 0xFF69},  {"rocps", 0xFF6A},
        {"rocpd", 0xFF6B}, {"rsvbk", 0xFF70}, {"rie", 0xFFFF},
        // Sound registers (NR10–NR52, $FF10–$FF26) — the APU control surface a sound driver writes.
        {"rnr10", 0xFF10}, {"rnr11", 0xFF11}, {"rnr12", 0xFF12}, {"rnr13", 0xFF13},
        {"rnr14", 0xFF14}, {"rnr21", 0xFF16}, {"rnr22", 0xFF17}, {"rnr23", 0xFF18},
        {"rnr24", 0xFF19}, {"rnr30", 0xFF1A}, {"rnr31", 0xFF1B}, {"rnr32", 0xFF1C},
        {"rnr33", 0xFF1D}, {"rnr34", 0xFF1E}, {"rnr41", 0xFF20}, {"rnr42", 0xFF21},
        {"rnr43", 0xFF22}, {"rnr44", 0xFF23}, {"rnr50", 0xFF24}, {"rnr51", 0xFF25},
        {"rnr52", 0xFF26},
    };
}

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_GAMEBOY_GB_SYMBOLS_H
