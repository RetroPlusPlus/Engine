#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/draw_state.h"

// The built-in colour-fill effect. Device-free coverage of the CPU side: the uniform resolution
// (colorFillParams — field copy + fillStrength clamp) and the colour transform the colorfill.frag GPU
// stage mirrors (applyColorFill: clamp(in*mul + add) then mix(in, fill, fillStrength)). Unlike the sin-
// based displace/ripple mirrors, this transform is pure arithmetic — genuinely constexpr — so the WHOLE
// math is static_assert-testable, not just a normalization. The live GPU path is build-compiled +
// dev-verified across all three backends (the documented CI-headless boundary).

namespace retropp {
namespace {

// colorFillParams's clamp is constexpr. It cannot be asserted over a `constexpr ScreenSpaceEffect`
// instance (the EngineConfig precedent — a constexpr object with a heap-owning member is rejected by
// libstdc++); a constexpr wrapper FUNCTION builds the effect as a LOCAL temporary (constructed +
// destroyed within the constant evaluation), which is well-formed, and exposes the resolved params.
constexpr ColorFillParams colorFillParamsOf(float mulR, float mulG, float mulB,
                                            float addR, float addG, float addB,
                                            float fillR, float fillG, float fillB,
                                            float fillStrength) {
    ScreenSpaceEffect e{};
    e.kind = ScreenSpaceEffectKind::ColorFill;
    e.mulR = mulR; e.mulG = mulG; e.mulB = mulB;
    e.addR = addR; e.addG = addG; e.addB = addB;
    e.fillR = fillR; e.fillG = fillG; e.fillB = fillB;
    e.fillStrength = fillStrength;
    return colorFillParams(e);
}

// ── colorFillParams — field copy + fillStrength clamp ──────────────────────────────────

// A default ColorFill effect (mul 1, add 0, fill 0, strength 0) resolves to the identity params — a
// pass-through, so a frame carrying it changes nothing.
TEST(ColorFillParams, DefaultEffectIsIdentity) {
    const ColorFillParams p = colorFillParamsOf(1, 1, 1, 0, 0, 0, 0, 0, 0, 0);
    EXPECT_TRUE(p == ColorFillParams{});
    static_assert(colorFillParamsOf(1, 1, 1, 0, 0, 0, 0, 0, 0, 0) == ColorFillParams{},
                  "an identity ColorFill effect resolves to identity params");
}

// fillStrength is clamped to [0,1] in the resolver (the authoritative mirror — the GPU gets a valid mix
// amount); the other fields copy through verbatim.
TEST(ColorFillParams, ClampsFillStrength) {
    EXPECT_FLOAT_EQ(colorFillParamsOf(1, 1, 1, 0, 0, 0, 1, 1, 1, -0.5f).fillStrength, 0.0f);
    EXPECT_FLOAT_EQ(colorFillParamsOf(1, 1, 1, 0, 0, 0, 1, 1, 1, 1.5f).fillStrength, 1.0f);
    EXPECT_FLOAT_EQ(colorFillParamsOf(1, 1, 1, 0, 0, 0, 1, 1, 1, 0.3f).fillStrength, 0.3f);
    static_assert(colorFillParamsOf(1, 1, 1, 0, 0, 0, 0, 0, 0, 2.0f).fillStrength == 1.0f,
                  "fillStrength clamps above 1");
    static_assert(colorFillParamsOf(1, 1, 1, 0, 0, 0, 0, 0, 0, -1.0f).fillStrength == 0.0f,
                  "fillStrength clamps below 0");
}

// The colour fields (mul / add / fill) are carried through verbatim.
TEST(ColorFillParams, CopiesColourFields) {
    const ColorFillParams p =
        colorFillParamsOf(0.5f, 0.6f, 0.7f, 0.1f, 0.2f, 0.3f, 0.9f, 0.8f, 0.7f, 0.4f);
    EXPECT_FLOAT_EQ(p.mulR, 0.5f);  EXPECT_FLOAT_EQ(p.mulG, 0.6f);  EXPECT_FLOAT_EQ(p.mulB, 0.7f);
    EXPECT_FLOAT_EQ(p.addR, 0.1f);  EXPECT_FLOAT_EQ(p.addG, 0.2f);  EXPECT_FLOAT_EQ(p.addB, 0.3f);
    EXPECT_FLOAT_EQ(p.fillR, 0.9f); EXPECT_FLOAT_EQ(p.fillG, 0.8f); EXPECT_FLOAT_EQ(p.fillB, 0.7f);
    EXPECT_FLOAT_EQ(p.fillStrength, 0.4f);
}

// ColorFillParams equality is constexpr (a plain struct, no heap — unlike its parent effect).
static_assert(ColorFillParams{} == ColorFillParams{}, "default ColorFillParams compare equal");
static_assert(!(ColorFillParams{.mulR = 0.5f} == ColorFillParams{}), "a differing field compares unequal");

// ── applyColorFill — the full colour transform mirror ──────────────────────────────────

// Identity params (mul 1, add 0, strength 0) leave the pixel exactly unchanged: mul/add is a no-op and
// the mix at strength 0 returns the input — exactly (the basis of byte-identical faithful output).
TEST(ApplyColorFill, IdentityLeavesInputUnchanged) {
    const ColorFillRgb in{0.3f, 0.6f, 0.9f};
    EXPECT_EQ(applyColorFill(in, ColorFillParams{}), in);
    static_assert(applyColorFill(ColorFillRgb{0.3f, 0.6f, 0.9f}, ColorFillParams{}) ==
                      ColorFillRgb{0.3f, 0.6f, 0.9f},
                  "identity ColorFill is a pass-through");
}

// A multiply < 1 darkens each channel.
TEST(ApplyColorFill, MultiplyDarkens) {
    constexpr ColorFillParams p{.mulR = 0.5f, .mulG = 0.5f, .mulB = 0.5f};
    const ColorFillRgb out = applyColorFill(ColorFillRgb{0.8f, 0.8f, 0.8f}, p);
    EXPECT_FLOAT_EQ(out.r, 0.4f);
    EXPECT_FLOAT_EQ(out.g, 0.4f);
    EXPECT_FLOAT_EQ(out.b, 0.4f);
}

// A positive add lifts each channel (mul defaults to 1).
TEST(ApplyColorFill, AddLifts) {
    constexpr ColorFillParams p{.addR = 0.2f, .addG = 0.2f, .addB = 0.2f};
    const ColorFillRgb out = applyColorFill(ColorFillRgb{0.5f, 0.5f, 0.5f}, p);
    EXPECT_FLOAT_EQ(out.r, 0.7f);
}

// mul*in + add overflowing [0,1] clamps to 1 (the clamp matches the shader, applied after mul+add).
TEST(ApplyColorFill, MulAddOverflowClampsToOne) {
    constexpr ColorFillParams p{.mulR = 2.0f, .mulG = 2.0f, .mulB = 2.0f,
                                .addR = 0.5f, .addG = 0.5f, .addB = 0.5f};
    const ColorFillRgb out = applyColorFill(ColorFillRgb{0.8f, 0.8f, 0.8f}, p);  // 0.8·2 + 0.5 = 2.1 → 1
    EXPECT_FLOAT_EQ(out.r, 1.0f);
    static_assert(applyColorFill(ColorFillRgb{0.8f, 0.8f, 0.8f},
                                 ColorFillParams{.mulR = 2.0f, .mulG = 2.0f, .mulB = 2.0f,
                                                 .addR = 0.5f, .addG = 0.5f, .addB = 0.5f})
                          .r == 1.0f,
                  "mul+add overflow clamps to 1");
}

// A negative result clamps to 0.
TEST(ApplyColorFill, NegativeClampsToZero) {
    constexpr ColorFillParams p{.addR = -0.5f, .addG = -0.5f, .addB = -0.5f};
    const ColorFillRgb out = applyColorFill(ColorFillRgb{0.2f, 0.2f, 0.2f}, p);  // 0.2 − 0.5 = −0.3 → 0
    EXPECT_FLOAT_EQ(out.r, 0.0f);
}

// fillStrength 1 → exactly `fill`, regardless of the input pixel (solid fill: the line/shape colour).
TEST(ApplyColorFill, FullStrengthIsSolidFill) {
    constexpr ColorFillParams p{.fillR = 0.9f, .fillG = 0.1f, .fillB = 0.4f, .fillStrength = 1.0f};
    const ColorFillRgb fromBlack = applyColorFill(ColorFillRgb{0.0f, 0.0f, 0.0f}, p);
    const ColorFillRgb fromWhite = applyColorFill(ColorFillRgb{1.0f, 1.0f, 1.0f}, p);
    EXPECT_FLOAT_EQ(fromBlack.r, 0.9f); EXPECT_FLOAT_EQ(fromBlack.g, 0.1f); EXPECT_FLOAT_EQ(fromBlack.b, 0.4f);
    EXPECT_FLOAT_EQ(fromWhite.r, 0.9f); EXPECT_FLOAT_EQ(fromWhite.g, 0.1f); EXPECT_FLOAT_EQ(fromWhite.b, 0.4f);
    static_assert(applyColorFill(ColorFillRgb{0.0f, 0.0f, 0.0f},
                                 ColorFillParams{.fillR = 0.9f, .fillG = 0.1f, .fillB = 0.4f,
                                                 .fillStrength = 1.0f}) == ColorFillRgb{0.9f, 0.1f, 0.4f},
                  "full strength paints exactly the fill colour");
}

// fillStrength 0.5 → the channel-wise midpoint between the (graded) input and the fill.
TEST(ApplyColorFill, HalfStrengthIsChannelMidpoint) {
    constexpr ColorFillParams p{.fillR = 1.0f, .fillG = 1.0f, .fillB = 1.0f, .fillStrength = 0.5f};
    const ColorFillRgb out = applyColorFill(ColorFillRgb{0.0f, 0.0f, 0.0f}, p);  // mix(0, 1, 0.5)
    EXPECT_FLOAT_EQ(out.r, 0.5f);
}

// The mul/add grade is applied BEFORE the fill mix — ordering matters. mul 0.5 darkens 0.8→0.4, then a
// half-strength mix toward white gives 0.7; applying the fill first would not.
TEST(ApplyColorFill, GradeAppliedBeforeFillMix) {
    constexpr ColorFillParams p{.mulR = 0.5f, .mulG = 0.5f, .mulB = 0.5f,
                                .fillR = 1.0f, .fillG = 1.0f, .fillB = 1.0f, .fillStrength = 0.5f};
    const ColorFillRgb out = applyColorFill(ColorFillRgb{0.8f, 0.8f, 0.8f}, p);  // mix(0.4, 1.0, 0.5)
    EXPECT_FLOAT_EQ(out.r, 0.7f);
}

// ColorFillRgb equality is constexpr.
static_assert(ColorFillRgb{} == ColorFillRgb{}, "default ColorFillRgb compare equal");
static_assert(!(ColorFillRgb{0.5f, 0.0f, 0.0f} == ColorFillRgb{}), "a differing channel compares unequal");

// ── Baseline preservation ──────────────────────────────────────────────────────────────

// An identity ColorFill effect is a pass-through end-to-end: identity params, and applyColorFill leaves
// an arbitrary pixel exactly unchanged.
TEST(ColorFillBaseline, IdentityEffectIsPassThrough) {
    const ColorFillParams p = colorFillParamsOf(1, 1, 1, 0, 0, 0, 0, 0, 0, 0);
    EXPECT_TRUE(p == ColorFillParams{});
    const ColorFillRgb sample{0.42f, 0.17f, 0.93f};
    EXPECT_EQ(applyColorFill(sample, p), sample);  // exact: identity mul/add, no mix
}

// A ColorFill effect is a real kind, not a None pass-through, so the frame-level chain keeps it (and still
// drops the None entries) — the renderer will run its pass.
TEST(ColorFillBaseline, ColorFillEffectSurvivesActiveFilter) {
    FrameDrawState frame;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill});
    frame.postEffects.push_back(ScreenSpaceEffect{});  // None — filtered out
    const std::vector<ScreenSpaceEffect> active = activeFrameEffects(frame);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].kind, ScreenSpaceEffectKind::ColorFill);
}

}  // namespace
}  // namespace retropp
