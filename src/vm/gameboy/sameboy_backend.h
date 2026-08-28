// Internal SM83 / Game Boy VM backend — the v1 concrete VmBackend, over SameBoyMachine.
//
// This is the ONE place SM83 / Game Boy machine idiom lives: the register-id → SM83 register mapping,
// the Game Boy memory map (ROM / WRAM / IO / HRAM), the boot-ROM-safe code arena, and the
// sentinel-on-stack run-to-return frame. The generic Vm (vm.cpp) drives it only through VmBackend,
// so it knows none of this. A future SNES / NES / Genesis backend is an independent VmBackend
// implementation over its own machine.
//
// INTERNAL — under src/vm/, never include/retropp/. Pulls no SameBoy GB_* type (SameBoyMachine is
// itself pimpl'd); it does include the PUBLIC gb.h for the SM83 register-id authority (gb::Reg).
#ifndef RETROPP_SRC_VM_SAMEBOY_BACKEND_H
#define RETROPP_SRC_VM_SAMEBOY_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "src/vm/gameboy/sameboy_machine.h"
#include "src/vm/vm_backend.h"

namespace retropp::vm {

class SameBoyBackend final : public VmBackend {
public:
    explicit SameBoyBackend(ConsoleModel model);

    void reset() override;
    void advanceClock(std::uint64_t cycles) override;
    std::uint32_t placeRoutine(std::span<const std::uint8_t> bytes) override;
    void loadRom(std::span<const std::uint8_t> rom) override;
    void bootHostedRom() override;
    [[nodiscard]] AssembledRoutine assemble(std::string_view source) const override;
    [[nodiscard]] int registerWidthBytes(std::uint16_t registerId) const override;
    [[nodiscard]] bool regionIsAddressable(const MemoryRegion& region) const override;
    void readRegion(const MemoryRegion& region, std::uint32_t index,
                    std::span<std::uint8_t> out) override;
    void writeRegion(const MemoryRegion& region, std::uint32_t index,
                     std::span<const std::uint8_t> bytes) override;

    void beginCall(std::uint32_t entry) override;
    void writeRegister(std::uint16_t registerId, std::uint64_t value, int width) override;
    void writeMemory(std::uint32_t address, std::uint64_t value, int width) override;
    void run() override;
    [[nodiscard]] std::uint64_t readRegister(std::uint16_t registerId) override;
    [[nodiscard]] std::uint64_t readMemory(std::uint32_t address, int width) override;

    // Audio chain: enable the SameBoy APU + drive a continuous hardware-speed driver.
    void enableAudio(unsigned sampleRate, AudioSampleSink sink) override;
    void beginContinuous(std::uint32_t entry) override;
    std::uint64_t runForCycles(std::uint64_t cpuCycles) override;

    // Resident driver: configure a (possibly banked) cartridge image and call its entries per frame.
    void configureResidentImage(std::span<const DriverImage> images, Mapper mapper,
                                std::uint32_t stackTop) override;
    std::uint64_t callResident(std::uint32_t entry, std::span<const ResidentRegister> presets,
                               std::uint64_t maxCpuCycles) override;

    // Escapes: watch (possibly bank-qualified) addresses and report each one reached.
    void setEscapeSink(EscapeSink sink) override;
    void armEscape(std::uint32_t address, bool replacesRoutine) override;
    void disarmEscape(std::uint32_t address) override;
    void writeLiveRegister(std::uint16_t registerId, std::uint64_t value, int width) override;

private:
    // One watched address: the encoded form the host armed — reported back verbatim when it fires,
    // so the host matches it to its declaration without decoding anything — beside the decoded pair
    // the machine actually watches. A bank-qualified address names a different byte per bank, so it
    // reports only while its own bank is the mapped one. A replacing escape carries the byte its RET
    // displaced, restored when it is disarmed.
    struct ArmedEscape {
        std::uint32_t encoded  = 0;
        std::uint16_t addr16   = 0;
        unsigned      bank     = 0;
        bool          banked   = false;
        bool          replaces = false;
        std::uint8_t  savedByte = 0;
    };

    // The machine reached a watched address: report the escape whose bank is live, if any.
    void onEscapeReached(std::uint16_t addr16);

    // Map an absolute Game Boy address to its region + offset; throws std::out_of_range if the
    // address is not in a directly-accessible region.
    std::span<std::uint8_t> regionFor(std::uint32_t address, std::size_t& offsetOut);

    // Resolve one entry of a declared place to the memory holding it + the entry's offset within
    // that memory, striding in decoded space. Throws std::out_of_range for an undeclared index or a
    // base that names no byte.
    std::span<std::uint8_t> regionSpanFor(const MemoryRegion& region, std::uint32_t index,
                                          std::size_t& offsetOut);

    // Plant the run-to-return sentinel address on the scratch stack at `stackTop` (work RAM), so a
    // routine's final RET pops it and the run stops. Shared by beginCall and the resident call path.
    void plantSentinel(std::uint16_t stackTop);

    SameBoyMachine machine_;
    std::uint16_t  nextOffset_;       // next free byte in the code arena
    Registers      pending_{};        // register file accumulated between beginCall and run
    std::uint64_t  audioOvershoot8MHz_ = 0;  // ticks a runForCycles overshot its budget, paid back next
    std::uint16_t  residentStackTop_ = 0xDFFC;  // scratch stack top for resident entry calls
    bool           residentConfigured_ = false; // configureResidentImage has run
    bool           romHosted_ = false;          // loadRom has run: the game owns this cartridge
    std::size_t    romBytes_ = 0;               // the loaded image's size; set wherever a ROM loads

    std::vector<ArmedEscape> armedEscapes_;  // watched addresses, in the order they were armed
    EscapeSink               escapeSink_;    // where a reached escape is reported
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SAMEBOY_BACKEND_H
