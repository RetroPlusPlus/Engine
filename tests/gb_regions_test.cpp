// The Game Boy's own memories, shipped as MemoryRegion constants.
//
// A console header supplies these exactly as it supplies gb::A for Location — the same value a game
// fills in for its own content, filled in here for the hardware. So the cases below check the two
// things a constant has to get right: that it resolves to the memory it names, and that its extent
// is that memory's whole extent and not a byte more.
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/gb.h"
#include "retropp/memory_region.h"
#include "retropp/vm.h"
#include "tests/authored_cartridge.h"

namespace retropp {
namespace {

using testing::authorCartridge;
using testing::kSmallestCartridge;

struct Places {
    MemoryRegion video;
    MemoryRegion work;
};

Vm::GBC hostedVm() {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));
    return vm;
}

TEST(GbRegions, EachMemoryReadsItsWholeExtentInOneGo) {
    Vm::GBC vm = hostedVm();

    EXPECT_EQ(vm.read(gb::VRam).size(), 0x2000u);
    EXPECT_EQ(vm.read(gb::WorkRam).size(), 0x2000u);
    EXPECT_EQ(vm.read(gb::Oam).size(), 0x00A0u);
    EXPECT_EQ(vm.read(gb::Io).size(), 0x0080u);
    EXPECT_EQ(vm.read(gb::Hram).size(), 0x007Fu);
}

TEST(GbRegions, AWholeMemoryIsOneEntry) {
    // count defaults to 1, so a whole memory is the degenerate array and read() needs no index.
    EXPECT_EQ(gb::VRam.count, 1u);
    EXPECT_EQ(gb::VRam.totalBytes(), 0x2000u);
    EXPECT_TRUE(gb::VRam.contains(0));
    EXPECT_FALSE(gb::VRam.contains(1));
}

TEST(GbRegions, WorkRamRoundTripsThroughTheConstant) {
    Vm::GBC vm = hostedVm();

    std::vector<std::uint8_t> whole = vm.read(gb::WorkRam);
    whole[0x0123] = 0x5A;
    vm.write(gb::WorkRam, whole);

    EXPECT_EQ(vm.read(gb::WorkRam)[0x0123], 0x5Au);
}

TEST(GbRegions, VideoRamRoundTripsThroughTheConstant) {
    Vm::GBC vm = hostedVm();

    std::vector<std::uint8_t> whole = vm.read(gb::VRam);
    whole[0x0010] = 0x3C;
    vm.write(gb::VRam, whole);

    EXPECT_EQ(vm.read(gb::VRam)[0x0010], 0x3Cu);
}

// A constant and a place named by address inside it must agree about where they are.
TEST(GbRegions, APlaceInsideAMemoryLandsWhereTheConstantSaysItDoes) {
    Vm::GBC vm = hostedVm();

    const MemoryRegion oneWord{.at = 0xC123, .size = 1};
    const std::vector<std::uint8_t> value{0xA7};
    vm.write(oneWord, value);

    EXPECT_EQ(vm.read(gb::WorkRam)[0x0123], 0xA7u);
}

TEST(GbRegions, AConstantCanBeDeclaredInABatchLikeAnyOtherPlace) {
    Vm::GBC vm = hostedVm();

    const auto places = vm.registerRegions(regions(
        region(&Places::video, gb::VRam, "video ram"),
        region(&Places::work, gb::WorkRam, "work ram")));

    EXPECT_EQ(places.size(), 2u);
    EXPECT_EQ(places.declared(&Places::video)->at, 0x8000u);
    EXPECT_EQ(vm.read(places, &Places::work).size(), 0x2000u);
}

TEST(GbRegions, AMemoryDoesNotDeclareASecondEntry) {
    Vm::GBC vm = hostedVm();

    EXPECT_THROW((void)vm.read(gb::Hram, 1), std::out_of_range);
}

// Each constant's extent stops at the end of its memory: one byte more does not resolve.
TEST(GbRegions, OneByteBeyondAMemoryIsRefused) {
    Vm::GBC vm = hostedVm();

    for (const MemoryRegion& m : {gb::VRam, gb::WorkRam, gb::Oam, gb::Io, gb::Hram}) {
        const MemoryRegion tooLong{.at = m.at, .size = m.size + 1};
        EXPECT_THROW((void)vm.registerRegions(regions(region(&Places::video, tooLong, "too long"))),
                     std::invalid_argument);
    }
}

// ── One answer about what memory exists ─────────────────────────────────────────────────────────
// A single byte is the one-byte case of a region, so the question a routine's memory binding asks
// and the question a declared place asks are the SAME question. These cases pin that: whatever a
// place may name, a binding may name, address for address. Two predicates drifting apart is how a
// surface ends up telling two callers different things about the same machine.

constexpr std::array<std::uint8_t, 1> kRet{0xC9};

bool aPlaceMayNameIt(Vm& vm, std::uint32_t address) {
    try {
        (void)vm.registerRegions(regions(
            region(&Places::video, MemoryRegion{.at = address, .size = 1}, "one byte")));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool aBindingMayNameIt(Vm& vm, std::uint32_t address) {
    try {
        (void)vm.uploadRoutine<void(std::uint8_t)>(
            std::span<const std::uint8_t>(kRet),
            RoutineBinding{.inputs = {Location::memory(address)}});
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

TEST(GbRegions, APlaceAndABindingAgreeAboutEveryAddress) {
    for (const std::uint32_t address : {std::uint32_t{0x0100},   // cartridge
                                        std::uint32_t{0x8000},   // video ram
                                        std::uint32_t{0x9FFF},   // video ram, last byte
                                        std::uint32_t{0xC000},   // work ram
                                        std::uint32_t{0xE000},   // echo ram: served by neither
                                        std::uint32_t{0xFE00},   // sprite attributes
                                        std::uint32_t{0xFEA0},   // unusable: served by neither
                                        std::uint32_t{0xFF00},   // hardware registers
                                        std::uint32_t{0xFF80}}) {  // high ram
        Vm::GBC placeVm;
        Vm::GBC bindingVm;
        EXPECT_EQ(aPlaceMayNameIt(placeVm, address), aBindingMayNameIt(bindingVm, address))
            << "the two answers disagree about address " << address;
    }
}

TEST(GbRegions, ABindingMayNameVideoRamBecauseAPlaceMay) {
    Vm::GBC vm;
    EXPECT_TRUE(aBindingMayNameIt(vm, 0x8000));
}

}  // namespace
}  // namespace retropp
