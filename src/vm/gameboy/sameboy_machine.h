// Internal VM backend: a thin wrapper over SameBoy's surgical toolkit.
//
// This header is INTERNAL — it lives under src/vm/, never include/retropp/, and is
// not part of the engine's public surface. The public VM-host API (VMPlatform,
// registerRoutine → typed callable with developer-declared I/O bindings) sits on
// top of this backend; nothing here is exposed to a game.
//
// The wrapper is pimpl'd so this header pulls no SameBoy (GB_*) type: only
// sameboy_machine.cpp includes gb.h. That keeps GB_* out of any surface a consumer
// compiles against, even this internal header, and lets the test include it without
// SameBoy on its include path.
//
// The operation surface is backend-neutral IN SHAPE so the generic host can lift it
// behind the VMPlatform abstraction: construct-with-model, load a ROM image,
// reset, mutable register and memory access, and run-to-return (step the CPU
// until PC reaches a declared return address).
#ifndef RETROPP_SRC_VM_SAMEBOY_MACHINE_H
#define RETROPP_SRC_VM_SAMEBOY_MACHINE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace retropp::vm {

// The console the backend instantiates. SameBoy is the GameBoy / GameBoyColor
// backend (the only one in v1); the wider VMPlatform selector lives in vm.h.
enum class ConsoleModel {
    GameBoy,       // → GB_MODEL_DMG_B
    GameBoyColor,  // → GB_MODEL_CGB_E
};

// A backend-neutral mirror of the SM83 register file. 16-bit pairs match
// SameBoy's GB_registers_t; the high byte of each pair is the first-named 8-bit
// register (e.g. A is af >> 8). The VM host's I/O bindings name registers here.
struct Registers {
    std::uint16_t af = 0;
    std::uint16_t bc = 0;
    std::uint16_t de = 0;
    std::uint16_t hl = 0;
    std::uint16_t sp = 0;
    std::uint16_t pc = 0;
};

// Which directly-accessible hardware memory to hand back — a selector, not an address range. A subset
// of SameBoy's GB_direct_access_t, covering the memories a routine reads and writes for I/O.
//
// Console-qualified on purpose: this names the Game Boy family's memories, so a second console's
// backend brings its own selector and the two never collide. It is also deliberately NOT called
// MemoryRegion — that name belongs to the public address-range value type.
enum class GbHardwareMemory {
    Rom,
    VRam,
    WorkRam,
    Oam,
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
    std::span<std::uint8_t> memory(GbHardwareMemory region);

    // Step the CPU from its current PC until PC reaches returnAddress, or until
    // maxInstructions have executed (a runaway guard — a routine that never
    // returns must still terminate the call). Returns the number of instructions
    // executed. The instruction at returnAddress is not relied upon to do
    // anything — the call stops once control reaches it.
    std::size_t runToReturn(std::uint16_t returnAddress,
                            std::size_t maxInstructions = 1'000'000);

    // Like runToReturn, but capped and reported in CYCLES rather than instructions: step the CPU from
    // its current PC until PC reaches returnAddress, or until `maxTicks8MHz` SameBoy cycles (8 MHz
    // units) have elapsed (a runaway guard). Returns the 8 MHz ticks actually run. Used for a resident
    // driver's per-frame entry calls, which must be accounted in cycles to pad the frame to the
    // hardware budget. Composes the execution callback (for the return address) with GB_run's tick
    // returns (for the cycle count).
    std::uint64_t runToReturnCycles(std::uint16_t returnAddress, std::uint64_t maxTicks8MHz);

    // ── Audio ─────────────────────────────────────────────────────────────────
    // A produced stereo PCM sample, neutral of the backend's sample type (the GB_*
    // type stays in the .cpp). One per APU output sample once audio is enabled.
    using SampleSink = std::function<void(std::int16_t left, std::int16_t right)>;

    // Enable APU audio output: set the APU's sample rate to `sampleRate` Hz (so it
    // resamples to the sink rate internally), select the hardware-accurate highpass
    // filter, and install the per-sample callback. Idempotent. After this, running
    // the CPU (runForCycles) produces samples into the installed sink.
    void enableAudio(unsigned sampleRate);

    // Install the sink the APU sample callback forwards each produced frame to. The
    // callback fires on the thread that runs the CPU (for the audio chain, the
    // AudioSystem's production thread) — so the sink is the producer side of the
    // audio ring buffer. Pass an empty sink to detach.
    void setSampleSink(SampleSink sink);

    // Run the CPU continuously for ~`ticks8MHz` SameBoy cycles (8 MHz units — twice
    // the 4 MHz T-cycles, since GB_run reports in 8 MHz ticks), WITHOUT a return
    // sentinel — the hosted routine is a continuously-running driver, not a call.
    // The APU produces samples throughout; any installed SampleSink receives them.
    // Returns the actual 8 MHz ticks run (≥ ticks8MHz; a partial last instruction
    // overshoots slightly — the caller carries the remainder for drift-free pacing).
    std::uint64_t runForCycles(std::uint64_t ticks8MHz);

    // Defined in sameboy_machine.cpp. Public only so the backend's TU-local
    // execution callback can name it (it receives the instance via SameBoy's
    // user-data pointer); an incomplete type here, it exposes nothing usable.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SAMEBOY_MACHINE_H
