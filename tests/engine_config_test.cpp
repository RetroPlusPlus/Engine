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

    // Enhancements: faithful sampling/fullscreen baseline + the factory window scale.
    EXPECT_FALSE(cfg.enhancements.fullscreen);
    EXPECT_EQ(cfg.enhancements.windowScale, 4);  // window = 4× viewport (clamped to the display)
    EXPECT_EQ(cfg.enhancements.sampling, SamplingMode::Nearest);

    // Window: the default title (the size derives from windowScale × viewport, not WindowConfig).
    EXPECT_EQ(cfg.window.title, "GBCPP");
}

TEST(EngineConfig, EachFieldIsIndependentlyOverridable) {
    const EngineConfig cfg{
        .window       = {.title = "Demo"},
        .viewport     = ViewportResolution::Snes,
        .timing       = TimingProfile::GameBoy,
        .inputProfile = InputProfile::Snes,
        .enhancements = {.windowScale = 6, .fullscreen = true},
    };

    EXPECT_EQ(cfg.window.title, "Demo");

    EXPECT_EQ(cfg.viewport.width, 256);   // SNES internal resolution
    EXPECT_EQ(cfg.viewport.height, 224);

    EXPECT_EQ(cfg.timing, TimingProfile::GameBoy);

    EXPECT_EQ(cfg.inputProfile.buttons, InputProfile::Snes.buttons);
    EXPECT_TRUE(cfg.inputProfile.has(Button::X));  // SNES exposes the extra face buttons

    EXPECT_TRUE(cfg.enhancements.fullscreen);
    EXPECT_EQ(cfg.enhancements.windowScale, 6);
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

TEST(WindowConfig, DefaultHasTheConventionalTitle) {
    const WindowConfig w{};  // const, not constexpr — see the note above (std::string member)
    EXPECT_EQ(w.title, "GBCPP");  // size is no longer a WindowConfig field — see windowScale
}

TEST(EnhancementToggles, DefaultsAreFactory) {
    constexpr EnhancementToggles e{};
    EXPECT_EQ(e.windowScale, 4);   // factory window scale
    EXPECT_FALSE(e.fullscreen);
    EXPECT_EQ(e.sampling, SamplingMode::Nearest);
}

}  // namespace
}  // namespace gbcpp
