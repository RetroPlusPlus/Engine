#include <gtest/gtest.h>

#include "gbcpp/engine_config.h"

namespace gbcpp {
namespace {

// EngineConfig is the single startup bundle. Its defining property: a default-constructed
// config reproduces the faithful Game Boy Color baseline, and every field is independently
// overridable via designated initializers. All checks are headless value-type comparisons.

TEST(EngineConfig, DefaultReproducesTheFaithfulGameBoyColorBaseline) {
    // const, not constexpr: WindowConfig holds a std::string, so a constexpr EngineConfig is
    // not portable (libstdc++ rejects a constexpr object with a std::string member). The
    // defaults are runtime values; that is all this asserts.
    const EngineConfig cfg{};

    // Internal viewport: the original 160×144 (ViewportResolution has no operator==, so compare
    // fields against the GameBoyColor preset).
    EXPECT_EQ(cfg.viewport.width, ViewportResolution::GameBoyColor.width);
    EXPECT_EQ(cfg.viewport.height, ViewportResolution::GameBoyColor.height);
    EXPECT_EQ(cfg.viewport.width, 160);
    EXPECT_EQ(cfg.viewport.height, 144);

    // Render timing: the GBC cadence (TimingProfile has a defaulted operator==).
    EXPECT_EQ(cfg.timing, TimingProfile::GameBoyColor);

    // Active controller profile: Game Boy (compare the logical button mask + name).
    EXPECT_EQ(cfg.inputProfile.buttons, InputProfile::GameBoy.buttons);
    EXPECT_EQ(cfg.inputProfile.name, "Game Boy");
    EXPECT_FALSE(cfg.inputProfile.has(Button::X));  // not the SNES set

    // Enhancements: every toggle at the faithful (OFF / identity) baseline.
    EXPECT_FALSE(cfg.enhancements.fullscreen);
    EXPECT_EQ(cfg.enhancements.integerScale, 0);

    // Window: the default title + size.
    EXPECT_EQ(cfg.window.title, "GBCPP");
    EXPECT_EQ(cfg.window.width, 160 * 4);
    EXPECT_EQ(cfg.window.height, 144 * 4);
}

TEST(EngineConfig, EachFieldIsIndependentlyOverridable) {
    const EngineConfig cfg{
        .window       = {.title = "Demo", .width = 800, .height = 600},
        .viewport     = ViewportResolution::Snes,
        .timing       = TimingProfile::GameBoy,
        .inputProfile = InputProfile::Snes,
        .enhancements = {.fullscreen = true, .integerScale = 3},
    };

    EXPECT_EQ(cfg.window.title, "Demo");
    EXPECT_EQ(cfg.window.width, 800);
    EXPECT_EQ(cfg.window.height, 600);

    EXPECT_EQ(cfg.viewport.width, 256);   // SNES internal resolution
    EXPECT_EQ(cfg.viewport.height, 224);

    EXPECT_EQ(cfg.timing, TimingProfile::GameBoy);

    EXPECT_EQ(cfg.inputProfile.buttons, InputProfile::Snes.buttons);
    EXPECT_TRUE(cfg.inputProfile.has(Button::X));  // SNES exposes the extra face buttons

    EXPECT_TRUE(cfg.enhancements.fullscreen);
    EXPECT_EQ(cfg.enhancements.integerScale, 3);
}

TEST(EngineConfig, PartialOverrideLeavesOtherFieldsAtTheBaseline) {
    // Overriding only the input profile must not disturb viewport / timing / enhancements.
    const EngineConfig cfg{.inputProfile = InputProfile::Snes};

    EXPECT_EQ(cfg.inputProfile.buttons, InputProfile::Snes.buttons);
    EXPECT_EQ(cfg.viewport.width, 160);
    EXPECT_EQ(cfg.timing, TimingProfile::GameBoyColor);
    EXPECT_FALSE(cfg.enhancements.fullscreen);
    EXPECT_EQ(cfg.window.title, "GBCPP");
}

TEST(WindowConfig, DefaultsAreTheConventionalWindowedSize) {
    const WindowConfig w{};  // const, not constexpr — see the note above (std::string member)
    EXPECT_EQ(w.title, "GBCPP");
    EXPECT_EQ(w.width, 640);
    EXPECT_EQ(w.height, 576);
}

TEST(EnhancementToggles, DefaultsAreAllFaithful) {
    constexpr EnhancementToggles e{};
    EXPECT_FALSE(e.fullscreen);
    EXPECT_EQ(e.integerScale, 0);
}

}  // namespace
}  // namespace gbcpp
