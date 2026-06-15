#include "retropp/draw_state.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

// ENG-2.B.2.c.2 — frame finish. Device-free coverage of the frame-level colour transform
// (the pure CPU mirror the blit fragment shader applies), N-layer composition across role-free
// mixed-content layers, and the postEffects carriage. The live blit/transform path is build-
// compiled + dev-verified (the documented CI-headless boundary); these are the failable units.

namespace retropp {
namespace {

// ── frameColorTransform — the blit-stage transform's CPU mirror ───────────────────────

// The faithful-baseline assertion: a default frame (no modifier, no blend) resolves to the
// IDENTITY, so the blit renders byte-identically to the pre-c.2 pass-through. Break this (give
// the identity a non-1 mul or non-0 add) and the baseline silently shifts — so it is a static
// guarantee too.
TEST(FrameColorTransform, DefaultIsIdentity) {
    EXPECT_EQ(frameColorTransform(ColorModifier{}, Blend{}), FrameColorTransform{});
    static_assert(frameColorTransform(ColorModifier{}, Blend{}) == FrameColorTransform{},
                  "a default frame must resolve to the identity colour transform");
}

TEST(FrameColorTransform, MultiplyAddMapsEveryChannel) {
    const ColorModifier m{.kind = ColorModifierKind::MultiplyAdd,
                          .mulR = 0.5f, .mulG = 0.25f, .mulB = 0.75f,
                          .addR = 0.1f, .addG = 0.2f, .addB = 0.3f};
    const FrameColorTransform t = frameColorTransform(m, Blend{});
    EXPECT_FLOAT_EQ(t.mulR, 0.5f);
    EXPECT_FLOAT_EQ(t.mulG, 0.25f);
    EXPECT_FLOAT_EQ(t.mulB, 0.75f);
    EXPECT_FLOAT_EQ(t.addR, 0.1f);
    EXPECT_FLOAT_EQ(t.addG, 0.2f);
    EXPECT_FLOAT_EQ(t.addB, 0.3f);
    // Blend untouched → flash identity.
    EXPECT_FLOAT_EQ(t.flashStrength, 0.0f);
}

// A None-kind modifier whose fields are (nonsensically) non-identity must STILL resolve to the
// multiply/add identity — the kind gates the effect, not the field values.
TEST(FrameColorTransform, NoneModifierLeavesMultiplyAddIdentity) {
    const ColorModifier m{.kind = ColorModifierKind::None, .mulR = 9.0f, .addR = 9.0f};
    const FrameColorTransform t = frameColorTransform(m, Blend{});
    EXPECT_FLOAT_EQ(t.mulR, 1.0f);
    EXPECT_FLOAT_EQ(t.addR, 0.0f);
}

TEST(FrameColorTransform, FlashMapsColourAndStrength) {
    const Blend b{.kind = BlendKind::Flash, .r = 1.0f, .g = 0.5f, .b = 0.0f, .strength = 0.4f};
    const FrameColorTransform t = frameColorTransform(ColorModifier{}, b);
    EXPECT_FLOAT_EQ(t.flashR, 1.0f);
    EXPECT_FLOAT_EQ(t.flashG, 0.5f);
    EXPECT_FLOAT_EQ(t.flashB, 0.0f);
    EXPECT_FLOAT_EQ(t.flashStrength, 0.4f);
    // Modifier untouched → multiply/add identity.
    EXPECT_FLOAT_EQ(t.mulR, 1.0f);
    EXPECT_FLOAT_EQ(t.addR, 0.0f);
}

TEST(FrameColorTransform, NoneBlendLeavesFlashIdentity) {
    const Blend b{.kind = BlendKind::None, .r = 1.0f, .strength = 1.0f};
    EXPECT_FLOAT_EQ(frameColorTransform(ColorModifier{}, b).flashStrength, 0.0f);
}

TEST(FrameColorTransform, StrengthClampedToUnitRange) {
    const Blend over{.kind = BlendKind::Flash, .strength = 2.5f};
    EXPECT_FLOAT_EQ(frameColorTransform(ColorModifier{}, over).flashStrength, 1.0f);
    const Blend under{.kind = BlendKind::Flash, .strength = -3.0f};
    EXPECT_FLOAT_EQ(frameColorTransform(ColorModifier{}, under).flashStrength, 0.0f);
    static_assert(frameColorTransform(ColorModifier{}, Blend{.kind = BlendKind::Flash,
                                                            .strength = 5.0f}).flashStrength == 1.0f);
}

TEST(FrameColorTransform, ModifierAndBlendMapIndependently) {
    const ColorModifier m{.kind = ColorModifierKind::MultiplyAdd, .mulR = 0.2f, .addB = 0.6f};
    const Blend b{.kind = BlendKind::Flash, .r = 1.0f, .strength = 0.8f};
    const FrameColorTransform t = frameColorTransform(m, b);
    EXPECT_FLOAT_EQ(t.mulR, 0.2f);
    EXPECT_FLOAT_EQ(t.addB, 0.6f);
    EXPECT_FLOAT_EQ(t.flashR, 1.0f);
    EXPECT_FLOAT_EQ(t.flashStrength, 0.8f);
}

// ── N-layer composition: role-free, mixed content, ordered back-to-front ──────────────

// A layer carries either tile or sprite content — there is NO engine role. Any content can sit
// at any z; "the layer above the sprite layer" (the priority realization) is just the highest-z
// layer, whatever it holds. This hardens the ordering across 4+ mixed-kind layers.
TEST(FrameFinish, MixedContentLayersOrderBackToFrontRoleFree) {
    std::vector<DrawLayer> layers;
    // Submission order deliberately scrambled; content kinds deliberately mixed at every z.
    layers.push_back(DrawLayer{.id = "foreground", .z = 30, .content = TileContent{}});    // top: TILES foreground
    layers.push_back(DrawLayer{.id = "farBackground", .z = 0, .content = TileContent{}});  // far bg
    layers.push_back(DrawLayer{.id = "characters", .z = 20, .content = SpriteContent{}});  // characters
    layers.push_back(DrawLayer{.id = "spriteBand", .z = 10, .content = SpriteContent{}});  // a sprite-content bg band

    const auto order = layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw);
    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(layers[order[0]].z, 0);    // composited first (back)
    EXPECT_EQ(layers[order[1]].z, 10);
    EXPECT_EQ(layers[order[2]].z, 20);
    EXPECT_EQ(layers[order[3]].z, 30);   // composited last (front) — the foreground-over-sprites layer

    // The front layer being TILES and a mid layer being SPRITES proves no role is fixed to a slot.
    EXPECT_EQ(contentKind(layers[order[3]].content), LayerContentKind::Tiles);
    EXPECT_EQ(contentKind(layers[order[2]].content), LayerContentKind::Sprites);
}

// Z values are ARBITRARY and need not be sequential or contiguous: a game can keep its sprite
// layers in a z-band like 40..60, drop a layer at z=255 right next to one at z=15, and use gaps
// or negatives freely. There is no "z=1 must exist before z=2" rule — the only constraint is that
// z be unique within a frame so the front-to-back order is unambiguous. Higher z is nearer the
// top; the compositor just sorts ascending and paints back-to-front, so gaps cost nothing. (Ids
// are equally free — non-sequential here too — and the renderer keys GPU resources by submission
// position, never by id or z value.)
TEST(FrameFinish, ArbitrarySparseZComposesByDepth) {
    std::vector<DrawLayer> layers{
        DrawLayer{.id = "hud",             .z = 255, .content = TileContent{}},    // a far-foreground HUD at z=255
        DrawLayer{.id = "background",       .z = 15,  .content = TileContent{}},    // a background at z=15
        DrawLayer{.id = "bottomSprites",    .z = 45,  .content = SpriteContent{}},  // sprite layer in the 40..60 band
        DrawLayer{.id = "topSprites",       .z = 55,  .content = SpriteContent{}},  // sprite layer in the 40..60 band
    };
    const auto order = layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw);
    ASSERT_EQ(order.size(), 4u);
    // Pure z-ascending (back-to-front) order, regardless of how sparse / unordered the z's are.
    EXPECT_EQ(layers[order[0]].z, 15);   // bottom / back
    EXPECT_EQ(layers[order[1]].z, 45);   // sprite band
    EXPECT_EQ(layers[order[2]].z, 55);   // sprite band
    EXPECT_EQ(layers[order[3]].z, 255);  // top / front
}

// LayerId is a human-readable LABEL with no depth role: depth follows z alone, never the name's
// lexical order. The name is stored verbatim and a duplicate name is rejected (identity must be
// unique). "we're not working on a database" — ids are names, not sequential keys.
TEST(FrameFinish, LayerIdIsANameWithNoDepthRole) {
    // Names sort OPPOSITE to z ("aaa" < "zzz" lexically, but z puts zzz on top): order must follow z.
    const std::vector<DrawLayer> layers{
        DrawLayer{.id = "zzzTopLayer",    .z = 100},
        DrawLayer{.id = "aaaBottomLayer", .z = 0},
    };
    const auto order = layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw);
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(layers[order[0]].id, LayerId{"aaaBottomLayer"});  // z=0 → back, despite name sorting last
    EXPECT_EQ(layers[order[1]].id, LayerId{"zzzTopLayer"});     // z=100 → front
    EXPECT_EQ(layers[order[1]].id.name, "zzzTopLayer");         // name preserved verbatim

    // A duplicate name is a DuplicateId collision — identity must be unambiguous.
    const std::vector<DrawLayer> dup{
        DrawLayer{.id = "player", .z = 0},
        DrawLayer{.id = "player", .z = 10},
    };
    EXPECT_THROW((void)layerDrawOrder(dup, LayerKeyCollisionPolicy::Throw), std::invalid_argument);
}

TEST(FrameFinish, ManyLayerStackStillTripsDuplicateZ) {
    constexpr std::array<const char*, 6> names{"l0", "l1", "l2", "l3", "l4", "l5"};
    std::vector<DrawLayer> layers;
    for (int i = 0; i < 6; ++i) {
        layers.push_back(DrawLayer{.id = names[static_cast<std::size_t>(i)], .z = i});
    }
    layers.push_back(DrawLayer{.id = "extra", .z = 3});  // z collides with l3
    EXPECT_THROW((void)layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw), std::invalid_argument);
}

// ── postEffects carriage (realized in ENG-2.C; carried + independent here) ────────────

// The frame-level postEffects list is carried by FrameDrawState and is independent of the colour
// transform (which reads only globalModifier + blend). c.2 confirms it is preserved; its shader
// realization is ENG-2.C / Issue 5.
TEST(FrameFinish, PostEffectsAreCarriedAndIndependentOfColourTransform) {
    FrameDrawState frame;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement,
                                                  .amplitude = 2.0f});
    frame.globalModifier = ColorModifier{.kind = ColorModifierKind::MultiplyAdd, .mulR = 0.5f};

    // The list is preserved verbatim...
    ASSERT_EQ(frame.postEffects.size(), 1u);
    EXPECT_EQ(frame.postEffects[0].kind, ScreenSpaceEffectKind::RowDisplacement);
    // ...and the colour transform is computed only from the modifier + blend, not the effects.
    EXPECT_FLOAT_EQ(frameColorTransform(frame.globalModifier, frame.blend).mulR, 0.5f);
}

}  // namespace
}  // namespace retropp
