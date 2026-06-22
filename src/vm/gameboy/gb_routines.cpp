// The Game Boy routine presets. Each preset is authored as a real SM83 assembly
// FILE under src/vm/gameboy/routines/; the constexpr SM83 assembler turns that file's text into
// embedded bytecode AT COMPILE TIME (see gb_routine_bytecode.h), so this TU registers the routine from
// a baked byte span — no .asm file is read at runtime, nothing ships beside the binary. The .asm file
// remains the source of truth, edited with assembly tooling.
#include "retropp/gb_routines.h"

#include <span>

#include "retropp/gb.h"
#include "retropp/vm.h"
#include "src/vm/gameboy/gb_routine_bytecode.h"  // compile-time-baked routine bytecode

namespace retropp::sameboy {

Routine<std::uint8_t()> divRng(Vm& vm) {
    return vm.uploadRoutine<std::uint8_t()>(
        std::span<const std::uint8_t>(routinebytes::kDivRng),
        RoutineBinding{.output = gb::A, .throttle = Throttle::HostSpeed});
}

Routine<std::uint8_t()> dualSeedRng(Vm& vm) {
    return vm.uploadRoutine<std::uint8_t()>(
        std::span<const std::uint8_t>(routinebytes::kDualSeedRng),
        RoutineBinding{.output = gb::A, .throttle = Throttle::HostSpeed});
}

Routine<void()> squareTone(Vm& vm) {
    // A continuously-running audio driver: no inputs, no output, hardware-speed (its APU writes must
    // occur at the original wall-clock cadence). Driven via Vm::startDriver / Vm::stepDriver.
    return vm.uploadRoutine<void()>(
        std::span<const std::uint8_t>(routinebytes::kTone),
        RoutineBinding{.throttle = Throttle::HardwareSpeed});
}

}  // namespace retropp::sameboy
