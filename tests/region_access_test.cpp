// Reading and writing the places a game declares in a machine.
//
// The case this file exists for is the bank-crossing indexed read. A realistic array in a cartridge
// — a few hundred entries of a couple of kilobytes — is far longer than the 0x4000 switchable
// window, so entry N is NOT at base + N * size in the address the base is written in: that
// arithmetic stops describing the run at the first boundary. Entries resolve in decoded physical
// space instead, and the planted pattern here proves it, because every entry carries its own index.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/gb.h"
#include "retropp/memory_region.h"
#include "retropp/vm.h"
#include "tests/authored_cartridge.h"

namespace retropp {
namespace {

using testing::authorCartridge;
using testing::kMbc3;
using testing::kSmallestCartridge;

struct Places {
    MemoryRegion array;
    MemoryRegion scratch;
    MemoryRegion absent;
};

// The long array the proof case reads: 251 entries of 0x800 bytes, based at the start of bank 1, so
// it runs about 917 KB through roughly 56 banks. Every byte of entry N is N, so a read that lands in
// the wrong bank returns the wrong number rather than merely different bytes.
constexpr std::uint32_t kEntrySize  = 0x800;
constexpr std::uint32_t kEntryCount = 251;
constexpr std::uint32_t kArrayBase  = 0x4000;  // physical: bank 1, offset 0

MemoryRegion arrayRegion() {
    return MemoryRegion{
        .at = gb::banked(1, 0x4000), .size = kEntrySize, .count = kEntryCount};
}

std::vector<std::uint8_t> cartridgeWithNumberedEntries() {
    std::vector<std::uint8_t> rom = authorCartridge(0x100000, kMbc3);  // 1 MiB
    for (std::uint32_t n = 0; n < kEntryCount; ++n) {
        const std::size_t base = kArrayBase + static_cast<std::size_t>(n) * kEntrySize;
        for (std::uint32_t k = 0; k < kEntrySize; ++k) {
            rom[base + k] = static_cast<std::uint8_t>(n & 0xFF);
        }
    }
    return rom;
}

TEST(RegionAccess, TheFirstEntryReadsFromTheDeclaredBase) {
    Vm::GBC vm;
    vm.hostRom(cartridgeWithNumberedEntries());
    const auto places =
        vm.registerRegions(regions(region(&Places::array, arrayRegion(), "long array")));

    const std::vector<std::uint8_t> entry = vm.read(places, &Places::array, 0);
    ASSERT_EQ(entry.size(), kEntrySize);
    EXPECT_EQ(entry.front(), 0u);
    EXPECT_EQ(entry.back(), 0u);
}

TEST(RegionAccess, AnEntryWithinTheFirstBankReads) {
    Vm::GBC vm;
    vm.hostRom(cartridgeWithNumberedEntries());
    const auto places =
        vm.registerRegions(regions(region(&Places::array, arrayRegion(), "long array")));

    const std::vector<std::uint8_t> entry = vm.read(places, &Places::array, 3);
    EXPECT_EQ(entry.front(), 3u);
    EXPECT_EQ(entry.back(), 3u);
}

// The proof. Entry 56 sits at physical 0x4000 + 56 * 0x800 = 0x20000 — bank 8 of the image, seven
// boundaries past the window the declared base names.
TEST(RegionAccess, AnEntryManyBanksPastTheBaseReadsItsOwnBytes) {
    Vm::GBC vm;
    vm.hostRom(cartridgeWithNumberedEntries());
    const auto places =
        vm.registerRegions(regions(region(&Places::array, arrayRegion(), "long array")));

    const std::vector<std::uint8_t> entry = vm.read(places, &Places::array, 56);
    ASSERT_EQ(entry.size(), kEntrySize);
    EXPECT_EQ(entry.front(), 56u);
    EXPECT_EQ(entry.back(), 56u);
}

TEST(RegionAccess, TheFirstEntryPastABankBoundaryReadsItsOwnBytes) {
    Vm::GBC vm;
    vm.hostRom(cartridgeWithNumberedEntries());
    const auto places =
        vm.registerRegions(regions(region(&Places::array, arrayRegion(), "long array")));

    // 0x4000 + 8 * 0x800 = 0x8000: the very first entry that leaves the base's own window.
    const std::vector<std::uint8_t> entry = vm.read(places, &Places::array, 8);
    EXPECT_EQ(entry.front(), 8u);
}

TEST(RegionAccess, TheLastDeclaredEntryReads) {
    Vm::GBC vm;
    vm.hostRom(cartridgeWithNumberedEntries());
    const auto places =
        vm.registerRegions(regions(region(&Places::array, arrayRegion(), "long array")));

    const std::vector<std::uint8_t> entry = vm.read(places, &Places::array, kEntryCount - 1);
    EXPECT_EQ(entry.front(), static_cast<std::uint8_t>((kEntryCount - 1) & 0xFF));
}

TEST(RegionAccess, AnIndexThePlaceDoesNotDeclareThrows) {
    Vm::GBC vm;
    vm.hostRom(cartridgeWithNumberedEntries());
    const auto places =
        vm.registerRegions(regions(region(&Places::array, arrayRegion(), "long array")));

    EXPECT_THROW((void)vm.read(places, &Places::array, kEntryCount), std::out_of_range);
}

TEST(RegionAccess, AFieldOutsideTheBatchThrows) {
    Vm::GBC vm;
    vm.hostRom(cartridgeWithNumberedEntries());
    const auto places =
        vm.registerRegions(regions(region(&Places::array, arrayRegion(), "long array")));

    EXPECT_THROW((void)vm.read(places, &Places::absent), std::invalid_argument);
}

TEST(RegionAccess, WritingAHostedCartridgePatchesTheImage) {
    Vm::GBC vm;
    vm.hostRom(cartridgeWithNumberedEntries());
    const auto places =
        vm.registerRegions(regions(region(&Places::array, arrayRegion(), "long array")));

    const std::vector<std::uint8_t> patch(kEntrySize, 0x7E);
    vm.write(places, &Places::array, patch, 56);

    EXPECT_EQ(vm.read(places, &Places::array, 56).front(), 0x7Eu);
}

TEST(RegionAccess, AWriteLeavesTheEntriesEitherSideAlone) {
    Vm::GBC vm;
    vm.hostRom(cartridgeWithNumberedEntries());
    const auto places =
        vm.registerRegions(regions(region(&Places::array, arrayRegion(), "long array")));

    const std::vector<std::uint8_t> patch(kEntrySize, 0x7E);
    vm.write(places, &Places::array, patch, 56);

    EXPECT_EQ(vm.read(places, &Places::array, 55).front(), 55u);
    EXPECT_EQ(vm.read(places, &Places::array, 57).front(), 57u);
    EXPECT_EQ(vm.read(places, &Places::array, 55).back(), 55u);
}

TEST(RegionAccess, AByteCountThatIsNotOneEntryThrows) {
    Vm::GBC vm;
    vm.hostRom(cartridgeWithNumberedEntries());
    const auto places =
        vm.registerRegions(regions(region(&Places::array, arrayRegion(), "long array")));

    const std::vector<std::uint8_t> tooFew(4, 0x00);
    EXPECT_THROW(vm.write(places, &Places::array, tooFew), std::invalid_argument);
}

TEST(RegionAccess, WorkRamRoundTrips) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));
    const auto places = vm.registerRegions(regions(region(
        &Places::scratch, MemoryRegion{.at = 0xC100, .size = 4, .count = 8}, "scratch")));

    const std::vector<std::uint8_t> value{0xDE, 0xAD, 0xBE, 0xEF};
    vm.write(places, &Places::scratch, value, 5);

    EXPECT_EQ(vm.read(places, &Places::scratch, 5), value);
    EXPECT_NE(vm.read(places, &Places::scratch, 4), value);
}

// A place built from bytes just read, rather than declared up front: the shape most real content
// takes, where a pointer table names variable-length blobs.
TEST(RegionAccess, APlaceBuiltAtRuntimeReadsWithoutBeingDeclared) {
    Vm::GBC vm;
    vm.hostRom(cartridgeWithNumberedEntries());

    const MemoryRegion built{.at = gb::banked(1, 0x4000), .size = kEntrySize, .count = kEntryCount};
    const std::vector<std::uint8_t> entry = vm.read(built, 56);

    EXPECT_EQ(entry.front(), 56u);
}

TEST(RegionAccess, ARuntimePlaceIsCheckedAtTheCall) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));

    const MemoryRegion built{.at = 0x1000, .size = 16, .count = 2};
    EXPECT_THROW((void)vm.read(built, 9), std::out_of_range);
}

}  // namespace
}  // namespace retropp
