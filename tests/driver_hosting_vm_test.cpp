// The Vm-layer resident-driver mechanics, exercised through PUBLIC headers only
// (retropp/vm.h, retropp/gb.h, retropp/driver_binding.h) — no backend header in sight, proving the
// surface is self-contained. Each case configures a hosted driver from hand-assembled SM83 images (NO
// ROM — surgically-placed `const` byte blobs), drives the resident tick, and observes results through a
// declared slot. Device-free: every VM core is deterministic, so a placed routine's effect on RAM is
// reproducible without a device.
//
// The mechanics under test: banked + flat placement into a synthetic cartridge, MBC3 bank switching
// observed from placed code, scratch-stack relocation, placement validation (overlap / boot window /
// mapper), cycle-accounted run-to-return, and the tick's apply-queue → call-tick → idle-remainder shape.
#include "retropp/driver_binding.h"
#include "retropp/gb.h"
#include "retropp/vm.h"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace retropp {
namespace {

// One CGB frame's CPU-cycle budget — the per-tick unit the resident tick pads to.
const std::uint64_t kFrame = TimingProfile::GameBoyColor.cpuCyclesPerTick();

// A DriverImage over a static byte array at a base (bank-qualified or flat).
template <std::size_t N>
DriverImage image(const std::array<std::uint8_t, N>& bytes, std::uint32_t base) {
    return DriverImage{.bytes = std::span<const std::uint8_t>(bytes), .base = base};
}

// ── Placement + tick ──────────────────────────────────────────────────────────────────────────

// A flat driver (Tetris shape): one image in the switchable half, a tick entry that writes a constant
// to a work-RAM slot. host → tick → the slot reflects the write.
TEST(DriverHostingVm, FlatDriverPlacesAndTicks) {
    Vm vm{VMPlatform::GameBoyColor};
    // ld a,0x37 ; ld [0xC000],a ; ret
    static constexpr std::array<std::uint8_t, 6> kTick{0x3E, 0x37, 0xEA, 0x00, 0xC0, 0xC9};
    DriverBinding b;
    b.images = {image(kTick, 0x6000)};
    b.tickEntry = 0x6000;
    b.slots = {SlotSpec{.address = 0xC000, .width = 1, .direction = SlotDirection::Read}};

    vm.hostDriver(b);
    vm.tickDriver({}, kFrame);
    EXPECT_EQ(vm.readSlot(0), 0x37u);
}

// The headline banked case: a bank-switching routine in the FIXED bank 0 selects ROM bank 2 through the
// MBC3, reads the first byte of the switchable window (bank 2's data), and stores it to a slot. Proves
// gb::banked placement + gb::Mbc3 sizing + SameBoy's own MBC model handling the switch from placed code.
TEST(DriverHostingVm, BankedPlacementReachesHighBankViaMbcSwitch) {
    Vm vm{VMPlatform::GameBoyColor};
    // ld a,0x02 ; ld [0x2000],a ; ld a,[0x4000] ; ld [0xC000],a ; ret
    static constexpr std::array<std::uint8_t, 12> kSwitch{
        0x3E, 0x02, 0xEA, 0x00, 0x20, 0xFA, 0x00, 0x40, 0xEA, 0x00, 0xC0, 0xC9};
    static constexpr std::array<std::uint8_t, 1> kBank2Data{0xAB};
    DriverBinding b;
    b.mapper = gb::Mbc3;
    b.images = {image(kSwitch, 0x0A00),                 // fixed bank 0 (> the CGB boot window)
                image(kBank2Data, gb::banked(2, 0x4000))};
    b.tickEntry = 0x0A00;
    b.slots = {SlotSpec{.address = 0xC000, .width = 1, .direction = SlotDirection::Read}};

    vm.hostDriver(b);
    vm.tickDriver({}, kFrame);
    EXPECT_EQ(vm.readSlot(0), 0xABu);  // the byte read from bank 2 after the placed-code switch
}

// A banked placement with the (default) none mapper is a declaration error — a flat image is only 32 KiB.
TEST(DriverHostingVm, BankedPlacementWithoutMapperThrows) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 1> kData{0xAB};
    DriverBinding b;
    b.images = {image(kData, gb::banked(2, 0x4000))};  // banked base, but b.mapper left as none
    b.tickEntry = 0x0A00;
    EXPECT_THROW(vm.hostDriver(b), std::invalid_argument);
}

// The declared stack top relocates the scratch stack: a tick that CALLs a subroutine (using the stack)
// works against the relocated top, and the DEFAULT scratch top (0xDFFC) — set to a sentinel via a
// queued write before the tick — is left untouched, proving the relocation took effect.
TEST(DriverHostingVm, StackRelocationUsesDeclaredTopAndSparesDefault) {
    Vm vm{VMPlatform::GameBoyColor};
    // 0x6000: call 0x6007 ; ld [0xC000],a ; ret     0x6007: ld a,0x5A ; ret
    static constexpr std::array<std::uint8_t, 10> kTick{
        0xCD, 0x07, 0x60, 0xEA, 0x00, 0xC0, 0xC9, 0x3E, 0x5A, 0xC9};
    DriverBinding b;
    b.images = {image(kTick, 0x6000)};
    b.tickEntry = 0x6000;
    b.stackTop = 0xDF00;  // relocated, far below the 0xDFFC default
    b.slots = {SlotSpec{.address = 0xC000, .width = 1, .direction = SlotDirection::Read},
               SlotSpec{.address = 0xDFFC, .width = 1, .direction = SlotDirection::ReadWrite}};

    vm.hostDriver(b);
    // Mark the default scratch top with a sentinel; if the stack were NOT relocated, planting the
    // return sentinel there would overwrite it.
    const std::array<Instruction, 1> queued{
        Instruction::write(Location::memory(0xDFFC), 1, /*fixedValue=*/0x99)};
    vm.tickDriver(std::span<const Instruction>(queued), kFrame);

    EXPECT_EQ(vm.readSlot(0), 0x5Au);  // the CALL/RET on the relocated stack ran
    EXPECT_EQ(vm.readSlot(1), 0x99u);  // the default top was untouched — the stack really moved
}

// ── Placement validation ────────────────────────────────────────────────────────────────────────

TEST(DriverHostingVm, OverlappingImagesThrow) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 0x100> kA{};
    static constexpr std::array<std::uint8_t, 0x100> kB{};
    DriverBinding b;
    b.images = {image(kA, 0x6000), image(kB, 0x6080)};  // 0x6000+0x100 overlaps 0x6080
    b.tickEntry = 0x6000;
    EXPECT_THROW(vm.hostDriver(b), std::invalid_argument);
}

TEST(DriverHostingVm, LowWindowPlacementThrows) {
    static constexpr std::array<std::uint8_t, 4> kBytes{0x00, 0x00, 0x00, 0xC9};
    {
        Vm vm{VMPlatform::GameBoyColor};
        DriverBinding b;
        b.images = {image(kBytes, 0x0000)};  // the boot-ROM / interrupt-vector window
        b.tickEntry = 0x0000;
        EXPECT_THROW(vm.hostDriver(b), std::invalid_argument);
    }
    {
        Vm vm{VMPlatform::GameBoyColor};
        DriverBinding b;
        b.images = {image(kBytes, 0x0500)};  // still inside the CGB boot window (< 0x0900)
        b.tickEntry = 0x0500;
        EXPECT_THROW(vm.hostDriver(b), std::invalid_argument);
    }
}

// ── Cycle accounting ──────────────────────────────────────────────────────────────────────────

// The tick reports the CPU cycles its entry consumed (run-to-return in the cycle unit), and it never
// exceeds the frame budget it is asked to pad to. A longer entry costs more — the accounting tracks work.
TEST(DriverHostingVm, CycleAccountingScalesAndStaysUnderFrame) {
    auto tickCost = [](std::span<const std::uint8_t> body) {
        Vm vm{VMPlatform::GameBoyColor};
        DriverBinding b;
        b.images = {DriverImage{.bytes = body, .base = 0x6000}};
        b.tickEntry = 0x6000;
        vm.hostDriver(b);
        return vm.tickDriver({}, kFrame);
    };
    static constexpr std::array<std::uint8_t, 1> kShort{0xC9};                 // ret
    static constexpr std::array<std::uint8_t, 9> kLong{                        // 8×nop ; ret
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC9};

    const std::uint64_t shortCost = tickCost(std::span<const std::uint8_t>(kShort));
    const std::uint64_t longCost = tickCost(std::span<const std::uint8_t>(kLong));

    EXPECT_GT(shortCost, 0u);
    EXPECT_LT(longCost, kFrame);          // a small tick leaves headroom the idle pads
    EXPECT_GT(longCost, shortCost);       // the extra NOPs are accounted
}

// A tick entry that never returns is capped at the frame budget — the run-to-return-with-cycles guard
// terminates it rather than spinning forever.
TEST(DriverHostingVm, RunawayTickEntryIsCappedByFrameBudget) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 2> kSpin{0x18, 0xFE};  // jr $ — never returns
    DriverBinding b;
    b.images = {image(kSpin, 0x6000)};
    b.tickEntry = 0x6000;
    vm.hostDriver(b);

    const std::uint64_t budget = 1000;
    const std::uint64_t spent = vm.tickDriver({}, budget);  // returns (does not hang) — the guard fires
    EXPECT_GE(spent, budget);
    EXPECT_LT(spent, budget * 2);  // capped near the budget (a partial last instruction overshoots)
}

// ── Queued instructions (the RAM-flag + argument families) ────────────────────────────────────

// The RAM-flag family: a queued write lands a value in a mailbox the tick then reads and acts on.
TEST(DriverHostingVm, QueuedWriteFeedsMailboxThenTickActs) {
    Vm vm{VMPlatform::GameBoyColor};
    // ld a,[0xC010] ; add a ; ld [0xC000],a ; ret   (double the mailbox into the slot)
    static constexpr std::array<std::uint8_t, 8> kTick{
        0xFA, 0x10, 0xC0, 0x87, 0xEA, 0x00, 0xC0, 0xC9};
    DriverBinding b;
    b.images = {image(kTick, 0x6000)};
    b.tickEntry = 0x6000;
    b.slots = {SlotSpec{.address = 0xC000, .width = 1, .direction = SlotDirection::Read},
               SlotSpec{.address = 0xC010, .width = 1, .direction = SlotDirection::Write}};
    vm.hostDriver(b);

    const std::array<Instruction, 1> queued{
        Instruction::write(Location::memory(0xC010), 1, /*fixedValue=*/5)};
    vm.tickDriver(std::span<const Instruction>(queued), kFrame);
    EXPECT_EQ(vm.readSlot(0), 10u);  // the tick doubled the mailbox value the write delivered
}

// The argument family: a queued call rides a value in a CPU register into an entry the engine calls.
TEST(DriverHostingVm, QueuedCallRidesRegisterArgIntoEntry) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 4> kEntry{0xEA, 0x00, 0xC0, 0xC9};  // ld [0xC000],a ; ret
    static constexpr std::array<std::uint8_t, 1> kTickRet{0xC9};                  // ret (bare tick)
    DriverBinding b;
    b.images = {image(kEntry, 0x6000), image(kTickRet, 0x6010)};
    b.tickEntry = 0x6010;
    b.slots = {SlotSpec{.address = 0xC000, .width = 1, .direction = SlotDirection::Read}};
    vm.hostDriver(b);

    const std::array<Instruction, 1> queued{
        Instruction::call(0x6000, gb::A, /*fixedValue=*/0x2A)};
    vm.tickDriver(std::span<const Instruction>(queued), kFrame);
    EXPECT_EQ(vm.readSlot(0), 0x2Au);  // the entry stored the register the call rode the value in
}

// The declared .init gesture is performed ONCE by the engine at host() — before any tick runs.
TEST(DriverHostingVm, InitGestureRunsOnceAtHost) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 1> kTickRet{0xC9};
    DriverBinding b;
    b.images = {image(kTickRet, 0x6000)};
    b.tickEntry = 0x6000;
    b.slots = {SlotSpec{.address = 0xC000, .width = 1, .direction = SlotDirection::Read}};
    b.init = Instruction::write(Location::memory(0xC000), 1, /*fixedValue=*/0xC3);

    vm.hostDriver(b);
    EXPECT_EQ(vm.readSlot(0), 0xC3u);  // init already ran — no tick was driven
}

// ── The Instruction value rule + host preconditions ───────────────────────────────────────────

// A fixed-value gesture (stop()'s constant) ignores the performer's value; an unfixed one carries it.
TEST(DriverHostingVm, InstructionValueForUsesFixedThenPerformer) {
    const Instruction fixed = Instruction::write(Location::memory(0xC000), 1, /*fixedValue=*/7);
    const Instruction open = Instruction::write(Location::memory(0xC000), 1);
    EXPECT_EQ(fixed.valueFor(99), 7u);
    EXPECT_EQ(open.valueFor(99), 99u);
}

// Driving or reading slots before a driver is hosted is a usage error.
TEST(DriverHostingVm, TickAndReadSlotThrowWithoutHost) {
    Vm vm{VMPlatform::GameBoyColor};
    EXPECT_THROW(vm.tickDriver({}, kFrame), std::logic_error);
    EXPECT_THROW((void)vm.readSlot(0), std::logic_error);
}

// A hosted driver coexists with routines already uploaded on the same VM: hostDriver preserves the
// routine arena, so a previously-registered routine still runs after the resident image is configured.
TEST(DriverHostingVm, ArenaCoexistsWithHostedDriver) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 2> kAdd{0x80, 0xC9};  // add a,b ; ret
    auto add = vm.uploadRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
        std::span<const std::uint8_t>(kAdd), RoutineBinding{.inputs = {gb::A, gb::B}, .output = gb::A});
    EXPECT_EQ(add(3, 4), 7);  // works before hosting

    static constexpr std::array<std::uint8_t, 6> kTick{0x3E, 0x37, 0xEA, 0x00, 0xC0, 0xC9};
    DriverBinding b;
    b.images = {image(kTick, 0x6000)};
    b.tickEntry = 0x6000;
    b.slots = {SlotSpec{.address = 0xC000, .width = 1, .direction = SlotDirection::Read}};
    vm.hostDriver(b);

    EXPECT_EQ(add(5, 6), 11);  // the uploaded routine's bytes survived the image rebuild
    vm.tickDriver({}, kFrame);
    EXPECT_EQ(vm.readSlot(0), 0x37u);  // and the resident driver ticks
}

}  // namespace
}  // namespace retropp
