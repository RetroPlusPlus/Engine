#include <gtest/gtest.h>

#include "gbcpp/viewport.h"

namespace gbcpp {
namespace {

TEST(Viewport, DefaultsToGameBoyResolution) {
    constexpr ViewportConfig config{};
    EXPECT_EQ(config.width, 160);
    EXPECT_EQ(config.height, 144);
}

TEST(Viewport, OverrideIsReportedBack) {
    constexpr ViewportConfig wide{320, 144};
    EXPECT_EQ(wide.width, 320);
    EXPECT_EQ(wide.height, 144);
}

// The named presets are just {width, height} tuples for common platforms.

TEST(Viewport, PresetsHoldExpectedDimensions) {
    EXPECT_EQ(resolutions::GameBoy.width, 160);
    EXPECT_EQ(resolutions::GameBoy.height, 144);
    EXPECT_EQ(resolutions::GameBoyAdvance.width, 240);
    EXPECT_EQ(resolutions::GameBoyAdvance.height, 160);
    EXPECT_EQ(resolutions::Nes.width, 256);
    EXPECT_EQ(resolutions::Nes.height, 240);
    EXPECT_EQ(resolutions::Snes.width, 256);
    EXPECT_EQ(resolutions::Snes.height, 224);
    EXPECT_EQ(resolutions::Genesis.width, 320);
    EXPECT_EQ(resolutions::Genesis.height, 224);
    EXPECT_EQ(resolutions::MasterSystem.width, 256);
    EXPECT_EQ(resolutions::MasterSystem.height, 192);
}

TEST(Viewport, GameBoyAndColorAreSameResolutionDistinctNames) {
    EXPECT_EQ(resolutions::GameBoyColor.width, resolutions::GameBoy.width);
    EXPECT_EQ(resolutions::GameBoyColor.height, resolutions::GameBoy.height);
}

TEST(Viewport, PresetIsInterchangeableWithRawConfig) {
    // A preset is usable wherever a raw ViewportConfig is — same type, no conversion.
    constexpr ViewportConfig fromPreset = resolutions::Snes;
    constexpr ViewportConfig raw{256, 224};
    EXPECT_EQ(fromPreset.width, raw.width);
    EXPECT_EQ(fromPreset.height, raw.height);
}

TEST(Viewport, DefaultStillMatchesGameBoyPreset) {
    // The ViewportConfig{} default is unchanged — every existing call site keeps GB.
    constexpr ViewportConfig def{};
    EXPECT_EQ(def.width, resolutions::GameBoy.width);
    EXPECT_EQ(def.height, resolutions::GameBoy.height);
}

}  // namespace
}  // namespace gbcpp
