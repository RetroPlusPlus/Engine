#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"

// ENG-2.C.2.a — post-process composition. Device-free coverage of the CPU side: the chain-build
// helper (activeFrameEffects) and the displacement math the displace.frag GPU stage mirrors
// (displaceSourceUv / displaceParams / displacementOffset). The live ping-pong GPU path is build-
// compiled + dev-verified across all three backends (the documented CI-headless boundary); these
// are the failable units. amplitude is in viewport pixels and the sine curve is GPU-verified, so the
// assertions pin axis routing + px→UV normalization at SINE-EXACT arguments (sin(2π·0.25) = 1).

namespace retropp {
namespace {

constexpr PixelSize kViewport{160, 144};

// ── activeFrameEffects — the frame-level chain ────────────────────────────────────────

TEST(ActiveFrameEffects, EmptyByDefault) {
    const FrameDrawState frame;
    EXPECT_TRUE(activeFrameEffects(frame).empty());
}

TEST(ActiveFrameEffects, FiltersNone) {
    FrameDrawState frame;
    frame.postEffects = {
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::None},
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 2.0f},
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::None},
    };
    const auto active = activeFrameEffects(frame);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].kind, ScreenSpaceEffectKind::RowDisplacement);
    EXPECT_FLOAT_EQ(active[0].amplitude, 2.0f);
}

TEST(ActiveFrameEffects, PreservesSubmissionOrder) {
    FrameDrawState frame;
    frame.postEffects = {
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 1.0f},
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 2.0f},
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 3.0f},
    };
    const auto active = activeFrameEffects(frame);
    ASSERT_EQ(active.size(), 3u);
    EXPECT_FLOAT_EQ(active[0].amplitude, 1.0f);
    EXPECT_FLOAT_EQ(active[1].amplitude, 2.0f);
    EXPECT_FLOAT_EQ(active[2].amplitude, 3.0f);
}

// activeFrameEffects reads ONLY the frame-level postEffects — a per-layer DrawLayer::effect is
// invisible to it (per-layer realization is ENG-2.C.2.b, a different code path).
TEST(ActiveFrameEffects, IgnoresPerLayerEffect) {
    FrameDrawState frame;
    DrawLayer layer{};
    layer.effect = ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 5.0f};
    frame.layers.push_back(layer);
    EXPECT_TRUE(activeFrameEffects(frame).empty());
}

// ── displacementOffset — the constexpr px→UV normalization ────────────────────────────

// amplitude in viewport pixels, normalized to UV by the inverse viewport dimension, times the sine.
// Genuinely constexpr (no sin) → static_assert-testable: 80 px over a 160 px viewport at sine 1 is a
// half-UV offset.
TEST(DisplacementOffset, NormalizesPixelsToUv) {
    EXPECT_FLOAT_EQ(displacementOffset(1.0f, 80.0f, 160), 0.5f);
    EXPECT_FLOAT_EQ(displacementOffset(-1.0f, 80.0f, 160), -0.5f);
    EXPECT_FLOAT_EQ(displacementOffset(1.0f, 0.0f, 160), 0.0f);   // zero amplitude → no offset
    EXPECT_FLOAT_EQ(displacementOffset(1.0f, 80.0f, 0), 0.0f);    // degenerate viewport → no offset
    static_assert(displacementOffset(1.0f, 80.0f, 160) == 0.5f,
                  "80px over a 160px viewport at sine 1 is a half-UV offset");
}

// ── displaceSourceUv — the full mirror (axis routing + normalization) ─────────────────

// amplitude 0 → identity, regardless of axis / frequency / phase.
TEST(DisplaceSourceUv, IdentityAtZeroAmplitude) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 0.0f, .frequency = 3.0f, .phase = 0.25f};
    const Uv uv{0.4f, 0.6f};
    EXPECT_EQ(displaceSourceUv(uv, e, kViewport), uv);
}

// A None-kind effect is also identity (the chain filters these, but the mirror is defensive).
TEST(DisplaceSourceUv, IdentityForNoneKind) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::None, .amplitude = 9.0f};
    const Uv uv{0.3f, 0.7f};
    EXPECT_EQ(displaceSourceUv(uv, e, kViewport), uv);
}

// Horizontal: displaces U by sin of the ROW (v), leaves V. At frequency 1 / phase 0 / v = 0.25 the
// sine argument is 2π·0.25 → sin = 1, so U shifts by amplitude/viewportW exactly.
TEST(DisplaceSourceUv, HorizontalDisplacesUByRow) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 80.0f, .frequency = 1.0f, .phase = 0.0f,
                              .axis = Axis::Horizontal};
    const Uv uv{0.5f, 0.25f};
    const Uv src = displaceSourceUv(uv, e, kViewport);
    EXPECT_FLOAT_EQ(src.u, 0.5f + 0.5f);  // 80/160 · sin(π/2) = 0.5
    EXPECT_FLOAT_EQ(src.v, 0.25f);        // V untouched on the horizontal axis
}

// Vertical: displaces V by sin of the COLUMN (u), leaves U. Same sine-exact construction on the
// other axis, normalized by viewport HEIGHT (144).
TEST(DisplaceSourceUv, VerticalDisplacesVByColumn) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 72.0f, .frequency = 1.0f, .phase = 0.0f,
                              .axis = Axis::Vertical};
    const Uv uv{0.25f, 0.5f};
    const Uv src = displaceSourceUv(uv, e, kViewport);
    EXPECT_FLOAT_EQ(src.u, 0.25f);        // U untouched on the vertical axis
    EXPECT_FLOAT_EQ(src.v, 0.5f + 0.5f);  // 72/144 · sin(π/2) = 0.5
}

// The phase shifts the sine argument: at v = 0, frequency 1, phase 0.25 the argument is 2π·0.25 → 1.
TEST(DisplaceSourceUv, PhaseEntersTheSineArgument) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 160.0f, .frequency = 1.0f, .phase = 0.25f,
                              .axis = Axis::Horizontal};
    const Uv src = displaceSourceUv(Uv{0.0f, 0.0f}, e, kViewport);
    EXPECT_FLOAT_EQ(src.u, 1.0f);  // 160/160 · sin(2π·0.25) = 1
}

// ── Edge boundary: blank (default) vs stretch ─────────────────────────────────────────

// A displaced UV inside [0,1]² samples the source; outside, it does not. The constexpr bounds
// predicate the stage's boundary branch keys off.
TEST(WithinSource, BoundsPredicate) {
    EXPECT_TRUE(withinSource(Uv{0.0f, 0.0f}));
    EXPECT_TRUE(withinSource(Uv{1.0f, 1.0f}));
    EXPECT_TRUE(withinSource(Uv{0.5f, 0.5f}));
    EXPECT_FALSE(withinSource(Uv{-0.01f, 0.5f}));  // pulled past the left edge
    EXPECT_FALSE(withinSource(Uv{1.01f, 0.5f}));   // past the right edge
    EXPECT_FALSE(withinSource(Uv{0.5f, 1.2f}));    // past the bottom
    static_assert(withinSource(Uv{0.5f, 0.5f}));
    static_assert(!withinSource(Uv{-0.1f, 0.5f}));
}

// The boundary decision: Blank (the default) shows the backdrop ONLY where the displaced UV left the
// source; in-bounds it samples. Stretch NEVER shows the backdrop — it samples (the CLAMP_TO_EDGE
// sampler duplicates the edge column). This is the developer-selectable behaviour (default Blank).
TEST(ResolvesToBackdrop, BlankShowsBackdropOnlyOutOfBounds) {
    EXPECT_TRUE(resolvesToBackdrop(Uv{-0.05f, 0.5f}, DisplacementEdge::Blank));   // exposed strip → blank
    EXPECT_FALSE(resolvesToBackdrop(Uv{0.5f, 0.5f}, DisplacementEdge::Blank));    // in-bounds → samples
    static_assert(resolvesToBackdrop(Uv{1.5f, 0.5f}, DisplacementEdge::Blank));
}

TEST(ResolvesToBackdrop, StretchNeverShowsBackdrop) {
    EXPECT_FALSE(resolvesToBackdrop(Uv{-0.05f, 0.5f}, DisplacementEdge::Stretch));  // edge column duplicated
    EXPECT_FALSE(resolvesToBackdrop(Uv{0.5f, 0.5f}, DisplacementEdge::Stretch));
    static_assert(!resolvesToBackdrop(Uv{2.0f, 2.0f}, DisplacementEdge::Stretch));
}

// ── displaceParams — the GPU uniform's resolved values ────────────────────────────────

TEST(DisplaceParams, ResolvesInverseViewportAndAxis) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 3.0f, .frequency = 2.0f, .phase = 0.1f,
                              .axis = Axis::Vertical, .edge = DisplacementEdge::Stretch};
    const DisplaceParams p = displaceParams(e, kViewport);
    EXPECT_FLOAT_EQ(p.amplitude, 3.0f);
    EXPECT_FLOAT_EQ(p.frequency, 2.0f);
    EXPECT_FLOAT_EQ(p.phase, 0.1f);
    EXPECT_EQ(p.axis, 1u);  // Vertical
    EXPECT_FLOAT_EQ(p.invViewportW, 1.0f / 160.0f);
    EXPECT_FLOAT_EQ(p.invViewportH, 1.0f / 144.0f);
    EXPECT_EQ(p.edge, 1u);  // Stretch
}

TEST(DisplaceParams, EdgeDefaultsToBlankZero) {
    // A plain RowDisplacement (no edge specified) resolves to Blank (0) — the faithful default.
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement};
    EXPECT_EQ(displaceParams(e, kViewport).edge, 0u);
    static_assert(displaceParams(ScreenSpaceEffect{}, PixelSize{160, 144}).edge == 0u);
}

TEST(DisplaceParams, HorizontalAxisIsZeroAndDegenerateViewportIsSafe) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement, .axis = Axis::Horizontal};
    const DisplaceParams p = displaceParams(e, PixelSize{0, 0});
    EXPECT_EQ(p.axis, 0u);  // Horizontal
    EXPECT_FLOAT_EQ(p.invViewportW, 0.0f);
    EXPECT_FLOAT_EQ(p.invViewportH, 0.0f);
    static_assert(displaceParams(ScreenSpaceEffect{.axis = Axis::Horizontal}, PixelSize{0, 0}).axis == 0u);
}

// ── ENG-2.C.2.b: per-layer dispatch + scope + the transparent-blank flag ──────────────

// layerHasScreenSpaceEffect — the renderer's per-layer dispatch predicate. A default (None) effect is
// no effect, so the layer composites on the unchanged faithful path.
TEST(LayerHasScreenSpaceEffect, NoneIsNoEffect) {
    DrawLayer layer{};  // default effect kind == None
    EXPECT_FALSE(layerHasScreenSpaceEffect(layer));
}

TEST(LayerHasScreenSpaceEffect, RowDisplacementIsAnEffect) {
    DrawLayer layer{};
    layer.effect.kind = ScreenSpaceEffectKind::RowDisplacement;
    EXPECT_TRUE(layerHasScreenSpaceEffect(layer));
}

// Effect scope defaults to Layer (isolated — displace only this layer); Below is the adjustment-layer
// scope (displace the whole accumulator at this z). The renderer routes the two differently.
TEST(EffectScope, DefaultsToLayerIsolated) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement};
    EXPECT_EQ(e.scope, ScreenSpaceEffectScope::Layer);
    EXPECT_FALSE(effectIsBelowScope(e));
    static_assert(!effectIsBelowScope(ScreenSpaceEffect{}));
}

TEST(EffectScope, BelowRoutesAsAdjustmentLayer) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .scope = ScreenSpaceEffectScope::Below};
    EXPECT_TRUE(effectIsBelowScope(e));
    static_assert(effectIsBelowScope(ScreenSpaceEffect{.scope = ScreenSpaceEffectScope::Below}));
}

// displaceParams.blankTransparent — the scope-dependent blank colour selector. Frame-level + Below
// pass false (opaque backdrop, the C.2.a default); the isolated Layer pass passes true (transparent,
// so the exposed strip reveals the layers below).
TEST(DisplaceParams, BlankTransparentDefaultsOpaque) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 2.0f};
    EXPECT_EQ(displaceParams(e, kViewport).blankTransparent, 0u);
    static_assert(displaceParams(ScreenSpaceEffect{}, PixelSize{160, 144}).blankTransparent == 0u);
}

TEST(DisplaceParams, BlankTransparentTrueForIsolatedLayer) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 2.0f};
    const DisplaceParams p = displaceParams(e, kViewport, /*blankTransparent=*/true);
    EXPECT_EQ(p.blankTransparent, 1u);
    // The flag does not disturb the other resolved params.
    EXPECT_FLOAT_EQ(p.amplitude, 2.0f);
    EXPECT_EQ(p.axis, 0u);
    EXPECT_FLOAT_EQ(p.invViewportW, 1.0f / 160.0f);
    static_assert(displaceParams(ScreenSpaceEffect{}, PixelSize{160, 144}, true).blankTransparent == 1u);
}

// The per-layer realization drives the SAME displacement math as the frame-level path — a regression
// guard that scope changes the renderer's routing, not the displacement curve.
TEST(DisplaceSourceUv, PerLayerEffectUsesTheSameMirror) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 80.0f, .frequency = 1.0f, .phase = 0.0f,
                              .axis = Axis::Horizontal, .scope = ScreenSpaceEffectScope::Layer};
    const Uv src = displaceSourceUv(Uv{0.5f, 0.25f}, e, kViewport);
    EXPECT_FLOAT_EQ(src.u, 1.0f);   // 0.5 + 80/160·sin(π/2)
    EXPECT_FLOAT_EQ(src.v, 0.25f);
}

}  // namespace
}  // namespace retropp
