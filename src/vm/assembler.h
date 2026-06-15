// Generic VM assembler output types (ENG-3.C) — platform-NEUTRAL. Every per-platform assembler (the
// Game Boy's SM83 assembler, a future SNES 65816 assembler, …) produces the same shape: machine-code
// bytes plus the byte offset of each label. These types live here, not in any one platform's
// assembler header, so the generic backend seam (vm_backend.h) names them without depending on a
// specific ISA's assembler.
//
// INTERNAL — under src/vm/, never include/retropp/.
#ifndef RETROPP_SRC_VM_ASSEMBLER_H
#define RETROPP_SRC_VM_ASSEMBLER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace retropp::vm {

// A name → value symbol table: constants an assembler resolves by name (a platform's hardware-
// register addresses, plus labels defined within a routine's source).
using SymbolTable = std::unordered_map<std::string, std::uint32_t>;

// The result of assembling routine source: the machine-code bytes, plus every label's byte offset
// within them (so a routine's entry, or a named cell, is referenced by name rather than a magic
// offset).
struct AssembledRoutine {
    std::vector<std::uint8_t> bytes;
    SymbolTable               labels;  // label name → byte offset within `bytes`
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_ASSEMBLER_H
