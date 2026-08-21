#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "retropp/draw_state.h"
#include "retropp/generated/custom_effect_packers.h"  // pack_effect_probe_frag (reflected from the probe)

// The custom shader-stage hook. Device-free coverage of the CPU side: the pure
// predicates the renderer's dispatch keys off (effectUsesCustomShader, customStagePassValid), that a
// Custom effect flows through the same chain-build as the built-ins (activeFrameEffects), and that the
// build reflects a custom shader's OWN cbuffer into ScreenSpaceEffect's inline param fields + a packer
// that writes them at the right offsets (the I.b mechanism). The live pipeline build + GPU passes are
// dev-verified across the three backends (the documented CI-headless boundary).
//
// tests/shaders/effect_probe.frag.hlsl is compiled + reflected for this target via an explicit EXTRA-path
// declaration in CMakeLists.txt (retropp_autocompile_shaders ... tests/shaders/effect_probe.frag.hlsl).
// This is a device-free test that drives the generated pack_effect_probe_frag DIRECTLY — there is no
// registerPostProcessStage(...) call here for the call-keyed scan to discover, so the build is told about
// the shader explicitly. That gives us pack_effect_probe_frag + the reflected .offset/.strength fields.

namespace retropp {
namespace {

// A handle to the Nth registered stage (the renderer assigns ids 0,1,2,… in registration order).
constexpr PostProcessStageId stageId(std::uint32_t n) { return static_cast<PostProcessStageId>(n); }

// ── effectUsesCustomShader — the renderer's kind-dispatch predicate ────────────────────

TEST(EffectUsesCustomShader, OnlyCustomKind) {
    EXPECT_FALSE(effectUsesCustomShader(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::None}));
    EXPECT_FALSE(effectUsesCustomShader(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement}));
    EXPECT_TRUE(effectUsesCustomShader(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom}));
    static_assert(effectUsesCustomShader(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom}));
    static_assert(!effectUsesCustomShader(ScreenSpaceEffect{}));
}

// ── customStagePassValid — the per-frame pass validation (handle in range) ─────────────

TEST(CustomStagePassValid, ValidWhenHandleInRange) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom, .customShader = stageId(0)};
    EXPECT_TRUE(customStagePassValid(e, /*registeredStageCount=*/1));
}

TEST(CustomStagePassValid, InvalidWhenHandleOutOfRange) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom,
                              .customShader = stageId(3)};  // only 0..1 registered
    EXPECT_FALSE(customStagePassValid(e, /*registeredStageCount=*/2));
}

TEST(CustomStagePassValid, FalseForNonCustomEffect) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement};
    EXPECT_FALSE(customStagePassValid(e, /*registeredStageCount=*/1));
}

// ── Reflected params + packer (the reflection mechanism) ────────────────────────────────

// The probe shader's `cbuffer Params { float2 offset; float strength; }` is reflected into inline fields
// on ScreenSpaceEffect, and its generated packer writes them at the HLSL cbuffer offsets (offset @0..7,
// strength @8..11, rounded to a 16-byte register). The renderer never reads the fields — it calls this.
TEST(CustomEffectPacker, ReflectsAndPacksShaderOwnParams) {
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom, .customShader = stageId(0)};
    e.offset   = Vec2{1.5f, -2.0f};  // the field exists ⇒ the cbuffer was reflected by NAME
    e.strength = 3.25f;

    std::array<std::byte, 64> buf{};
    buf.fill(std::byte{0xAB});  // poison: confirm the packer zero-fills then writes
    const std::uint32_t size = shaders::pack_effect_probe_frag(e, buf.data());

    EXPECT_EQ(size, 16u);  // 8 (float2) + 4 (float) → rounded to one 16-byte register
    float vals[4]{};
    std::memcpy(vals, buf.data(), sizeof(vals));
    EXPECT_FLOAT_EQ(vals[0], 1.5f);   // offset.x  @0
    EXPECT_FLOAT_EQ(vals[1], -2.0f);  // offset.y  @4
    EXPECT_FLOAT_EQ(vals[2], 3.25f);  // strength  @8
    EXPECT_FLOAT_EQ(vals[3], 0.0f);   // padding   @12 (zero-filled, not poison)
}

// ── A Custom effect flows through the same frame-level chain as the built-ins ──────────

// activeFrameEffects keeps a Custom entry (it is not None) and preserves submission order alongside a
// built-in — the chain dispatches on kind per pass, but the build is the same one C.2.a ships.
TEST(ActiveFrameEffects, KeepsCustomComposedWithBuiltinInOrder) {
    FrameDrawState frame;
    frame.postEffects = {
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom, .customShader = stageId(0)},
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
                              .customShader = stageId(2), .scope = ScreenSpaceEffectScope::Below};
    EXPECT_TRUE(effectUsesCustomShader(e));
    EXPECT_TRUE(effectIsBelowScope(e));
}

}  // namespace
}  // namespace retropp
