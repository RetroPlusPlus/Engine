// The SM83 / Game Boy VM backend implementation. The ONE place SM83 / Game Boy machine
// idiom lives (the generic Vm drives this only through the VmBackend interface).
//
// Execution model (NO ROM): the backend builds a blank 32 KiB cartridge-shaped image with a
// return-landing sentinel in the boot-ROM-safe header gap, loads it once, and resets the machine.
// placeRoutine PATCHES a routine's extracted bytes straight into the loaded code space (via the ROM
// direct-access region) at the next free offset in that gap — no reload, no further reset — so RNG
// state (the seed in HRAM, the rDIV cadence) persists across calls. A call sets PC to the routine's
// entry and SP to a scratch stack top, plants the sentinel return address on the stack, marshals the
// inputs, runs to the sentinel, and reads the output back.
#include "src/vm/gameboy/sameboy_backend.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "retropp/gb.h"  // gb::Reg — the SM83 register-id authority (the generic layer stays neutral)
#include "src/vm/gameboy/gb_symbols.h"      // gbHardwareSymbols() — shared with the compile-time bake
#include "src/vm/gameboy/sm83_assembler.h"

namespace retropp::vm {

namespace {

// ── Game Boy code arena ─────────────────────────────────────────────────────────────────────────
// Routines live in 0x0100–0x01FF, the header gap both the DMG and the CGB boot ROM leave mapped to
// cartridge ROM (the proven window — see tests/vm_machine_test.cpp). We bypass the boot ROM
// by setting PC directly, so the rest of low ROM stays boot-overlaid and unusable; this gap is the
// arena. A fixed JR-$ self-loop at kReturnLanding is the run-to-return sentinel.
constexpr std::size_t   kRomSize       = 0x8000;  // 32 KiB ROM-only image (smallest GB cartridge)
constexpr std::uint16_t kReturnLanding = 0x0150;  // sentinel landing: JR $ (harmless if executed)
constexpr std::uint16_t kRoutineBase   = 0x0160;  // routines are placed from here, growing upward
constexpr std::uint16_t kArenaEnd      = 0x0200;  // first address the CGB boot ROM overlays
constexpr std::uint16_t kStackTop      = 0xDFFC;  // SP scratch top (high WRAM); sentinel planted here

// advanceClock ticks the divider by running the JR-$ sentinel idle; JR $ is 12 T-cycles when taken,
// so one frame's 70'224-cycle budget is exactly 5'852 idle steps.
constexpr std::uint64_t kCyclesPerIdleStep = 12;

// Game Boy memory map bases for the directly-accessible regions a binding can name.
constexpr std::uint16_t kWramBase = 0xC000;
constexpr std::uint16_t kIoBase   = 0xFF00;
constexpr std::uint16_t kHramBase = 0xFF80;

// Marshal a value into a register field of the pending register file. 8-bit registers set the
// matching byte of their pair; 16-bit locations set the whole pair (or SP / PC).
void writeRegisterField(Registers& regs, gb::Reg reg, std::uint64_t value) {
    const auto lo = static_cast<std::uint16_t>(value & 0xFF);
    const auto wide = static_cast<std::uint16_t>(value & 0xFFFF);
    switch (reg) {
        case gb::Reg::A:  regs.af = static_cast<std::uint16_t>((regs.af & 0x00FF) | (lo << 8)); break;
        case gb::Reg::F:  regs.af = static_cast<std::uint16_t>((regs.af & 0xFF00) | lo);        break;
        case gb::Reg::B:  regs.bc = static_cast<std::uint16_t>((regs.bc & 0x00FF) | (lo << 8)); break;
        case gb::Reg::C:  regs.bc = static_cast<std::uint16_t>((regs.bc & 0xFF00) | lo);        break;
        case gb::Reg::D:  regs.de = static_cast<std::uint16_t>((regs.de & 0x00FF) | (lo << 8)); break;
        case gb::Reg::E:  regs.de = static_cast<std::uint16_t>((regs.de & 0xFF00) | lo);        break;
        case gb::Reg::H:  regs.hl = static_cast<std::uint16_t>((regs.hl & 0x00FF) | (lo << 8)); break;
        case gb::Reg::L:  regs.hl = static_cast<std::uint16_t>((regs.hl & 0xFF00) | lo);        break;
        case gb::Reg::AF: regs.af = wide; break;
        case gb::Reg::BC: regs.bc = wide; break;
        case gb::Reg::DE: regs.de = wide; break;
        case gb::Reg::HL: regs.hl = wide; break;
        case gb::Reg::SP: regs.sp = wide; break;
        case gb::Reg::PC: regs.pc = wide; break;
    }
}

std::uint64_t readRegisterField(const Registers& regs, gb::Reg reg) {
    switch (reg) {
        case gb::Reg::A:  return (regs.af >> 8) & 0xFF;
        case gb::Reg::F:  return regs.af & 0xFF;
        case gb::Reg::B:  return (regs.bc >> 8) & 0xFF;
        case gb::Reg::C:  return regs.bc & 0xFF;
        case gb::Reg::D:  return (regs.de >> 8) & 0xFF;
        case gb::Reg::E:  return regs.de & 0xFF;
        case gb::Reg::H:  return (regs.hl >> 8) & 0xFF;
        case gb::Reg::L:  return regs.hl & 0xFF;
        case gb::Reg::AF: return regs.af;
        case gb::Reg::BC: return regs.bc;
        case gb::Reg::DE: return regs.de;
        case gb::Reg::HL: return regs.hl;
        case gb::Reg::SP: return regs.sp;
        case gb::Reg::PC: return regs.pc;
    }
    return 0;  // unreachable; quiets -Wreturn-type
}

}  // namespace

SameBoyBackend::SameBoyBackend(ConsoleModel model)
    : machine_(model), nextOffset_(kRoutineBase) {
    // A blank cartridge image: header bytes for ROM-ONLY / 32 KiB / no-RAM, and the sentinel landing.
    std::vector<std::uint8_t> rom(kRomSize, 0x00);
    rom[0x0147] = 0x00;  // cartridge type: ROM ONLY (no MBC)
    rom[0x0148] = 0x00;  // ROM size: 32 KiB
    rom[0x0149] = 0x00;  // RAM size: none
    rom[kReturnLanding]     = 0x18;  // JR $  (0x18 0xFE) — the run-to-return sentinel
    rom[kReturnLanding + 1] = 0xFE;
    machine_.loadRom(rom);
    machine_.reset();
}

void SameBoyBackend::reset() { machine_.reset(); }

void SameBoyBackend::advanceClock(std::uint64_t cycles) {
    if (cycles == 0) {
        return;
    }
    // Tick the free-running divider without executing a routine: park the CPU at the JR-$ sentinel
    // (kReturnLanding) and run idle instructions, so rDIV advances exactly as it does on hardware
    // between routine calls. JR $ is the engine's busy-idle — each iteration is a fixed handful of
    // cycles; run enough to cover `cycles`. The loop never reaches the (unreachable) return address,
    // so it stops at the instruction cap. Only timing/divider state advances — HRAM, the RNG seed,
    // and placed routine bytes are untouched; the next beginCall resets PC anyway.
    // (The machine runs headless with PPU rendering disabled — see SameBoyMachine's ctor — so running
    // the CPU for extended periods here is safe.)
    Registers regs = machine_.registers();
    regs.pc = kReturnLanding;
    machine_.setRegisters(regs);
    const std::size_t instructions = static_cast<std::size_t>(cycles / kCyclesPerIdleStep);
    if (instructions == 0) {
        return;
    }
    machine_.runToReturn(/*returnAddress=*/0x0000, instructions);  // 0x0000 is never reached from JR $
}

std::uint32_t SameBoyBackend::placeRoutine(std::span<const std::uint8_t> bytes) {
    const std::size_t end = static_cast<std::size_t>(nextOffset_) + bytes.size();
    if (end > kArenaEnd) {
        throw std::runtime_error(
            "Game Boy routine arena exhausted (the boot-safe window 0x0160-0x01FF holds the "
            "registered routines; this one does not fit)");
    }
    std::span<std::uint8_t> rom = machine_.memory(MemoryRegion::Rom);
    const std::uint16_t base = nextOffset_;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        rom[static_cast<std::size_t>(base) + i] = bytes[i];
    }
    nextOffset_ = static_cast<std::uint16_t>(base + bytes.size());
    return base;
}

AssembledRoutine SameBoyBackend::assemble(std::string_view source) const {
    // The Game Boy backend's assembler is SM83 (the engine's own, no external toolchain), with the
    // standard hardware-register names predefined. A future console's backend assembles its own ISA.
    return assembleSm83(source, gbHardwareSymbols());
}

int SameBoyBackend::registerWidthBytes(std::uint16_t registerId) const {
    if (registerId > static_cast<std::uint16_t>(gb::Reg::PC)) {
        return 0;  // not an SM83 register
    }
    const bool wide = registerId >= static_cast<std::uint16_t>(gb::Reg::AF);
    return wide ? 2 : 1;
}

bool SameBoyBackend::addressIsAccessible(std::uint32_t address) const {
    return address <= 0x7FFF ||                              // ROM
           (address >= kWramBase && address <= 0xDFFF) ||    // WRAM
           (address >= kIoBase && address <= 0xFF7F) ||      // IO
           (address >= kHramBase && address <= 0xFFFE);      // HRAM
}

std::span<std::uint8_t> SameBoyBackend::regionFor(std::uint32_t address, std::size_t& offsetOut) {
    if (address <= 0x7FFF) {
        offsetOut = address;
        return machine_.memory(MemoryRegion::Rom);
    }
    if (address >= kWramBase && address <= 0xDFFF) {
        offsetOut = address - kWramBase;
        return machine_.memory(MemoryRegion::WorkRam);
    }
    if (address >= kHramBase && address <= 0xFFFE) {
        offsetOut = address - kHramBase;
        return machine_.memory(MemoryRegion::Hram);
    }
    if (address >= kIoBase && address <= 0xFF7F) {
        offsetOut = address - kIoBase;
        return machine_.memory(MemoryRegion::Io);
    }
    throw std::out_of_range(
        "Game Boy address " + std::to_string(address) +
        " is not in a directly-accessible region (ROM / WRAM / IO / HRAM)");
}

void SameBoyBackend::plantSentinel(std::uint16_t stackTop) {
    // Plant the sentinel return address on the stack so the routine's RET pops it and the run stops.
    std::span<std::uint8_t> wram = machine_.memory(MemoryRegion::WorkRam);
    const std::size_t spOffset = static_cast<std::size_t>(stackTop) - kWramBase;
    wram[spOffset]     = static_cast<std::uint8_t>(kReturnLanding & 0xFF);
    wram[spOffset + 1] = static_cast<std::uint8_t>((kReturnLanding >> 8) & 0xFF);
}

void SameBoyBackend::beginCall(std::uint32_t entry) {
    pending_ = Registers{};
    pending_.pc = static_cast<std::uint16_t>(entry);
    pending_.sp = kStackTop;
    plantSentinel(kStackTop);
}

void SameBoyBackend::writeRegister(std::uint16_t registerId, std::uint64_t value, int /*width*/) {
    writeRegisterField(pending_, static_cast<gb::Reg>(registerId), value);
}

void SameBoyBackend::writeMemory(std::uint32_t address, std::uint64_t value, int width) {
    std::size_t off = 0;
    std::span<std::uint8_t> region = regionFor(address, off);
    for (int i = 0; i < width; ++i) {
        region[off + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);  // little-endian (SM83)
    }
}

void SameBoyBackend::run() {
    machine_.setRegisters(pending_);
    machine_.runToReturn(kReturnLanding);
}

std::uint64_t SameBoyBackend::readRegister(std::uint16_t registerId) {
    return readRegisterField(machine_.registers(), static_cast<gb::Reg>(registerId));
}

std::uint64_t SameBoyBackend::readMemory(std::uint32_t address, int width) {
    std::size_t off = 0;
    std::span<std::uint8_t> region = regionFor(address, off);
    std::uint64_t value = 0;
    for (int i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(region[off + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return value;
}

// ── Audio chain ─────────────────────────────────────────────────────────────────────────────────

void SameBoyBackend::enableAudio(unsigned sampleRate, AudioSampleSink sink) {
    machine_.enableAudio(sampleRate);
    machine_.setSampleSink(std::move(sink));
    audioOvershoot8MHz_ = 0;
}

void SameBoyBackend::beginContinuous(std::uint32_t entry) {
    // Position the machine at the driver's entry with a scratch stack — applied immediately (unlike
    // beginCall, which only stages pending_ for run()). No return sentinel: the driver runs forever;
    // runForCycles stops it on a cycle budget, not a return. Subsequent runForCycles calls continue
    // from wherever the driver left off (its idle loop), sustaining the APU output.
    Registers regs{};
    regs.pc = static_cast<std::uint16_t>(entry);
    regs.sp = kStackTop;
    machine_.setRegisters(regs);
    audioOvershoot8MHz_ = 0;
}

std::uint64_t SameBoyBackend::runForCycles(std::uint64_t cpuCycles) {
    // The TimingProfile CPU unit is 4 MHz T-cycles; SameBoy counts in 8 MHz ticks (GB_run's unit), so
    // a frame's 70'224 T-cycles is 140'448 ticks. Pay back any overshoot the previous run carried, so
    // the long-run production rate tracks real time exactly (no slow drift, no periodic dropped frame).
    const std::uint64_t budget8MHz = cpuCycles * 2;
    if (budget8MHz <= audioOvershoot8MHz_) {
        audioOvershoot8MHz_ -= budget8MHz;  // still paying back a prior overshoot; run nothing this tick
        return 0;
    }
    const std::uint64_t target = budget8MHz - audioOvershoot8MHz_;
    const std::uint64_t ran = machine_.runForCycles(target);
    audioOvershoot8MHz_ = ran - target;  // ran ≥ target (a partial last instruction); carry it forward
    return ran / 2;                       // report in CPU T-cycles
}

// ── Resident driver ───────────────────────────────────────────────────────────────────────────────

void SameBoyBackend::configureResidentImage(std::span<const DriverImage> images, Mapper mapper,
                                            std::uint32_t stackTop) {
    // Resolve + validate the scratch stack top (0 = the platform default; else must be in work RAM).
    if (stackTop != 0 && (stackTop < kWramBase || stackTop > 0xDFFF)) {
        throw std::invalid_argument("resident driver stack top " + std::to_string(stackTop) +
                                    " is not in work RAM (0xC000-0xDFFF)");
    }
    const std::uint16_t resolvedStack =
        (stackTop == 0) ? kStackTop : static_cast<std::uint16_t>(stackTop);

    // The first freely-placeable byte in the fixed bank-0 region: below it is either boot-ROM-overlaid
    // (DMG 0x0000-0x00FF; CGB 0x0000-0x08FF) or the engine-reserved header gap (sentinel + arena,
    // 0x0100-0x01FF). A flat address in the switchable half (0x4000-0x7FFF) is the first bank, unaffected.
    const std::uint32_t firstFreeBank0 =
        (machine_.model() == ConsoleModel::GameBoyColor) ? 0x0900u : 0x0200u;

    // Decode each placement to a physical cartridge range; validate; track overlap + the highest end.
    struct Placed {
        std::uint32_t begin;
        std::uint32_t end;
        std::span<const std::uint8_t> bytes;
    };
    std::vector<Placed> placed;
    placed.reserve(images.size());
    std::uint32_t highestEnd = kRomSize;  // a cartridge is at least 32 KiB

    for (const DriverImage& img : images) {
        if (img.bytes.empty()) {
            throw std::invalid_argument("resident driver image has no bytes");
        }
        const unsigned      bankHi = img.base >> 16;
        const std::uint32_t addr16 = img.base & 0xFFFF;
        std::uint32_t       physical = 0;
        if (bankHi == 0) {
            if (addr16 > 0x7FFF) {
                throw std::invalid_argument(
                    "flat driver image base " + std::to_string(img.base) +
                    " is outside the low 32 KiB (0x0000-0x7FFF); use gb::banked for a higher bank");
            }
            physical = addr16;
            if (physical < 0x4000 && physical < firstFreeBank0) {
                throw std::invalid_argument(
                    "driver image base " + std::to_string(img.base) +
                    " falls in the boot-ROM / engine-reserved low window (first free bank-0 byte is " +
                    std::to_string(firstFreeBank0) + ")");
            }
        } else {
            if (mapper.isNone()) {
                throw std::invalid_argument(
                    "banked driver placement requires a mapper (DriverBinding.mapper); the none mapper "
                    "is a flat 32 KiB image");
            }
            if (addr16 < 0x4000 || addr16 > 0x7FFF) {
                throw std::invalid_argument(
                    "banked driver image address must be in the switchable window 0x4000-0x7FFF");
            }
            physical = bankHi * 0x4000u + (addr16 - 0x4000u);
        }
        const std::uint32_t begin = physical;
        const std::uint32_t end   = physical + static_cast<std::uint32_t>(img.bytes.size());
        // The engine-reserved header gap (sentinel + routine arena, 0x0150-0x0200).
        if (begin < kArenaEnd && end > kReturnLanding) {
            throw std::invalid_argument(
                "driver image overlaps the engine-reserved header gap (sentinel + routine arena)");
        }
        // Any uploaded-routine arena already claimed on this VM.
        if (begin < nextOffset_ && end > kRoutineBase) {
            throw std::invalid_argument("driver image overlaps the uploaded-routine arena on this VM");
        }
        for (const Placed& p : placed) {
            if (begin < p.end && p.begin < end) {
                throw std::invalid_argument("driver images overlap in the cartridge image");
            }
        }
        placed.push_back({begin, end, img.bytes});
        highestEnd = std::max(highestEnd, end);
    }

    // Size the cartridge to a power-of-two byte count holding the highest placement.
    std::uint32_t totalBytes = kRomSize;
    while (totalBytes < highestEnd) {
        totalBytes <<= 1;
    }
    if (mapper.isNone() && totalBytes > kRomSize) {
        throw std::invalid_argument(
            "driver placement exceeds 32 KiB but no mapper is declared (use gb::Mbc3)");
    }
    // ROM-size header byte: log2(totalBytes / 32 KiB).
    std::uint8_t sizeByte = 0;
    for (std::uint32_t s = totalBytes; s > kRomSize; s >>= 1) {
        ++sizeByte;
    }

    // Build the image: header + sentinel + preserved routine arena + the placed images.
    std::vector<std::uint8_t> rom(totalBytes, 0x00);
    rom[0x0147] = static_cast<std::uint8_t>(mapper.id());  // cartridge type (0x00 none / 0x10 MBC3)
    rom[0x0148] = sizeByte;                                // ROM size
    rom[0x0149] = 0x00;                                    // RAM size: none
    rom[kReturnLanding]     = 0x18;                        // JR $ — the run-to-return sentinel
    rom[kReturnLanding + 1] = 0xFE;
    if (nextOffset_ > kRoutineBase) {  // preserve any uploaded-routine bytes from the current image
        std::span<std::uint8_t> oldRom = machine_.memory(MemoryRegion::Rom);
        for (std::uint16_t a = kRoutineBase; a < nextOffset_; ++a) {
            rom[a] = oldRom[a];
        }
    }
    for (const Placed& p : placed) {
        for (std::size_t i = 0; i < p.bytes.size(); ++i) {
            rom[p.begin + i] = p.bytes[i];
        }
    }

    machine_.loadRom(rom);
    machine_.reset();
    residentStackTop_ = resolvedStack;
    residentConfigured_ = true;
    audioOvershoot8MHz_ = 0;
}

std::uint64_t SameBoyBackend::callResident(std::uint32_t entry,
                                           std::span<const ResidentRegister> presets,
                                           std::uint64_t maxCpuCycles) {
    if (!residentConfigured_) {
        throw std::logic_error("callResident: configureResidentImage has not run on this backend");
    }
    Registers regs{};
    regs.pc = static_cast<std::uint16_t>(entry);
    regs.sp = residentStackTop_;
    for (const ResidentRegister& p : presets) {
        writeRegisterField(regs, static_cast<gb::Reg>(p.registerId), p.value);
    }
    machine_.setRegisters(regs);
    plantSentinel(residentStackTop_);
    // Run to the routine's return, counting cycles (the SameBoy 8 MHz tick is twice the 4 MHz CPU unit),
    // capped so a driver entry that never returns still terminates. Report back in CPU T-cycles.
    const std::uint64_t ran8 = machine_.runToReturnCycles(kReturnLanding, maxCpuCycles * 2);
    return ran8 / 2;
}

}  // namespace retropp::vm
