#pragma once

// A value's home in a target machine: a CPU register, or an absolute memory address.
//
// This is the platform-neutral vocabulary the VM host and the driver-hosting surface both name. It
// lives in its own leaf header (no VM, no audio dependency) so the low-level driver-binding vocabulary
// (retropp/driver_binding.h) can name a Location without pulling the full VM host surface (retropp/vm.h),
// while vm.h and the platform headers (retropp/gb.h) keep using it exactly as before.

#include <cstdint>

namespace retropp {

// Where one input or output value lives in the target machine: a CPU register, or an absolute memory
// address. PLATFORM-NEUTRAL — a register is an opaque id whose meaning the selected backend defines;
// a platform header (retropp/gb.h) supplies the typed constants that name them (gb::A, gb::HL, …). The
// address is 32-bit so systems with address spaces wider than 16-bit (e.g. the SNES's 24-bit bus, or a
// bank-qualified Game Boy address) fit without a surface change.
class Location {
public:
    enum class Kind : std::uint8_t { Register, Memory };

    // A register location, identified by a backend-defined id. Consumers use a platform header's
    // typed constants (gb::A, …) rather than calling this directly.
    static constexpr Location reg(std::uint16_t registerId) noexcept {
        Location loc;
        loc.kind_ = Kind::Register;
        loc.id_ = registerId;
        return loc;
    }

    // An absolute memory address in the target machine's address space.
    static constexpr Location memory(std::uint32_t address) noexcept {
        Location loc;
        loc.kind_ = Kind::Memory;
        loc.id_ = address;
        return loc;
    }

    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr std::uint16_t registerId() const noexcept {
        return static_cast<std::uint16_t>(id_);
    }
    [[nodiscard]] constexpr std::uint32_t address() const noexcept { return id_; }

private:
    constexpr Location() = default;
    Kind kind_ = Kind::Register;
    std::uint32_t id_ = 0;  // register id (Kind::Register) or memory address (Kind::Memory)
};

}  // namespace retropp
