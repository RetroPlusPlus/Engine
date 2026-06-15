// ENG-3.C — the Game Boy routine presets. Each preset is authored as a real SM83 assembly FILE under
// src/vm/gameboy/routines/; this TU just points Vm::registerRoutine at that file (the VM reads and
// assembles it in-process) and attaches the canonical binding. No ASM is inlined in C++ here — the
// routine source lives in its .asm file, edited with assembly tooling.
#include "retropp/gb_routines.h"

#include <string>

#include "retropp/gb.h"
#include "retropp/vm.h"

// The directory holding this platform's routine .asm files, baked in at build time (see
// CMakeLists.txt). Engine-owned presets read their source from here.
#ifndef RETROPP_VM_GAMEBOY_ROUTINES_DIR
#error "RETROPP_VM_GAMEBOY_ROUTINES_DIR must be defined (the Game Boy routine .asm directory)"
#endif

namespace retropp::sameboy {
namespace {
std::string routineFile(const char* name) {
    return std::string(RETROPP_VM_GAMEBOY_ROUTINES_DIR) + "/" + name;
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

Routine<void()> squareTone(Vm& vm) {
    // A continuously-running audio driver: no inputs, no output, hardware-speed (its APU writes must
    // occur at the original wall-clock cadence). Driven via Vm::startDriver / Vm::stepDriver.
    return vm.registerRoutine<void()>(
        routineFile("tone.asm"),
        RoutineBinding{.throttle = Throttle::HardwareSpeed});
}

}  // namespace retropp::sameboy
