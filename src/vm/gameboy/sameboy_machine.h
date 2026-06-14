// Internal VM backend (ENG-3.A): a thin wrapper over SameBoy's surgical toolkit.
//
// This header is INTERNAL — it lives under src/vm/, never include/gbcpp/, and is
// not part of the engine's public surface. The public VM-host API (VMPlatform,
// registerRoutine → typed callable with developer-declared I/O bindings) is
// ENG-3.B and sits on top of this backend; nothing here is exposed to a game.
//
// The wrapper is pimpl'd so this header pulls no SameBoy (GB_*) type: only
// sameboy_machine.cpp includes gb.h. That keeps acceptance #4 (no GB_* in a
// surface a consumer compiles against) true even for this internal header, and
// lets the test include it without SameBoy on its include path.
//
// The operation surface is backend-neutral IN SHAPE so ENG-3.B can lift it
// behind the VMPlatform abstraction: construct-with-model, load a ROM image,
// reset, mutable register and memory access, and run-to-return (step the CPU
// until PC reaches a declared return address). No throttling, no public typed
// callable — those are ENG-3.B / ENG-4.
#ifndef GBCPP_SRC_VM_SAMEBOY_MACHINE_H
#define GBCPP_SRC_VM_SAMEBOY_MACHINE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace gbcpp::vm {

// The console the backend instantiates. SameBoy is the GameBoy / GameBoyColor
// backend (the only one in v1); the wider VMPlatform selector is ENG-3.B.
enum class ConsoleModel {
    GameBoy,       // → GB_MODEL_DMG_B
    GameBoyColor,  // → GB_MODEL_CGB_E
};

// A backend-neutral mirror of the SM83 register file. 16-bit pairs match
// SameBoy's GB_registers_t; the high byte of each pair is the first-named 8-bit
// register (e.g. A is af >> 8). ENG-3.B's I/O bindings name registers here.
struct Registers {
    std::uint16_t af = 0;
    std::uint16_t bc = 0;
    std::uint16_t de = 0;
    std::uint16_t hl = 0;
    std::uint16_t sp = 0;
    std::uint16_t pc = 0;
};

// The directly-accessible hardware memories this backend exposes. A subset of
// SameBoy's GB_direct_access_t — the regions a routine reads/writes for I/O.
enum class MemoryRegion {
    Rom,
    WorkRam,
    Hram,
    Io,
};

class SameBoyMachine {
public:
    explicit SameBoyMachine(ConsoleModel model);
    ~SameBoyMachine();

    // Non-copyable (owns a SameBoy instance); movable.
    SameBoyMachine(const SameBoyMachine&) = delete;
    SameBoyMachine& operator=(const SameBoyMachine&) = delete;
    SameBoyMachine(SameBoyMachine&&) noexcept;
    SameBoyMachine& operator=(SameBoyMachine&&) noexcept;

    ConsoleModel model() const;

    // Load a ROM image in situ (the routine runs at its address in this image).
    // SameBoy pads short buffers internally; a synthetic blob is fine for tests.
    void loadRom(std::span<const std::uint8_t> rom);

    // Reset to the model's post-reset state.
    void reset();

    Registers registers() const;
    void setRegisters(const Registers& regs);

    // A mutable view of a hardware memory region (live SameBoy storage — writes
    // land in the running machine). Empty if the region is unavailable.
    std::span<std::uint8_t> memory(MemoryRegion region);

    // Step the CPU from its current PC until PC reaches returnAddress, or until
    // maxInstructions have executed (a runaway guard — a routine that never
    // returns must still terminate the call). Returns the number of instructions
    // executed. The instruction at returnAddress is not relied upon to do
    // anything — the call stops once control reaches it.
    std::size_t runToReturn(std::uint16_t returnAddress,
                            std::size_t maxInstructions = 1'000'000);

    // Defined in sameboy_machine.cpp. Public only so the backend's TU-local
    // execution callback can name it (it receives the instance via SameBoy's
    // user-data pointer); an incomplete type here, it exposes nothing usable.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace gbcpp::vm

#endif  // GBCPP_SRC_VM_SAMEBOY_MACHINE_H
