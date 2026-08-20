// Internal VM backend seam: the abstract interface every per-system VM backend implements.
//
// This is the boundary that makes the VM host multi-system. The generic Vm (vm.cpp) owns one
// VmBackend chosen by VMPlatform and drives it through this interface; it knows nothing about SM83,
// the Game Boy memory map, or SameBoy. Each system supplies a concrete backend (SameBoyBackend is
// the only one in v1; a SNES / NES / Genesis backend is a drop-in implementation of this same
// interface). All machine idiom for a system lives behind its backend.
//
// This header is INTERNAL — under src/vm/, never include/retropp/. It pulls no backend-library type.
#ifndef RETROPP_SRC_VM_VM_BACKEND_H
#define RETROPP_SRC_VM_VM_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "retropp/driver_binding.h"  // DriverImage / Mapper — the resident-driver image configuration
#include "retropp/memory_region.h"   // MemoryRegion — a declared place in the guest's address space
#include "src/vm/assembler.h"        // AssembledRoutine — the assemble() return shape (bytes + labels)

namespace retropp::vm {

// One register preset the generic host applies live before a resident entry call (the folded argument
// plus any fixed presets). Backend-neutral: a register id (the Location register id) and its value.
struct ResidentRegister {
    std::uint16_t registerId;
    std::uint64_t value;
};

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
    // its first byte. Throws (std::runtime_error) if the backend's code arena cannot hold it, or
    // (std::logic_error) if this machine hosts a game's own cartridge — that image has no arena.
    virtual std::uint32_t placeRoutine(std::span<const std::uint8_t> bytes) = 0;

    // Load a whole cartridge image the game supplies and reset the machine, so the image's bytes are
    // addressable. The backend parses the image's own header; the engine never reads a ROM byte and
    // exposes no cartridge metadata. This makes the image READABLE, not running — there is no boot,
    // no entry point, no execution.
    //
    // Hosting a game's cartridge and hosting an engine-built one are EXCLUSIVE. The resident-driver
    // path synthesizes a cartridge — it writes its own header and places engine content into the
    // gaps — so the engine owns that image; here the game owns it. Each refuses the other rather
    // than silently overwriting it. Throws std::logic_error if the other mode is configured, and
    // std::invalid_argument for an empty image.
    virtual void loadRom(std::span<const std::uint8_t> rom) = 0;

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

    // Whether every byte a region spans is reachable on this machine as it stands. The backend
    // decodes the region's (possibly bank-qualified) base into its own address space and checks the
    // whole extent fits the memory it starts in — a long array crossing bank boundaries is resolved
    // in decoded space, never by arithmetic on the encoded base. The generic host calls this once
    // per entry when a region batch is registered; it never learns the encoding.
    //
    // This is the ONE answer to what memory a machine has. Everything that asks — a region batch, a
    // routine's memory binding, a word read — asks this, so no two callers can be told different
    // things about the same address.
    [[nodiscard]] virtual bool regionIsAddressable(const MemoryRegion& region) const = 0;

    // Whether a single byte at `address` is reachable: the one-byte case of the question above, and
    // implemented as exactly that. The generic host uses it to validate a memory binding.
    [[nodiscard]] bool addressIsAccessible(std::uint32_t address) const {
        return regionIsAddressable(MemoryRegion{.at = address, .size = 1});
    }

    // Copy entry `index` of `region` into `out` (exactly region.size bytes), and the reverse. The
    // backend decodes the region's base into its own address space and strides to the entry THERE —
    // an entry past the first boundary of a banked memory is not at base + index * size, because the
    // encoded base stops describing the run once it leaves the window it names. No caller above the
    // backend does stride arithmetic on an encoded address.
    //
    // Writing a cartridge is allowed: the image is a buffer this process owns, not read-only silicon,
    // and patching a hosted image is a thing a game extending one legitimately does. The write lands
    // in memory only — nothing touches the file the bytes came from, and re-hosting replaces it.
    //
    // Throws std::out_of_range for an index the region does not declare or a region that does not
    // resolve, and std::invalid_argument if `bytes` is not exactly one entry wide.
    virtual void readRegion(const MemoryRegion& region, std::uint32_t index,
                            std::span<std::uint8_t> out) = 0;
    virtual void writeRegion(const MemoryRegion& region, std::uint32_t index,
                             std::span<const std::uint8_t> bytes) = 0;

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

    // ── Audio chain (the hardware-speed driver path) ──────────────────────────────────────────────
    // The producer-side sink the backend's APU forwards each produced PCM frame to. Fires on the
    // thread that steps the driver (for the audio chain, the AudioSystem's production thread).
    // A backend with no audio model leaves it unused.
    using AudioSampleSink = std::function<void(std::int16_t left, std::int16_t right)>;

    // Enable the backend's APU audio at `sampleRate` Hz and route produced frames to `sink`. The
    // generic host calls this once when a consumer wires up the audio chain. A backend with no audio
    // synthesis throws (the seam exists; the GB backend realizes it). Idempotent.
    virtual void enableAudio(unsigned sampleRate, AudioSampleSink sink) = 0;

    // Position the machine at a continuously-running driver routine's entry (set PC + a scratch stack),
    // with NO return sentinel — unlike beginCall, the driver is not run to a return; it is stepped a
    // cycle budget at a time by runForCycles while its APU writes produce audio.
    virtual void beginContinuous(std::uint32_t entry) = 0;

    // Run the positioned driver for `cpuCycles` CPU cycles (the TimingProfile CPU unit — 4 MHz
    // T-cycles, e.g. TimingProfile::cpuCyclesPerTick()); the APU produces ~rate/frameRate frames into
    // the enabled sink during the run. Returns the CPU cycles actually run (≥ cpuCycles; a partial
    // last instruction overshoots — the caller carries the remainder for drift-free pacing).
    virtual std::uint64_t runForCycles(std::uint64_t cpuCycles) = 0;

    // ── Resident driver (the hosted-machine path) ─────────────────────────────────────────────────
    // Configure the machine as a resident-driver host: build a cartridge image sized to hold the
    // highest placed bank, install `mapper` (the backend decodes its opaque id), place each image's
    // bytes at its (possibly bank-qualified) base, then load + reset. `stackTop` relocates the scratch
    // stack (0 = the backend's default scratch top). Preserves any already-placed routine arena. Throws
    // (std::invalid_argument / std::runtime_error) on a banked placement with the none mapper,
    // overlapping placed ranges, placement into the boot-ROM window / engine-reserved header gap, a
    // stack top outside work RAM, or a cartridge the backend cannot address.
    virtual void configureResidentImage(std::span<const DriverImage> images, Mapper mapper,
                                        std::uint32_t stackTop) = 0;

    // Perform one resident entry call: apply `presets` to the register file live, set PC = entry and SP
    // = the configured resident stack top, plant the return sentinel, and run to the routine's return
    // counting CPU cycles (capped at `maxCpuCycles` — a runaway guard for a driver entry that never
    // returns). Returns the CPU cycles consumed. configureResidentImage must have been called first.
    virtual std::uint64_t callResident(std::uint32_t entry,
                                       std::span<const ResidentRegister> presets,
                                       std::uint64_t maxCpuCycles) = 0;
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_VM_BACKEND_H
