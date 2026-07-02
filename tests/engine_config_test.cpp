#include <gtest/gtest.h>

#include "retropp/animation.h"      // AnimationPlayer::defaultTiming
#include "retropp/tween.h"          // TweenPlayer<T>::defaultTiming (float/Vec2/Vec3/Vec4)
#include "retropp/engine_config.h"
#include "retropp/renderer.h"       // Renderer::defaultViewport (read only — no GPU construction)
#include "retropp/run_loop.h"       // RunLoop::defaultTiming + the inherited-ctor check
#include "manual_clock.h"           // retropp::test::ManualClock

namespace retropp {
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
    EXPECT_EQ(cfg.window.title, "Retro++");

    // Evaluation grid: Viewport (crisp) — the analytic paths evaluate on the viewport grid by default.
    EXPECT_EQ(cfg.evaluationGrid, EvaluationGrid::Viewport);
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
    EXPECT_EQ(cfg.window.title, "Retro++");
}

TEST(WindowConfig, DefaultHasTheConventionalTitle) {
    const WindowConfig w{};  // const, not constexpr — see the note above (std::string member)
    EXPECT_EQ(w.title, "Retro++");  // size is no longer a WindowConfig field — see windowScale
}

TEST(EnhancementToggles, DefaultsAreFactory) {
    constexpr EnhancementToggles e{};
    EXPECT_EQ(e.windowScale, 4);   // factory window scale
    EXPECT_FALSE(e.fullscreen);
    EXPECT_EQ(e.sampling, SamplingMode::Nearest);
}

// ── EngineConfig::setActive fan-out ──────────────────────────────────────────────
// setActive() assigns the active config AND fans its fields into the per-type SDL-free static
// defaults so bare ctors inherit them. These statics are process-global, so the fixture restores
// the faithful Game Boy Color baseline after every case — case ordering can't leak state.
class EngineConfigActive : public ::testing::Test {
protected:
    void TearDown() override { EngineConfig::setActive(EngineConfig{}); }
};

TEST_F(EngineConfigActive, SetActiveFansOutToThePerTypeDefaults) {
    EngineConfig cfg{};
    cfg.timing                = TimingProfile{TickPeriodNs::Hz60};  // non-default cadence
    cfg.viewport              = ViewportResolution::Snes;           // non-default resolution
    cfg.enhancements.sampling = SamplingMode::Bilinear;            // non-default sampling
    cfg.evaluationGrid        = EvaluationGrid::Output;            // non-default evaluation grid
    EngineConfig::setActive(cfg);

    // The active config holds the assigned values.
    EXPECT_EQ(EngineConfig::active.timing, cfg.timing);
    EXPECT_EQ(EngineConfig::active.viewport.width, cfg.viewport.width);
    EXPECT_EQ(EngineConfig::active.viewport.height, cfg.viewport.height);

    // The per-type SDL-free defaults were fanned out (ViewportResolution has no operator==).
    EXPECT_EQ(RunLoop::defaultTiming, cfg.timing);
    EXPECT_EQ(Renderer::defaultViewport.width, cfg.viewport.width);
    EXPECT_EQ(Renderer::defaultViewport.height, cfg.viewport.height);
    EXPECT_EQ(Renderer::defaultSamplingMode, cfg.enhancements.sampling);  // sampling rides the fan-out
    EXPECT_EQ(Renderer::defaultEvaluationGrid, cfg.evaluationGrid);       // evaluation grid rides it too
    EXPECT_EQ(AnimationPlayer::defaultTiming, cfg.timing);
    // Every interpolable tween type's cadence is fanned out too (per-T template static).
    EXPECT_EQ(TweenPlayer<float>::defaultTiming, cfg.timing);
    EXPECT_EQ(TweenPlayer<Vec2>::defaultTiming, cfg.timing);
    EXPECT_EQ(TweenPlayer<Vec3>::defaultTiming, cfg.timing);
    EXPECT_EQ(TweenPlayer<Vec4>::defaultTiming, cfg.timing);
}

TEST_F(EngineConfigActive, BareRunLoopInheritsTheFannedTimingAndExplicitOverrideStillWins) {
    EngineConfig cfg{};
    cfg.timing = TimingProfile{TickPeriodNs::Hz60};
    EngineConfig::setActive(cfg);

    test::ManualClock clock;
    // Inherited: a bare RunLoop picks up the fanned default through its ctor default argument.
    RunLoop inherited{clock};
    EXPECT_EQ(inherited.timing(), cfg.timing);

    // Override: an explicitly-passed profile still wins over the default.
    RunLoop overridden{clock, TimingProfile::GameBoyColor};
    EXPECT_EQ(overridden.timing(), TimingProfile::GameBoyColor);
}

TEST_F(EngineConfigActive, FaithfulDefaultIsPreservedByADefaultConfig) {
    // A default config fans the GBC baseline back into every per-type default (the byte-unchanged
    // baseline) — proving setActive with EngineConfig{} is a no-op against the faithful defaults.
    EngineConfig::setActive(EngineConfig{});

    EXPECT_EQ(RunLoop::defaultTiming, TimingProfile::GameBoyColor);
    EXPECT_EQ(Renderer::defaultViewport.width, ViewportResolution::GameBoyColor.width);
    EXPECT_EQ(Renderer::defaultViewport.height, ViewportResolution::GameBoyColor.height);
    EXPECT_EQ(Renderer::defaultSamplingMode, SamplingMode::Nearest);  // faithful crisp-pixel default
    EXPECT_EQ(Renderer::defaultEvaluationGrid, EvaluationGrid::Viewport);  // crisp evaluation default
    EXPECT_EQ(AnimationPlayer::defaultTiming, TimingProfile::GameBoyColor);
    EXPECT_EQ(TweenPlayer<float>::defaultTiming, TimingProfile::GameBoyColor);
    EXPECT_EQ(TweenPlayer<Vec3>::defaultTiming, TimingProfile::GameBoyColor);
}

}  // namespace
}  // namespace retropp
