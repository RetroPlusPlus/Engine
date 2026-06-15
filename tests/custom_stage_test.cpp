#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

#include "retropp/draw_state.h"

// ENG-2.C.3 — custom shader-stage hook. Device-free coverage of the CPU side: the pure helpers the
// renderer's registration + per-pass dispatch key off (uniformSizeIsValid, effectUsesCustomShader,
// customStagePassValid) and that a Custom effect flows through the same chain-build as the built-ins
// (activeFrameEffects). The live pipeline build + GPU passes are dev-verified across the three
// backends (the documented CI-headless boundary); these are the failable units.

namespace retropp {
namespace {

// A handle to the Nth registered stage (the renderer assigns ids 0,1,2,… in registration order).
constexpr PostProcessStageId stageId(std::uint32_t n) { return static_cast<PostProcessStageId>(n); }

// ── uniformSizeIsValid — the registration contract ────────────────────────────────────

TEST(UniformSizeIsValid, ZeroOrPositiveMultipleOf16) {
    EXPECT_TRUE(uniformSizeIsValid(0));    // no-uniform stage
    EXPECT_TRUE(uniformSizeIsValid(16));   // one register
    EXPECT_TRUE(uniformSizeIsValid(32));   // RippleUniforms (the demo)
    EXPECT_TRUE(uniformSizeIsValid(48));
    EXPECT_FALSE(uniformSizeIsValid(1));
    EXPECT_FALSE(uniformSizeIsValid(8));
    EXPECT_FALSE(uniformSizeIsValid(24));
    static_assert(uniformSizeIsValid(32));
    static_assert(!uniformSizeIsValid(24));
}

// ── effectUsesCustomShader — the renderer's kind-dispatch predicate ────────────────────

TEST(EffectUsesCustomShader, OnlyCustomKind) {
    EXPECT_FALSE(effectUsesCustomShader(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::None}));
    EXPECT_FALSE(effectUsesCustomShader(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement}));
    EXPECT_TRUE(effectUsesCustomShader(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom}));
    static_assert(effectUsesCustomShader(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom}));
    static_assert(!effectUsesCustomShader(ScreenSpaceEffect{}));
}

// ── customStagePassValid — the per-frame pass validation ───────────────────────────────

TEST(CustomStagePassValid, ValidWhenHandleInRangeAndUniformSizeMatches) {
    std::array<std::byte, 32> buf{};
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom,
                              .customShader = stageId(0),
                              .uniform = std::span<const std::byte>(buf)};
    // 1 stage registered (id 0 valid), declared uniform size 32 → matches the 32-byte span.
    EXPECT_TRUE(customStagePassValid(e, /*registeredStageCount=*/1, /*registeredUniformSize=*/32));
}

TEST(CustomStagePassValid, InvalidWhenHandleOutOfRange) {
    std::array<std::byte, 16> buf{};
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom,
                              .customShader = stageId(3),  // only 0..1 registered
                              .uniform = std::span<const std::byte>(buf)};
    EXPECT_FALSE(customStagePassValid(e, /*registeredStageCount=*/2, /*registeredUniformSize=*/16));
}

TEST(CustomStagePassValid, InvalidWhenUniformSizeMismatches) {
    std::array<std::byte, 16> buf{};  // 16 bytes, but the stage was registered with 32
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom,
                              .customShader = stageId(0),
                              .uniform = std::span<const std::byte>(buf)};
    EXPECT_FALSE(customStagePassValid(e, /*registeredStageCount=*/1, /*registeredUniformSize=*/32));
}

TEST(CustomStagePassValid, ZeroUniformStageTakesEmptySpan) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom, .customShader = stageId(0)};
    EXPECT_TRUE(e.uniform.empty());
    EXPECT_TRUE(customStagePassValid(e, /*registeredStageCount=*/1, /*registeredUniformSize=*/0));
}

TEST(CustomStagePassValid, FalseForNonCustomEffect) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement};
    EXPECT_FALSE(customStagePassValid(e, /*registeredStageCount=*/1, /*registeredUniformSize=*/0));
}

// ── A Custom effect flows through the same frame-level chain as the built-ins ──────────

// activeFrameEffects keeps a Custom entry (it is not None) and preserves submission order alongside a
// built-in — the chain dispatches on kind per pass, but the build is the same one C.2.a ships.
TEST(ActiveFrameEffects, KeepsCustomComposedWithBuiltinInOrder) {
    std::array<std::byte, 32> buf{};
    FrameDrawState frame;
    frame.postEffects = {
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom, .customShader = stageId(0),
                          .uniform = std::span<const std::byte>(buf)},
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::None},  // filtered
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 4.0f},
    };
    const auto active = activeFrameEffects(frame);
    ASSERT_EQ(active.size(), 2u);                                    // None dropped
    EXPECT_EQ(active[0].kind, ScreenSpaceEffectKind::Custom);        // order preserved: custom first
    EXPECT_EQ(active[1].kind, ScreenSpaceEffectKind::RowDisplacement);
}

// A Custom effect declared per-layer carries its scope (Layer/Below) like a built-in — the renderer
// routes scope identically regardless of kind (scope is a compositing decision, not a shader one).
TEST(EffectUsesCustomShader, PerLayerCustomKeepsScope) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom,
                              .scope = ScreenSpaceEffectScope::Below, .customShader = stageId(2)};
    EXPECT_TRUE(effectUsesCustomShader(e));
    EXPECT_TRUE(effectIsBelowScope(e));
}

}  // namespace
}  // namespace retropp
