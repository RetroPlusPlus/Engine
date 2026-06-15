#include <gtest/gtest.h>

#include "retropp/viewport.h"

namespace retropp {
namespace {

TEST(Viewport, DefaultsToGameBoyResolution) {
    constexpr ViewportResolution config{};
    EXPECT_EQ(config.width, 160);
    EXPECT_EQ(config.height, 144);
}

TEST(Viewport, OverrideIsReportedBack) {
    constexpr ViewportResolution wide{320, 144};
    EXPECT_EQ(wide.width, 320);
    EXPECT_EQ(wide.height, 144);
}

// The named presets are just {width, height} tuples for common platforms.

TEST(Viewport, PresetsHoldExpectedDimensions) {
    EXPECT_EQ(ViewportResolution::GameBoy.width, 160);
    EXPECT_EQ(ViewportResolution::GameBoy.height, 144);
    EXPECT_EQ(ViewportResolution::GameBoyAdvance.width, 240);
    EXPECT_EQ(ViewportResolution::GameBoyAdvance.height, 160);
    EXPECT_EQ(ViewportResolution::Nes.width, 256);
    EXPECT_EQ(ViewportResolution::Nes.height, 240);
    EXPECT_EQ(ViewportResolution::Snes.width, 256);
    EXPECT_EQ(ViewportResolution::Snes.height, 224);
    EXPECT_EQ(ViewportResolution::Genesis.width, 320);
    EXPECT_EQ(ViewportResolution::Genesis.height, 224);
    EXPECT_EQ(ViewportResolution::MasterSystem.width, 256);
    EXPECT_EQ(ViewportResolution::MasterSystem.height, 192);
}

TEST(Viewport, GameBoyAndColorAreSameResolutionDistinctNames) {
    EXPECT_EQ(ViewportResolution::GameBoyColor.width, ViewportResolution::GameBoy.width);
    EXPECT_EQ(ViewportResolution::GameBoyColor.height, ViewportResolution::GameBoy.height);
}

TEST(Viewport, PresetIsInterchangeableWithRawConfig) {
    // A preset is usable wherever a raw ViewportResolution is — same type, no conversion.
    constexpr ViewportResolution fromPreset = ViewportResolution::Snes;
    constexpr ViewportResolution raw{256, 224};
    EXPECT_EQ(fromPreset.width, raw.width);
    EXPECT_EQ(fromPreset.height, raw.height);
}

TEST(Viewport, DefaultStillMatchesGameBoyPreset) {
    // The ViewportResolution{} default is unchanged — every existing call site keeps GB.
    constexpr ViewportResolution def{};
    EXPECT_EQ(def.width, ViewportResolution::GameBoy.width);
    EXPECT_EQ(def.height, ViewportResolution::GameBoy.height);
}

}  // namespace
}  // namespace retropp
