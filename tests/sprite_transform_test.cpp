// Per-sprite + per-layer geometric transform on the SPRITES path.
//
// makeGpuSprite bakes a COMPOSED clip-space homography into each GpuSprite: a unit-quad corner
// (cx, cy) ∈ {0,1}² travels scale(w,h) → Sprite::transform → translate(scrolled top-left) →
// DrawLayer::transform → screen→clip. The vertex shader then evaluates clip = H · (cx,cy,1) and
// the GPU perspective-divides. These headless tests reconstruct H from the baked rows and evaluate
// it at the unit corners — the exact placement the GPU computes — so the bake is verified without a
// device. The cornerstone is the IDENTITY case: with no sprite/layer transform the corners must
// reproduce the axis-aligned (clipX + cx·clipW, clipY + cy·clipH) rect byte-for-byte.

#include "retropp/draw_state.h"
#include "retropp/transform.h"

#include <gtest/gtest.h>

namespace retropp {
namespace {

// Reconstruct the baked homography from a GpuSprite's three rows (the matrix the vertex shader
// applies). Pure + constexpr so identity-case assertions can fold at compile time.
[[nodiscard]] constexpr Transform homographyOf(const GpuSprite& g) noexcept {
    return Transform{g.row0[0], g.row0[1], g.row0[2],
                     g.row1[0], g.row1[1], g.row1[2],
                     g.row2[0], g.row2[1], g.row2[2]};
}

constexpr float kTol = 1e-4f;

// ── The cornerstone: identity transforms reproduce the axis-aligned quad byte-for-byte ──

TEST(SpriteTransform, IdentityReproducesLegacyAxisAlignedQuad) {
    Sprite s;
    s.x = 40;
    s.y = 24;
    s.size = AssetDimensions{16, 16};
    // viewport 160×144, scroll (8, 4) → screen top-left (32, 20). The axis-aligned bake would have
    // produced clipX = 32/160*2-1, clipW = 16/160*2, clipY = 1-20/144*2, clipH = -(16/144*2).
    const GpuSprite g = makeGpuSprite(s, 160, 144, 8, 4);  // no layer transform (defaulted)
    const Transform H = homographyOf(g);

    const float clipX = 32.0f / 160.0f * 2.0f - 1.0f;
    const float clipW = 16.0f / 160.0f * 2.0f;
    const float clipY = 1.0f - 20.0f / 144.0f * 2.0f;
    const float clipH = -(16.0f / 144.0f * 2.0f);

    // Every unit corner maps to the rect's corresponding corner, w ≡ 1 (affine).
    EXPECT_NEAR(H.applyX(0.0f, 0.0f), clipX,          kTol);
    EXPECT_NEAR(H.applyY(0.0f, 0.0f), clipY,          kTol);
    EXPECT_NEAR(H.applyX(1.0f, 0.0f), clipX + clipW,  kTol);
    EXPECT_NEAR(H.applyY(1.0f, 0.0f), clipY,          kTol);
    EXPECT_NEAR(H.applyX(0.0f, 1.0f), clipX,          kTol);
    EXPECT_NEAR(H.applyY(0.0f, 1.0f), clipY + clipH,  kTol);
    EXPECT_NEAR(H.applyX(1.0f, 1.0f), clipX + clipW,  kTol);
    EXPECT_NEAR(H.applyY(1.0f, 1.0f), clipY + clipH,  kTol);
    EXPECT_NEAR(H.weight(0.5f, 0.5f), 1.0f,           kTol);
}

TEST(SpriteTransform, IdentityFoldsAtCompileTime) {
    constexpr Sprite s{};
    constexpr GpuSprite g = makeGpuSprite(s, 160, 144, 0, 0);
    // Affine identity case: bottom row is (0,0,1), so w is constant 1.
    static_assert(g.row2[0] == 0.0f && g.row2[1] == 0.0f && g.row2[2] == 1.0f);
    SUCCEED();
}

// ── Per-sprite transform ──────────────────────────────────────────────────────────────

TEST(SpriteTransform, PerSpriteTranslationOffsetsTheQuad) {
    Sprite base;
    base.x = 0;
    base.y = 0;
    base.size = AssetDimensions{8, 8};

    Sprite moved = base;
    moved.transform = Transform::translation(8.0f, 0.0f);  // shift one sprite-width right, sprite-local

    // viewport 16×16: 8 sprite-local px = 8 screen px = +1.0 in clip x.
    const Transform a = homographyOf(makeGpuSprite(base,  16, 16, 0, 0));
    const Transform b = homographyOf(makeGpuSprite(moved, 16, 16, 0, 0));
    EXPECT_NEAR(b.applyX(0.0f, 0.0f) - a.applyX(0.0f, 0.0f), 1.0f, kTol);  // 8/16*2
    EXPECT_NEAR(b.applyY(0.0f, 0.0f) - a.applyY(0.0f, 0.0f), 0.0f, kTol);
}

TEST(SpriteTransform, PerSpriteScaleAboutCentre) {
    Sprite s;
    s.x = 0;
    s.y = 0;
    s.size = AssetDimensions{8, 8};
    s.transform = Transform::scale(2.0f, 2.0f, 4.0f, 4.0f);  // 2× about the sprite centre (4,4)

    // Scaling about the centre keeps the centre fixed and doubles the half-extent. viewport 16×16.
    const Transform H = homographyOf(makeGpuSprite(s, 16, 16, 0, 0));
    // Centre unit (0.5,0.5) → sprite-local (4,4) → fixed point of the scale → screen (4,4) → clip.
    const float centreClipX = 4.0f / 16.0f * 2.0f - 1.0f;  // -0.5
    const float centreClipY = 1.0f - 4.0f / 16.0f * 2.0f;  //  0.5
    EXPECT_NEAR(H.applyX(0.5f, 0.5f), centreClipX, kTol);
    EXPECT_NEAR(H.applyY(0.5f, 0.5f), centreClipY, kTol);
    // Top-left corner moves out to sprite-local (-4,-4) → screen (-4,-4).
    EXPECT_NEAR(H.applyX(0.0f, 0.0f), -4.0f / 16.0f * 2.0f - 1.0f, kTol);  // -1.5
    EXPECT_NEAR(H.applyY(0.0f, 0.0f), 1.0f + 4.0f / 16.0f * 2.0f,  kTol);  //  1.5
}

TEST(SpriteTransform, PerSpriteRotation90AboutCentre) {
    Sprite s;
    s.x = 0;
    s.y = 0;
    s.size = AssetDimensions{8, 8};
    s.transform = Transform::rotation(90.0f, 4.0f, 4.0f);  // 90° CW about the sprite centre

    // Worked by hand: rotation(90) about (4,4) maps sprite-local (x,y) → (8 - y, x); screen→clip
    // with vw=vh=16 is x'=x/8-1, y'=1-y/8. Unit(0,0)→local(0,0)→(8,0)→clip(0,1).
    const Transform H = homographyOf(makeGpuSprite(s, 16, 16, 0, 0));
    EXPECT_NEAR(H.applyX(0.0f, 0.0f),  0.0f, kTol);  // local (8,0)
    EXPECT_NEAR(H.applyY(0.0f, 0.0f),  1.0f, kTol);
    EXPECT_NEAR(H.applyX(1.0f, 0.0f),  0.0f, kTol);  // local (8,8)
    EXPECT_NEAR(H.applyY(1.0f, 0.0f),  0.0f, kTol);
    EXPECT_NEAR(H.applyX(0.0f, 1.0f), -1.0f, kTol);  // local (0,0)
    EXPECT_NEAR(H.applyY(0.0f, 1.0f),  1.0f, kTol);
    // The centre is invariant under a rotation about itself.
    EXPECT_NEAR(H.applyX(0.5f, 0.5f), -0.5f, kTol);
    EXPECT_NEAR(H.applyY(0.5f, 0.5f),  0.5f, kTol);
}

// ── Per-layer transform reaches sprites ─────────────────────────────────────────────────

TEST(SpriteTransform, PerLayerTransformAloneMovesAllCorners) {
    Sprite s;  // identity sprite at origin, 8×8
    s.size = AssetDimensions{8, 8};
    const Transform layer = Transform::translation(16.0f, 0.0f);  // shift the whole layer +16 screen px

    const Transform plain   = homographyOf(makeGpuSprite(s, 32, 32, 0, 0));
    const Transform shifted = homographyOf(makeGpuSprite(s, 32, 32, 0, 0, layer));
    // 16 screen px on a 32px viewport = +1.0 clip x, applied to every corner identically.
    for (float cx : {0.0f, 1.0f}) {
        for (float cy : {0.0f, 1.0f}) {
            EXPECT_NEAR(shifted.applyX(cx, cy) - plain.applyX(cx, cy), 1.0f, kTol);
            EXPECT_NEAR(shifted.applyY(cx, cy) - plain.applyY(cx, cy), 0.0f, kTol);
        }
    }
}

TEST(SpriteTransform, SpriteThenLayerComposeInOrder) {
    Sprite s;
    s.size = AssetDimensions{8, 8};
    s.transform = Transform::translation(8.0f, 0.0f);          // sprite-local: +8 px right
    const Transform layer = Transform::scale(2.0f, 1.0f, 0.0f, 0.0f);  // layer: 2× horizontally about screen x=0

    // The sprite offset happens in sprite-local space, THEN the layer doubles it in screen space:
    // a +8px sprite-local shift at screen origin becomes +16 screen px after the layer 2× → +1.0
    // clip x on a 32px viewport. Order matters: layer-then-sprite would give +8px → +0.5.
    const Transform plain = homographyOf(makeGpuSprite(Sprite{.size = AssetDimensions{8, 8}},
                                                       32, 32, 0, 0, layer));
    const Transform both  = homographyOf(makeGpuSprite(s, 32, 32, 0, 0, layer));
    EXPECT_NEAR(both.applyX(0.0f, 0.0f) - plain.applyX(0.0f, 0.0f), 1.0f, kTol);
}

// ── Projective support survives the bake ────────────────────────────────────────────────

TEST(SpriteTransform, PerspectiveYieldsVaryingW) {
    Sprite s;
    s.size = AssetDimensions{8, 8};
    s.transform = Transform::perspective(0.05f, 0.0f);  // foreshorten along sprite-local x

    const Transform H = homographyOf(makeGpuSprite(s, 64, 64, 0, 0));
    // A perspective transform makes the homogeneous w vary across the quad (w ≢ 1) — the property an
    // affine "origin + two edge vectors" representation could NOT carry.
    const float wNear = H.weight(0.0f, 0.0f);  // sprite-local x=0
    const float wFar  = H.weight(1.0f, 0.0f);  // sprite-local x=8 → w grew by 0.05*8
    EXPECT_NEAR(wNear, 1.0f, kTol);
    EXPECT_NEAR(wFar,  1.4f, kTol);            // 1 + 0.05*8
    EXPECT_GT(wFar, wNear + 0.1f);             // genuinely projective, not affine
}

// ── Scroll subtraction + field passthrough ──────────────────────────────────────────────

TEST(SpriteTransform, ScrollShiftsTheQuadBeforeTheLayerTransform) {
    Sprite s;
    s.size = AssetDimensions{8, 8};
    const Transform a = homographyOf(makeGpuSprite(s, 32, 32, 0,  0));
    const Transform b = homographyOf(makeGpuSprite(s, 32, 32, 16, 0));  // scroll right 16
    // Scrolling the layer right moves the sprite left in screen space: -16 px on a 32px viewport = -1.0 clip x.
    EXPECT_NEAR(b.applyX(0.0f, 0.0f) - a.applyX(0.0f, 0.0f), -1.0f, kTol);
}

TEST(SpriteTransform, AttrFieldsPassThroughUnchangedByTransforms) {
    Sprite s;
    s.size = AssetDimensions{16, 16};
    s.tile = 0x00AB;
    s.atlas = static_cast<AtlasId>(3);
    s.palette = static_cast<PaletteId>(7);
    s.flipX = true;
    s.flipY = true;
    s.transform = Transform::rotation(33.0f, 8.0f, 8.0f);  // arbitrary transform must not touch attr

    const GpuSprite g = makeGpuSprite(s, 64, 64, 0, 0, Transform::scale(3.0f, 3.0f));
    EXPECT_EQ(g.tile, 0x00ABu);
    EXPECT_EQ(g.atlasPalette,
              packSpriteAtlasPalette(static_cast<AtlasId>(3), static_cast<PaletteId>(7)));
    EXPECT_EQ(g.flags, packSpriteFlags(true, true));  // flip bits independent of geometry
    EXPECT_EQ(g.size, (16u << 16) | 16u);
}

}  // namespace
}  // namespace retropp
