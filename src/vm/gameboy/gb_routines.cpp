// ENG-3.C — the Game Boy routine presets. Each preset is authored as a real SM83 assembly FILE under
// src/vm/gameboy/routines/; this TU just points Vm::registerRoutine at that file (the VM reads and
// assembles it in-process) and attaches the canonical binding. No ASM is inlined in C++ here — the
// routine source lives in its .asm file, edited with assembly tooling.
#include "gbcpp/gb_routines.h"

#include <string>

#include "gbcpp/gb.h"
#include "gbcpp/vm.h"

// The directory holding this platform's routine .asm files, baked in at build time (see
// CMakeLists.txt). Engine-owned presets read their source from here.
#ifndef GBCPP_VM_GAMEBOY_ROUTINES_DIR
#error "GBCPP_VM_GAMEBOY_ROUTINES_DIR must be defined (the Game Boy routine .asm directory)"
#endif

namespace gbcpp::sameboy {
namespace {
std::string routineFile(const char* name) {
    return std::string(GBCPP_VM_GAMEBOY_ROUTINES_DIR) + "/" + name;
}
}  // namespace

Routine<std::uint8_t()> divRng(Vm& vm) {
    return vm.registerRoutine<std::uint8_t()>(
        routineFile("div_rng.asm"),
        RoutineBinding{.output = gb::A, .throttle = Throttle::HostSpeed});
}

Routine<std::uint8_t()> dualSeedRng(Vm& vm) {
    return vm.registerRoutine<std::uint8_t()>(
        routineFile("dual_seed_rng.asm"),
        RoutineBinding{.output = gb::A, .throttle = Throttle::HostSpeed});
}

}  // namespace gbcpp::sameboy
