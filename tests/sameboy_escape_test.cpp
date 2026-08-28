// Escapes on the real core: what an unwatched machine carries, that a watched address reports while
// the CPU runs, and that the instruction there still executes exactly once.
//
// The device-free suite (guest_escape_test.cpp) pins the surface on a machine with no CPU. These are
// the claims only a CPU can answer.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/gb.h"  // gb::banked — a bank-qualified address in the machine's own vocabulary
#include "retropp/vm.h"
#include "src/vm/gameboy/sameboy_machine.h"
#include "src/vm/vm_testing.h"

namespace {

using retropp::GuestEscape;
using retropp::Vm;
using retropp::VMPlatform;
using retropp::vm::ConsoleModel;
using retropp::vm::Registers;
using retropp::vm::SameBoyMachine;
using retropp::vm::VmTestAccess;

constexpr std::uint16_t kLoop = 0x0150;

// A 32 KiB ROM-only image whose whole program is a two-instruction loop in the header gap — a place
// neither boot ROM overlays, so setting PC there and stepping just works.
//
//   0x0100:  JP 0x0150        (the cartridge entry point, where a booted machine starts)
//   0x0150:  INC A
//   0x0151:  JR -3            (back to 0x0150)
//
// A escapes at 0x0150 exactly as often as INC A executes, so the two counts prove each other.
std::vector<std::uint8_t> makeLoopRom() {
    std::vector<std::uint8_t> rom(0x8000, 0x00);
    rom[0x0147] = 0x00;  // ROM ONLY
    rom[0x0148] = 0x00;  // 32 KiB
    rom[0x0149] = 0x00;  // no RAM

    rom[0x0100] = 0xC3;
    rom[0x0101] = 0x50;
    rom[0x0102] = 0x01;

    rom[0x0150] = 0x3C;  // INC A
    rom[0x0151] = 0x18;
    rom[0x0152] = 0xFD;  // JR -3
    return rom;
}

// ── What an unwatched machine carries ───────────────────────────────────────────────────────────

TEST(SameBoyEscapes, AMachineWatchingNothingCarriesNoHook) {
    SameBoyMachine m(ConsoleModel::GameBoyColor);
    m.loadRom(makeLoopRom());
    m.reset();

    EXPECT_FALSE(m.hookInstalled());
    m.setRegisters(Registers{.pc = kLoop});
    m.runForCycles(200);
    EXPECT_FALSE(m.hookInstalled());
}

TEST(SameBoyEscapes, WatchingAnAddressInstallsTheHookAndDroppingItRemovesIt) {
    SameBoyMachine m(ConsoleModel::GameBoyColor);
    m.loadRom(makeLoopRom());
    m.reset();
    m.setEscapeSink([](std::uint16_t) {});

    EXPECT_FALSE(m.hookInstalled());
    m.armEscape(kLoop);
    EXPECT_TRUE(m.hookInstalled());
    EXPECT_EQ(m.armedEscapeCount(), 1u);

    m.disarmEscape(kLoop);
    EXPECT_FALSE(m.hookInstalled());
    EXPECT_EQ(m.armedEscapeCount(), 0u);
}

TEST(SameBoyEscapes, AWatchedAddressWithNowhereToReportInstallsNothing) {
    SameBoyMachine m(ConsoleModel::GameBoyColor);
    m.loadRom(makeLoopRom());
    m.reset();

    m.armEscape(kLoop);  // watched, but no sink: nothing could be told about it
    EXPECT_FALSE(m.hookInstalled());
}

TEST(SameBoyEscapes, ARunToReturnKeepsTheHookWhileAnAddressIsStillWatched) {
    SameBoyMachine m(ConsoleModel::GameBoyColor);
    m.loadRom(makeLoopRom());
    m.reset();
    m.setEscapeSink([](std::uint16_t) {});
    m.armEscape(kLoop);

    m.setRegisters(Registers{.pc = 0x0160});
    m.runToReturn(0x0161, 4);
    EXPECT_TRUE(m.hookInstalled());  // the run ended; the watch did not

    m.disarmEscape(kLoop);
    EXPECT_FALSE(m.hookInstalled());
}

// ── Firing, and what happens to the instruction that was reached ────────────────────────────────

TEST(SameBoyEscapes, AWatchedAddressReportsItselfWhileTheCpuRuns) {
    SameBoyMachine m(ConsoleModel::GameBoyColor);
    m.loadRom(makeLoopRom());
    m.reset();

    int           fired = 0;
    std::uint16_t seen  = 0;
    m.setEscapeSink([&](std::uint16_t address) {
        ++fired;
        seen = address;
    });
    m.armEscape(kLoop);

    m.setRegisters(Registers{.pc = kLoop});
    m.runForCycles(320);

    EXPECT_GT(fired, 0);
    EXPECT_EQ(seen, kLoop);
}

// The instruction at a watched address executes once the handler returns, whatever the handler did.
// INC A runs exactly as many times as the address reported, which is what "observed, never replaced"
// means where it can be counted.
TEST(SameBoyEscapes, TheInstructionAtAWatchedAddressStillExecutesExactlyOncePerReport) {
    SameBoyMachine m(ConsoleModel::GameBoyColor);
    m.loadRom(makeLoopRom());
    m.reset();

    int fired = 0;
    m.setEscapeSink([&](std::uint16_t) { ++fired; });
    m.armEscape(kLoop);

    m.setRegisters(Registers{.af = 0x0000, .pc = kLoop});
    m.runForCycles(320);

    ASSERT_GT(fired, 0);
    ASSERT_LT(fired, 256);  // A is eight bits; keep the budget inside one wrap
    EXPECT_EQ(m.registers().af >> 8, static_cast<std::uint16_t>(fired));
}

// The same loop with nothing watched advances A identically: watching costs the guest no time of its
// own, so the same cycle budget does the same amount of guest work either way.
TEST(SameBoyEscapes, WatchingAnAddressCostsTheGuestNoneOfItsOwnTime) {
    const auto runLoop = [](bool watched) {
        SameBoyMachine m(ConsoleModel::GameBoyColor);
        m.loadRom(makeLoopRom());
        m.reset();
        if (watched) {
            m.setEscapeSink([](std::uint16_t) {});
            m.armEscape(kLoop);
        }
        m.setRegisters(Registers{.af = 0x0000, .pc = kLoop});
        m.runForCycles(320);
        return static_cast<int>(m.registers().af >> 8);
    };

    EXPECT_EQ(runLoop(true), runLoop(false));
}

// ── Through the public surface, on a cartridge that is running ──────────────────────────────────

TEST(SameBoyEscapes, AnEscapeFiresOnTheRunningCartridge) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeLoopRom());

    int fired = 0;
    machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "loop", .at = kLoop, .handler = [&](Vm&, std::uint32_t) { ++fired; }}));

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();

    EXPECT_GT(fired, 0);
}

TEST(SameBoyEscapes, AnEscapeSwitchedOffDoesNotFireOnTheRunningCartridge) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeLoopRom());

    int fired = 0;
    machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "loop", .at = kLoop, .handler = [&](Vm&, std::uint32_t) { ++fired; }}));
    machine.escapes()["loop"].armed(false);

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();

    EXPECT_EQ(fired, 0);
}

// ── Replacing a routine: `.replaces` answers in the routine's own calling convention ────────────

constexpr std::uint32_t kRule   = 0x0180;
constexpr std::uint32_t kSeed   = 0xC000;
constexpr std::uint32_t kResult = 0xC001;

struct Places {
    retropp::MemoryRegion seed;
    retropp::MemoryRegion result;
};

// A cartridge whose loop calls its own routine forever, in a REGISTER convention — the case a raw
// handler could never answer for. The loop carries the seed to the routine in B and stores whatever
// comes back in A; the routine's own rule is to double it.
std::vector<std::uint8_t> makeCallingRom() {
    std::vector<std::uint8_t> rom(0x8000, 0x00);
    rom[0x0147] = 0x00;
    rom[0x0148] = 0x00;
    rom[0x0100] = 0xC3;
    rom[0x0101] = 0x50;
    rom[0x0102] = 0x01;  // jp $0150

    rom[0x0150] = 0xFA;
    rom[0x0151] = 0x00;
    rom[0x0152] = 0xC0;  // ld a,[$C000]
    rom[0x0153] = 0x47;  // ld b,a       — the seed rides to the routine in B
    rom[0x0154] = 0xCD;
    rom[0x0155] = 0x80;
    rom[0x0156] = 0x01;  // call $0180
    rom[0x0157] = 0xEA;
    rom[0x0158] = 0x01;
    rom[0x0159] = 0xC0;  // ld [$C001],a — the answer comes back in A
    rom[0x015A] = 0x18;
    rom[0x015B] = 0xF4;  // jr -12, back to $0150

    rom[0x0180] = 0x78;  // ld a,b
    rom[0x0181] = 0x87;  // add a,a
    rom[0x0182] = 0xC9;  // ret
    return rom;
}

retropp::RegionMapId<Places> declarePlaces(Vm& machine) {
    return machine.registerRegions(retropp::regions(
        retropp::region(&Places::seed, retropp::MemoryRegion{.at = kSeed, .size = 1}, "seed"),
        retropp::region(&Places::result, retropp::MemoryRegion{.at = kResult, .size = 1},
                        "result")));
}

TEST(SameBoyEscapes, UnreplacedTheCartridgeAnswersWithItsOwnRule) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeCallingRom());
    const auto places = declarePlaces(machine);

    VmTestAccess::runInline(machine);
    machine.write(places, &Places::seed, std::vector<std::uint8_t>{21});
    VmTestAccess::stepOnce(machine);

    EXPECT_EQ(machine.read(places, &Places::result).at(0), 42);
    machine.stop();
}

// The replacement speaks the routine's own register convention through its binding: the engine reads
// B out of the parked machine (the guest's caller put the seed there), calls the native function, and
// writes its answer into A — where the caller was always going to look. The caller reads a correct
// answer in the SAME step, which is the property the binding exists to deliver.
TEST(SameBoyEscapes, AReplacementAnswersInTheRoutinesOwnRegisters) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeCallingRom());
    const auto places = declarePlaces(machine);

    machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "answer the rule", .at = kRule,
        .replaces = retropp::routine(
            retropp::RoutineBinding{.inputs = {retropp::gb::B}, .output = retropp::gb::A},
            [](std::uint8_t seed) -> std::uint8_t { return static_cast<std::uint8_t>(seed + 100); })}));

    VmTestAccess::runInline(machine);
    machine.write(places, &Places::seed, std::vector<std::uint8_t>{21});
    VmTestAccess::stepOnce(machine);

    EXPECT_EQ(machine.read(places, &Places::result).at(0), 121);  // the native rule, not 42
    machine.stop();
}

// While a replacement is armed, the routine's entry holds the machine's own return instead of its
// first byte; disarming puts the byte back exactly. Asserted as displaced-and-restored rather than as
// any particular opcode — which instruction a console returns with is its backend's business.
TEST(SameBoyEscapes, AReplacementDisplacesTheEntryByteAndRestoresIt) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeCallingRom());
    const retropp::MemoryRegion entry{.at = kRule, .size = 1};
    const std::uint8_t original = machine.read(entry).at(0);

    machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "answer the rule", .at = kRule,
        .replaces = retropp::routine(
            retropp::RoutineBinding{.inputs = {retropp::gb::B}, .output = retropp::gb::A},
            [](std::uint8_t seed) -> std::uint8_t { return seed; })}));
    EXPECT_NE(machine.read(entry).at(0), original);

    machine.escapes()["answer the rule"].armed(false);
    EXPECT_EQ(machine.read(entry).at(0), original);  // the routine is the guest's own again

    machine.escapes()["answer the rule"].armed(true);
    EXPECT_NE(machine.read(entry).at(0), original);

    machine.escapes()["answer the rule"].remove();
    EXPECT_EQ(machine.read(entry).at(0), original);
}

// ── A banked escape names one bank's byte, and only that one ────────────────────────────────────

// A 64 KiB MBC1 cartridge. The program selects ROM bank 2 and then calls $4000 — the switchable
// window — forever. The same address in bank 3 holds identical bytes and is never mapped, so an
// escape declared there is a test of the bank check and nothing else.
std::vector<std::uint8_t> makeBankedRom() {
    std::vector<std::uint8_t> rom(0x10000, 0x00);
    rom[0x0147] = 0x01;  // MBC1
    rom[0x0148] = 0x01;  // 64 KiB — four banks
    rom[0x0100] = 0xC3;
    rom[0x0101] = 0x50;
    rom[0x0102] = 0x01;  // jp $0150

    rom[0x0150] = 0x3E;
    rom[0x0151] = 0x02;  // ld a,2
    rom[0x0152] = 0xEA;
    rom[0x0153] = 0x00;
    rom[0x0154] = 0x20;  // ld [$2000],a — select ROM bank 2
    rom[0x0155] = 0xCD;
    rom[0x0156] = 0x00;
    rom[0x0157] = 0x40;  // call $4000 — into the switchable window
    rom[0x0158] = 0x18;
    rom[0x0159] = 0xFB;  // jr -5, back to the call

    rom[2 * 0x4000] = 0xC9;  // bank 2 @ $4000: ret
    rom[3 * 0x4000] = 0xC9;  // bank 3 @ $4000: the same byte, never mapped
    return rom;
}

TEST(SameBoyEscapes, ABankedEscapeFiresOnlyWhileItsOwnBankIsMapped) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeBankedRom());

    int inBankTwo   = 0;
    int inBankThree = 0;
    // The unmapped bank is declared FIRST on purpose. Both escapes decode to the same address in the
    // switchable window, so a machine that reached that address and ignored which bank was live would
    // answer with whichever was declared first — and this order makes that answer the wrong one.
    machine.registerEscapes(retropp::escapes(
        GuestEscape{.key     = "bank three",
                    .at      = retropp::gb::banked(3, 0x4000),
                    .handler = [&](Vm&, std::uint32_t) { ++inBankThree; }},
        GuestEscape{.key     = "bank two",
                    .at      = retropp::gb::banked(2, 0x4000),
                    .handler = [&](Vm&, std::uint32_t) { ++inBankTwo; }}));

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();

    EXPECT_GT(inBankTwo, 0);      // the mapped bank's escape fires
    EXPECT_EQ(inBankThree, 0);    // the same address in an unmapped bank does not
}

TEST(SameBoyEscapes, DeclaringEscapesOnARunningMachineIsRefused) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeLoopRom());
    VmTestAccess::runInline(machine);

    EXPECT_THROW(machine.registerEscapes(retropp::escapes(GuestEscape{
                     .key = "loop", .at = kLoop, .handler = [](Vm&, std::uint32_t) {}})),
                 std::logic_error);
    machine.stop();
}

}  // namespace
