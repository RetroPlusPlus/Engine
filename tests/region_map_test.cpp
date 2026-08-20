// Declaring the places in a machine a game cares about, as one batch, and having every entry checked
// when the batch is registered.
//
// The point of handing the batch over is that it is answered once: a table generated from a symbol
// file can be hundreds of entries, and a report that stops at the first mistake turns one
// registration into as many rounds as there are mistakes. So the cases here pin that a bad batch
// names EVERY bad entry, each by the name it was declared with.
#include <cstdint>
#include <string>

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

// The vocabulary a game declares its places against. Never instantiated — it exists so the places
// have names a pointer-to-member can key on.
struct Places {
    MemoryRegion tileArt;
    MemoryRegion textTable;
    MemoryRegion elsewhere;
};

// Catch a throw and hand back its message, so a case can assert on what the report actually says.
template <class Fn>
std::string messageFrom(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

TEST(RegionMap, AValidBatchRoundTripsItsDeclarations) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));

    const auto places = vm.registerRegions(regions(
        region(&Places::tileArt, MemoryRegion{.at = 0x1000, .size = 16, .count = 64}, "tile art"),
        region(&Places::textTable, MemoryRegion{.at = 0x3000, .size = 32, .count = 8}, "text table")));

    EXPECT_EQ(places.size(), 2u);
    ASSERT_TRUE(places.declared(&Places::tileArt).has_value());
    EXPECT_EQ(places.declared(&Places::tileArt)->at, 0x1000u);
    EXPECT_EQ(places.declared(&Places::tileArt)->count, 64u);
    EXPECT_EQ(places.declared(&Places::textTable)->size, 32u);
}

TEST(RegionMap, AKeyOutsideTheBatchNamesNothing) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));

    const auto places = vm.registerRegions(regions(
        region(&Places::tileArt, MemoryRegion{.at = 0x1000, .size = 16, .count = 64}, "tile art")));

    EXPECT_FALSE(places.declared(&Places::elsewhere).has_value());
}

TEST(RegionMap, ACountOfOneIsTheDefault) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));

    const auto places = vm.registerRegions(regions(
        region(&Places::tileArt, MemoryRegion{.at = 0x1000, .size = 256}, "one blob")));

    EXPECT_EQ(places.declared(&Places::tileArt)->count, 1u);
    EXPECT_EQ(places.declared(&Places::tileArt)->totalBytes(), 256u);
}

TEST(RegionMap, AnEmptyBatchIsRefused) {
    Vm::GBC vm;
    EXPECT_THROW((void)vm.registerRegions(RegionMap<Places>{}), std::invalid_argument);
}

TEST(RegionMap, AnUnreachableAddressIsRefused) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));

    // 0xE000-0xFDFF is echo RAM: not a memory this backend serves directly.
    EXPECT_THROW((void)vm.registerRegions(regions(region(
                     &Places::tileArt, MemoryRegion{.at = 0xE000, .size = 16}, "echo"))),
                 std::invalid_argument);
}

TEST(RegionMap, APlaceRunningPastTheEndOfItsMemoryIsRefused) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));

    // High RAM is 0xFF80-0xFFFE; a 200-byte place starting inside it runs off the end.
    EXPECT_THROW((void)vm.registerRegions(regions(region(
                     &Places::tileArt, MemoryRegion{.at = 0xFF80, .size = 200}, "overruns hram"))),
                 std::invalid_argument);
}

TEST(RegionMap, APlaceRunningPastTheEndOfTheCartridgeIsRefused) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));  // 32 KiB

    EXPECT_THROW((void)vm.registerRegions(regions(region(
                     &Places::tileArt, MemoryRegion{.at = 0x7000, .size = 0x2000}, "off the end"))),
                 std::invalid_argument);
}

TEST(RegionMap, ABadBatchNamesEveryBadEntryByItsDeclaredName) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));

    const std::string report = messageFrom([&] {
        (void)vm.registerRegions(regions(
            region(&Places::tileArt, MemoryRegion{.at = 0xE000, .size = 16}, "tile art"),
            region(&Places::textTable, MemoryRegion{.at = 0x1000, .size = 16}, "text table"),
            region(&Places::elsewhere, MemoryRegion{.at = 0xFEA0, .size = 16}, "elsewhere")));
    });

    ASSERT_FALSE(report.empty());
    EXPECT_NE(report.find("tile art"), std::string::npos);
    EXPECT_NE(report.find("elsewhere"), std::string::npos);
    // The one good entry is not reported as a failure, and the count reflects only the bad ones.
    EXPECT_NE(report.find("2 of 3"), std::string::npos);
}

TEST(RegionMap, ABankQualifiedPlaceInsideTheCartridgeIsAccepted) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(0x100000, kMbc3));  // 1 MiB, 64 banks

    // Bank 2 through the switchable window: physical 0x8000, well inside a 1 MiB image.
    const auto places = vm.registerRegions(regions(region(
        &Places::tileArt, MemoryRegion{.at = gb::banked(2, 0x4000), .size = 16, .count = 384},
        "banked tile art")));

    EXPECT_EQ(places.size(), 1u);
}

TEST(RegionMap, APlaceLongerThanOneBankIsAcceptedWhenTheCartridgeHoldsIt) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(0x100000, kMbc3));  // 1 MiB

    // 251 x 0x800 is about 917 KB — roughly 56 banks. It is a place, not a window, so it is
    // measured against the whole cartridge rather than against the 0x4000 window it starts in.
    const auto places = vm.registerRegions(regions(region(
        &Places::tileArt, MemoryRegion{.at = gb::banked(1, 0x4000), .size = 0x800, .count = 251},
        "long array")));

    EXPECT_EQ(places.size(), 1u);
}

TEST(RegionMap, ABankQualifiedPlacePastTheEndOfTheCartridgeIsRefused) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge, kMbc3));  // 32 KiB: banks 0 and 1 only

    EXPECT_THROW((void)vm.registerRegions(regions(region(
                     &Places::tileArt, MemoryRegion{.at = gb::banked(40, 0x4000), .size = 16},
                     "bank 40"))),
                 std::invalid_argument);
}

TEST(RegionMap, ABankQualifiedAddressOutsideTheSwitchableWindowIsRefused) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(0x100000, kMbc3));

    // A bank names a byte through 0x4000-0x7FFF; qualifying a fixed-bank address is meaningless.
    EXPECT_THROW((void)vm.registerRegions(regions(region(
                     &Places::tileArt, MemoryRegion{.at = gb::banked(2, 0x1000), .size = 16},
                     "fixed-bank address with a bank"))),
                 std::invalid_argument);
}

TEST(RegionMap, APlaceSpanningNoBytesIsRefused) {
    Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));

    EXPECT_THROW((void)vm.registerRegions(regions(region(
                     &Places::tileArt, MemoryRegion{.at = 0x1000, .size = 0}, "empty"))),
                 std::invalid_argument);
}

}  // namespace
}  // namespace retropp
