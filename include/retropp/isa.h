#pragma once

#include <cstdint>

namespace retropp {

// The instruction-set architecture a VM routine's bytes (or .asm source) are written for. This is the
// true compatibility unit for a routine or a chiptune driver: the bytes only run, and the source only
// assembles, on a VM whose backend speaks the same ISA. It is COARSER than VMPlatform — several consoles
// can share one ISA (the Game Boy and the Game Boy Color both run SM83), so an SM83 chiptune is valid on
// either. A future console adds its ISA here alongside its backend (e.g. Wdc65816 for the SNES).
//
// Lives in its own tiny header (no VM, no audio dependency) so the VM-free AudioLibrary can store an ISA
// tag on a chiptune entry without pulling vm.h, while vm.h supplies the VMPlatform → Isa mapping
// (isaFor) and the assembler that targets it. The audio system stamps each registration with the ISA of
// the system it was registered on, and play() throws if an AudioId is cued on a VM of a different ISA.
enum class Isa : std::uint8_t {
    Sm83,  // Sharp SM83 — the Game Boy / Game Boy Color CPU (the only ISA built in v1)
};

}  // namespace retropp
