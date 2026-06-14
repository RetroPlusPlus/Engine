// Internal VM backend seam (ENG-3.B): the abstract interface every per-system VM backend implements.
//
// This is the boundary that makes the VM host multi-system. The generic Vm (vm.cpp) owns one
// VmBackend chosen by VMPlatform and drives it through this interface; it knows nothing about SM83,
// the Game Boy memory map, or SameBoy. Each system supplies a concrete backend (SameBoyBackend is
// the only one in v1; a SNES / NES / Genesis backend is a drop-in implementation of this same
// interface). All machine idiom for a system lives behind its backend.
//
// This header is INTERNAL — under src/vm/, never include/gbcpp/. It pulls no backend-library type.
#ifndef GBCPP_SRC_VM_VM_BACKEND_H
#define GBCPP_SRC_VM_VM_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "src/vm/assembler.h"  // AssembledRoutine — the assemble() return shape (bytes + labels)

namespace gbcpp::vm {

// The lifecycle the generic host drives per system. A call is: beginCall(entry) → writeRegister /
// writeMemory (marshal inputs) → run() → readRegister / readMemory (read the output). Registers are
// named by the backend-defined register id carried in a Location (the platform header — gb.h — is
// the id authority for the Game Boy family).
class VmBackend {
public:
    virtual ~VmBackend() = default;

    // Reset the machine to its post-reset state, clearing persistent routine state (e.g. RNG seeds).
    // Placed routines stay placed (their bytes live in the code space, untouched by reset).
    virtual void reset() = 0;

    // Advance the machine's free-running clock by `cycles` CPU cycles without executing a routine, so
    // time-based hardware registers (e.g. the Game Boy's rDIV divider) keep ticking between calls as
    // on always-running hardware. Must not disturb routine-visible state (registers/RAM the next call
    // marshals); only the timing/divider state advances.
    virtual void advanceClock(std::uint64_t cycles) = 0;

    // Inject a routine's extracted bytes into the code space and return the absolute entry address of
    // its first byte. Throws (std::runtime_error) if the backend's code arena cannot hold it.
    virtual std::uint32_t placeRoutine(std::span<const std::uint8_t> bytes) = 0;

    // Assemble routine source written in this backend's assembly language into machine-code bytes
    // (+ exported label offsets), using the backend's own ISA assembler and platform symbol defaults
    // (e.g. the Game Boy backend assembles SM83 and predefines rDIV). The generic host calls this for
    // the source form of registerRoutine, then injects the bytes via placeRoutine exactly as for
    // pre-assembled bytes — so a future console's backend brings its own assembler, no shared change.
    // Throws (std::runtime_error, with line context) on a source error.
    [[nodiscard]] virtual AssembledRoutine assemble(std::string_view source) const = 0;

    // The byte-width of a register id (1, 2, or 4), or 0 if the id is not a register on this system.
    // The generic host uses this to validate a value's width against its bound register.
    [[nodiscard]] virtual int registerWidthBytes(std::uint16_t registerId) const = 0;

    // Whether `address` falls in a directly-accessible region this backend can read/write. The
    // generic host uses this to validate a memory binding before a call.
    [[nodiscard]] virtual bool addressIsAccessible(std::uint32_t address) const = 0;

    // Begin a fresh call frame: set the program counter to `entry`, set up a scratch stack, and
    // arrange for run() to terminate when the routine returns.
    virtual void beginCall(std::uint32_t entry) = 0;

    // Marshal one input into the pending frame (register) or live memory (address). Width in bytes.
    virtual void writeRegister(std::uint16_t registerId, std::uint64_t value, int width) = 0;
    virtual void writeMemory(std::uint32_t address, std::uint64_t value, int width) = 0;

    // Run the current call to its return.
    virtual void run() = 0;

    // Read an output back after run(). Width in bytes (for memory).
    [[nodiscard]] virtual std::uint64_t readRegister(std::uint16_t registerId) = 0;
    [[nodiscard]] virtual std::uint64_t readMemory(std::uint32_t address, int width) = 0;
};

}  // namespace gbcpp::vm

#endif  // GBCPP_SRC_VM_VM_BACKEND_H
