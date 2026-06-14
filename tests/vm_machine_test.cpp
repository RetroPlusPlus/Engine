// ENG-3.A — drives a synthetic SM83 routine through the SameBoy Machine backend:
// load a hand-assembled ROM image, set input registers + stack, run to the
// routine's return address, read the output from a register and from HRAM. This
// is the first proof the engine's compiled SameBoy core executes instructions
// through the surgical toolkit. No public VM API yet (that is ENG-3.B).
#include "src/vm/gameboy/sameboy_machine.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace gbcpp::vm {
namespace {

// A 32 KiB ROM-only image (the smallest a Game Boy cartridge can be), zero-filled
// (0x00 = NOP) with a synthetic routine and a return-landing self-loop placed in
// the header gap (0x0150 / 0x0160), which neither the DMG nor the CGB boot ROM
// overlays — so the routine reads from cart ROM even with the boot ROM mapped,
// which is why setting PC directly and stepping just works.
constexpr std::size_t kRomSize = 0x8000;
constexpr std::uint16_t kRoutineEntry = 0x0150;
constexpr std::uint16_t kReturnLanding = 0x0160;

std::vector<std::uint8_t> makeSyntheticRom() {
    std::vector<std::uint8_t> rom(kRomSize, 0x00);
    rom[0x0147] = 0x00;  // cartridge type: ROM ONLY (no MBC)
    rom[0x0148] = 0x00;  // ROM size: 32 KiB
    rom[0x0149] = 0x00;  // RAM size: none

    // routine @ 0x0150:  A = A + 5;  HRAM[0] (0xFF80) = A;  RET
    rom[0x0150] = 0xC6; rom[0x0151] = 0x05;  // ADD A, 5
    rom[0x0152] = 0xE0; rom[0x0153] = 0x80;  // LDH (0x80), A   → 0xFF80
    rom[0x0154] = 0xC9;                      // RET

    // landing @ 0x0160:  JR $ (self-loop) — where RET returns; harmless if it
    // executes, it never touches A or HRAM.
    rom[0x0160] = 0x18; rom[0x0161] = 0xFE;  // JR -2
    return rom;
}

TEST(VmMachine, ConstructsBothModels) {
    SameBoyMachine gb(ConsoleModel::GameBoy);
    EXPECT_EQ(gb.model(), ConsoleModel::GameBoy);
    SameBoyMachine cgb(ConsoleModel::GameBoyColor);
    EXPECT_EQ(cgb.model(), ConsoleModel::GameBoyColor);
}

TEST(VmMachine, RegisterRoundTrip) {
    SameBoyMachine m(ConsoleModel::GameBoyColor);
    // F's low nibble is always zero on hardware — keep af's low nibble zero.
    m.setRegisters(Registers{0x1230, 0x4567, 0x89AB, 0xCDEF, 0xFFFE, 0x0100});
    const Registers r = m.registers();
    EXPECT_EQ(r.af, 0x1230);
    EXPECT_EQ(r.bc, 0x4567);
    EXPECT_EQ(r.de, 0x89AB);
    EXPECT_EQ(r.hl, 0xCDEF);
    EXPECT_EQ(r.sp, 0xFFFE);
    EXPECT_EQ(r.pc, 0x0100);
}

TEST(VmMachine, HramIsAddressableAndRoundTrips) {
    SameBoyMachine m(ConsoleModel::GameBoy);
    std::span<std::uint8_t> hram = m.memory(MemoryRegion::Hram);
    ASSERT_EQ(hram.size(), 127u);  // 0xFF80..0xFFFE
    hram[0] = 0xAB;
    EXPECT_EQ(m.memory(MemoryRegion::Hram)[0], 0xAB);
}

// The core proof: a routine that takes A in, computes, writes a register and
// HRAM, and returns — observed end to end through the backend.
TEST(VmMachine, RunsSyntheticRoutineToReturn) {
    SameBoyMachine m(ConsoleModel::GameBoy);
    const std::vector<std::uint8_t> rom = makeSyntheticRom();
    m.loadRom(rom);
    m.reset();

    // Push the return-landing address onto the stack so RET pops it. SP lives in
    // work RAM (0xC000..0xDFFF); 0xDFFC holds the low byte, 0xDFFD the high byte.
    std::span<std::uint8_t> wram = m.memory(MemoryRegion::WorkRam);
    ASSERT_GE(wram.size(), 0x2000u);
    wram[0x1FFC] = kReturnLanding & 0xFF;         // 0xDFFC = 0x60
    wram[0x1FFD] = (kReturnLanding >> 8) & 0xFF;  // 0xDFFD = 0x01

    m.setRegisters(Registers{/*af=*/0x4000, 0, 0, 0, /*sp=*/0xDFFC, /*pc=*/kRoutineEntry});

    const std::size_t executed = m.runToReturn(kReturnLanding);

    EXPECT_EQ(m.registers().pc, kReturnLanding);     // control reached the landing
    EXPECT_EQ(m.registers().af >> 8, 0x45);          // A = 0x40 + 5
    EXPECT_EQ(m.memory(MemoryRegion::Hram)[0], 0x45);  // routine wrote HRAM[0]
    EXPECT_GE(executed, 3u);                         // ADD, LDH, RET (at least)
    EXPECT_LT(executed, 16u);                         // and it terminated promptly
}

// Same routine on the CGB model — proves both backends execute, not just DMG.
TEST(VmMachine, RunsSyntheticRoutineOnGameBoyColor) {
    SameBoyMachine m(ConsoleModel::GameBoyColor);
    const std::vector<std::uint8_t> rom = makeSyntheticRom();
    m.loadRom(rom);
    m.reset();

    std::span<std::uint8_t> wram = m.memory(MemoryRegion::WorkRam);
    ASSERT_GE(wram.size(), 0x2000u);
    wram[0x1FFC] = kReturnLanding & 0xFF;
    wram[0x1FFD] = (kReturnLanding >> 8) & 0xFF;

    m.setRegisters(Registers{0x0700, 0, 0, 0, 0xDFFC, kRoutineEntry});

    m.runToReturn(kReturnLanding);
    EXPECT_EQ(m.registers().af >> 8, 0x0C);            // 0x07 + 5
    EXPECT_EQ(m.memory(MemoryRegion::Hram)[0], 0x0C);
}

// A routine that never reaches the return address must still terminate at the
// instruction cap rather than spinning forever.
TEST(VmMachine, RunawayGuardTerminatesAtCap) {
    SameBoyMachine m(ConsoleModel::GameBoy);
    const std::vector<std::uint8_t> rom = makeSyntheticRom();
    m.loadRom(rom);
    m.reset();
    // Start inside the self-loop; declare a return address it never reaches.
    m.setRegisters(Registers{0, 0, 0, 0, 0xFFFE, kReturnLanding});

    const std::size_t executed = m.runToReturn(/*returnAddress=*/0xFFFF, /*maxInstructions=*/50);
    EXPECT_EQ(executed, 50u);
    EXPECT_EQ(m.registers().pc, kReturnLanding);  // still stuck in the JR self-loop
}

}  // namespace
}  // namespace gbcpp::vm
