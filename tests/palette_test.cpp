#include "retropp/palette.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace retropp {

// Rgba8 is the engine's final output colour: four named channels, opaque by default, 4 bytes
// so it uploads as a tightly-packed RGBA8 texture row with no padding.

TEST(Palette, Rgba8IsFourTightlyPackedBytes) {
    static_assert(sizeof(Rgba8) == 4, "Rgba8 must upload as packed RGBA8");
    EXPECT_EQ(sizeof(Rgba8), 4u);
}

TEST(Palette, Rgba8DefaultsToOpaqueBlack) {
    constexpr Rgba8 c{};
    EXPECT_EQ(c.r, 0);
    EXPECT_EQ(c.g, 0);
    EXPECT_EQ(c.b, 0);
    EXPECT_EQ(c.a, 255);  // opaque default — never accidentally transparent
}

TEST(Palette, Rgba8NamedFieldsAndEquality) {
    constexpr Rgba8 a{10, 20, 30, 40};
    constexpr Rgba8 b{10, 20, 30, 40};
    constexpr Rgba8 c{10, 20, 30, 41};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(a.r, 10);
    EXPECT_EQ(a.g, 20);
    EXPECT_EQ(a.b, 30);
    EXPECT_EQ(a.a, 40);
}

// PaletteSize presets are count mnemonics: the enumerator VALUE is the entry count, so a caller
// can pass a preset or a raw integer interchangeably.

TEST(Palette, SizePresetValuesEqualEntryCounts) {
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::GameBoy), 4u);
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::GameBoyColor), 4u);
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::Nes), 4u);
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::MasterSystem), 16u);
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::Genesis), 16u);
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::Snes), 16u);
}

TEST(Palette, SizeRoundTripsRawInteger) {
    // A raw entry count constructs the matching preset value and back — the interchangeability.
    constexpr auto fromRaw = static_cast<PaletteSize>(4u);
    EXPECT_EQ(fromRaw, PaletteSize::GameBoy);
    EXPECT_EQ(static_cast<std::uint32_t>(static_cast<PaletteSize>(16u)), 16u);
}

}  // namespace retropp
