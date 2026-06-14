// Internal SM83 / Game Boy VM backend (ENG-3.B) — the v1 concrete VmBackend, over SameBoyMachine.
//
// This is the ONE place SM83 / Game Boy machine idiom lives: the register-id → SM83 register mapping,
// the Game Boy memory map (ROM / WRAM / IO / HRAM), the boot-ROM-safe code arena, and the
// sentinel-on-stack run-to-return frame. The generic Vm (vm.cpp) drives it only through VmBackend,
// so it knows none of this. A future SNES / NES / Genesis backend is an independent VmBackend
// implementation over its own machine.
//
// INTERNAL — under src/vm/, never include/gbcpp/. Pulls no SameBoy GB_* type (SameBoyMachine is
// itself pimpl'd); it does include the PUBLIC gb.h for the SM83 register-id authority (gb::Reg).
#ifndef GBCPP_SRC_VM_SAMEBOY_BACKEND_H
#define GBCPP_SRC_VM_SAMEBOY_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "src/vm/gameboy/sameboy_machine.h"
#include "src/vm/vm_backend.h"

namespace gbcpp::vm {

class SameBoyBackend final : public VmBackend {
public:
    explicit SameBoyBackend(ConsoleModel model);

    void reset() override;
    void advanceClock(std::uint64_t cycles) override;
    std::uint32_t placeRoutine(std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] AssembledRoutine assemble(std::string_view source) const override;
    [[nodiscard]] int registerWidthBytes(std::uint16_t registerId) const override;
    [[nodiscard]] bool addressIsAccessible(std::uint32_t address) const override;

    void beginCall(std::uint32_t entry) override;
    void writeRegister(std::uint16_t registerId, std::uint64_t value, int width) override;
    void writeMemory(std::uint32_t address, std::uint64_t value, int width) override;
    void run() override;
    [[nodiscard]] std::uint64_t readRegister(std::uint16_t registerId) override;
    [[nodiscard]] std::uint64_t readMemory(std::uint32_t address, int width) override;

private:
    // Map an absolute Game Boy address to its region + offset; throws std::out_of_range if the
    // address is not in a directly-accessible region.
    std::span<std::uint8_t> regionFor(std::uint32_t address, std::size_t& offsetOut);

    SameBoyMachine machine_;
    std::uint16_t  nextOffset_;       // next free byte in the code arena
    Registers      pending_{};        // register file accumulated between beginCall and run
};

}  // namespace gbcpp::vm

#endif  // GBCPP_SRC_VM_SAMEBOY_BACKEND_H
