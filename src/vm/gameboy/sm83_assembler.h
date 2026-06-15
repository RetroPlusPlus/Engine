// In-engine SM83 (Sharp LR35902 / Game Boy CPU) assembler (ENG-3.C) — converts SM83 assembly
// source text into machine-code bytes. NO external toolchain: this is ordinary engine C++, not a
// shell-out to RGBDS or any other assembler. It exists so a routine can be authored as readable
// SM83 assembly instead of a hand-typed hex array, and assembled in-process at registration time
// (the VmBackend::assemble seam) — see eng-3.c Amendment A1.
//
// INTERNAL — under src/vm/, never include/retropp/. SM83-specific (a future console's backend brings
// its own ISA encoder); the generic VM host knows nothing of it.
#ifndef RETROPP_SRC_VM_SM83_ASSEMBLER_H
#define RETROPP_SRC_VM_SM83_ASSEMBLER_H

#include <string_view>

#include "src/vm/assembler.h"  // SymbolTable + AssembledRoutine (platform-neutral)

namespace retropp::vm {

// Assemble SM83 assembly source text into machine code. RGBDS-flavoured syntax — the format the
// Game Boy disassembly is already in: `;` line comments, `label:` definitions, `$hex` / `%bin` /
// decimal literals, `[hl]` / `[$FF04]` memory operands, condition codes (z / nz / c / nc). One
// instruction per line (a label may share a line with an instruction). `predefined` seeds the
// symbol table with platform constants; labels in the source extend it.
//
// Throws std::runtime_error — carrying the 1-based line number and the offending text — on an
// unknown mnemonic, a malformed/again-wrong-shape operand, an unknown symbol, or an out-of-range
// value (e.g. a jr target further than a signed byte reaches).
AssembledRoutine assembleSm83(std::string_view source, const SymbolTable& predefined = {});

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SM83_ASSEMBLER_H
