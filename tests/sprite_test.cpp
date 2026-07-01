#include "retropp/draw_state.h"
#include "retropp/image.h"     // AtlasId
#include "retropp/palette.h"   // PaletteId

#include <cstdint>

#include <gtest/gtest.h>

namespace retropp {

// ── AssetDimensions presets (the value-as-data, self-type-constant idiom) ──────────────────

TEST(AssetDimensions, PresetsCarryTheirDimensions) {
    EXPECT_EQ(AssetDimensions::GameBoy8x8, (AssetDimensions{8, 8}));
    EXPECT_EQ(AssetDimensions::GameBoy8x16, (AssetDimensions{8, 16}));
    EXPECT_EQ(AssetDimensions::GameBoyColor8x8, (AssetDimensions{8, 8}));
    EXPECT_EQ(AssetDimensions::GameBoyColor8x16, (AssetDimensions{8, 16}));
    EXPECT_EQ(AssetDimensions::GameBoyAdvance8x8, (AssetDimensions{8, 8}));
    EXPECT_EQ(AssetDimensions::Nes8x8, (AssetDimensions{8, 8}));
    EXPECT_EQ(AssetDimensions::Nes8x16, (AssetDimensions{8, 16}));
    EXPECT_EQ(AssetDimensions::MasterSystem8x8, (AssetDimensions{8, 8}));
    EXPECT_EQ(AssetDimensions::MasterSystem8x16, (AssetDimensions{8, 16}));
    EXPECT_EQ(AssetDimensions::Snes8x8, (AssetDimensions{8, 8}));
    EXPECT_EQ(AssetDimensions::Snes16x16, (AssetDimensions{16, 16}));
    EXPECT_EQ(AssetDimensions::Snes32x32, (AssetDimensions{32, 32}));
    EXPECT_EQ(AssetDimensions::Snes64x64, (AssetDimensions{64, 64}));
    EXPECT_EQ(AssetDimensions::Genesis32x32, (AssetDimensions{32, 32}));
}

TEST(AssetDimensions, FieldsReadBack) {
    EXPECT_EQ(AssetDimensions::GameBoy8x16.width, 8);
    EXPECT_EQ(AssetDimensions::GameBoy8x16.height, 16);
    EXPECT_EQ(AssetDimensions::Snes64x64.width, 64);
    EXPECT_EQ(AssetDimensions::Snes64x64.height, 64);
}

TEST(AssetDimensions, EqualityComparesBothAxes) {
    EXPECT_EQ((AssetDimensions{16, 16}), (AssetDimensions{16, 16}));
    EXPECT_NE((AssetDimensions{8, 16}), (AssetDimensions{16, 8}));
    EXPECT_NE((AssetDimensions{8, 8}), (AssetDimensions{8, 16}));
}

TEST(AssetDimensions, ArbitrarySizeIsAllowed) {
    constexpr AssetDimensions custom{24, 40};
    static_assert(custom.width == 24 && custom.height == 40);
    EXPECT_EQ(custom.width, 24);
    EXPECT_EQ(custom.height, 40);
}

// ── Sprite defaults ───────────────────────────────────────────────────────────────────

TEST(Sprite, DefaultsToGameBoy8x8AtOriginOpaque) {
    const Sprite s{.key = "s"};
    EXPECT_EQ(s.size, AssetDimensions::GameBoy8x8);
    EXPECT_EQ(s.x, 0);
    EXPECT_EQ(s.y, 0);
    EXPECT_EQ(s.tile, 0u);
    EXPECT_EQ(s.atlas, AtlasId{});       // names its own sheet directly; default handle
    EXPECT_EQ(s.palette, PaletteId{});   // names its own palette directly; default handle
    EXPECT_FALSE(s.flipX);
    EXPECT_FALSE(s.flipY);
    EXPECT_EQ(s.rotation, Rotation::None);
}

// ── packSpriteFlags (the GpuSprite.flags bit layout) ──────────────────────────────────

TEST(SpriteFlags, BitPositionsForEveryCombination) {
    EXPECT_EQ(packSpriteFlags(false, false), 0u);
    EXPECT_EQ(packSpriteFlags(true, false), 1u);   // flipX → bit 0
    EXPECT_EQ(packSpriteFlags(false, true), 2u);   // flipY → bit 1
    EXPECT_EQ(packSpriteFlags(true, true), 3u);
    // rotation → bits 2..3 (None=0, Rot90=1, Rot180=2, Rot270=3).
    EXPECT_EQ(packSpriteFlags(false, false, Rotation::Rot90),  4u);
    EXPECT_EQ(packSpriteFlags(false, false, Rotation::Rot180), 8u);
    EXPECT_EQ(packSpriteFlags(false, false, Rotation::Rot270), 12u);
    // flips + rotation are independent bit fields.
    EXPECT_EQ(packSpriteFlags(true, true, Rotation::Rot270), 3u | 12u);
}

TEST(SpriteFlags, IsConstexpr) {
    static_assert(packSpriteFlags(true, false) == 1u);
    static_assert(packSpriteFlags(false, false, Rotation::Rot270) == 12u);
    SUCCEED();
}

// ── packSpriteAtlasPalette (GpuSprite.atlasPalette: atlas low 16 | palette flat offset high 16) ──

TEST(SpriteAtlasPalette, PacksAtlasLowPaletteHigh) {
    EXPECT_EQ(packSpriteAtlasPalette(static_cast<AtlasId>(0x1234), static_cast<PaletteId>(0xABCD)),
              0xABCD1234u);
    EXPECT_EQ(packSpriteAtlasPalette(static_cast<AtlasId>(0), static_cast<PaletteId>(0)), 0u);
    EXPECT_EQ(packSpriteAtlasPalette(static_cast<AtlasId>(0xFFFF), static_cast<PaletteId>(0xFFFF)),
              0xFFFFFFFFu);
}

TEST(SpriteAtlasPalette, IsConstexpr) {
    static_assert(packSpriteAtlasPalette(static_cast<AtlasId>(5), static_cast<PaletteId>(7)) ==
                  (5u | (7u << 16)));
    SUCCEED();
}

// ── GpuSprite layout (the CPU↔GPU mirror) ─────────────────────────────────────────────

// Reconstruct the baked clip-space homography from a GpuSprite's three rows — the matrix the
// vertex shader applies (clip = H · (cx,cy,1); placement = clip.xy / w). Lets a test evaluate the
// quad at its unit-corner positions the same way the GPU does.
[[nodiscard]] constexpr Transform spriteHomography(const GpuSprite& g) noexcept {
    return Transform{g.row0[0], g.row0[1], g.row0[2],
                     g.row1[0], g.row1[1], g.row1[2],
                     g.row2[0], g.row2[1], g.row2[2]};
}

TEST(GpuSprite, LayoutIs64Bytes) {
    static_assert(sizeof(GpuSprite) == 64);
    EXPECT_EQ(sizeof(GpuSprite), 64u);
}

TEST(AssetDimensionsPacking, PacksWidthHighHeightLow) {
    EXPECT_EQ(packAssetSize(AssetDimensions::GameBoy8x8), (8u << 16) | 8u);
    EXPECT_EQ(packAssetSize(AssetDimensions::GameBoy8x16), (8u << 16) | 16u);
    EXPECT_EQ(packAssetSize(AssetDimensions::Snes64x64), (64u << 16) | 64u);
    EXPECT_EQ(packAssetSize(AssetDimensions{24, 40}), (24u << 16) | 40u);
}

TEST(GpuSprite, MakeBakesClipTransformAndMapsFields) {
    Sprite s{.key = "s"};
    s.x = 32;
    s.y = 64;
    s.size = AssetDimensions::Snes32x32;
    s.tile = 0x0042;
    s.atlas = static_cast<AtlasId>(2);
    s.palette = static_cast<PaletteId>(9);
    s.flipX = true;
    s.flipY = false;

    // viewport 128×128, scroll (0,32) → screen-space top-left (32, 32). Powers of two keep the
    // clip math exactly representable. With identity transforms the corners reproduce the
    // axis-aligned rect: top-left (32px) → clip (-0.5, 0.5); the 32px quad spans +0.5 in x and
    // −0.5 in y (the top-left-origin V-flip); w ≡ 1 (affine).
    const GpuSprite g = makeGpuSprite(s, 128, 128, 0, 32);
    const Transform H = spriteHomography(g);
    EXPECT_FLOAT_EQ(H.applyX(0.0f, 0.0f), -0.5f);  // 32/128*2 - 1
    EXPECT_FLOAT_EQ(H.applyX(1.0f, 0.0f),  0.0f);  // + 32/128*2
    EXPECT_FLOAT_EQ(H.applyY(0.0f, 0.0f),  0.5f);  // 1 - 32/128*2
    EXPECT_FLOAT_EQ(H.applyY(0.0f, 1.0f),  0.0f);  // - 32/128*2 (V-flip)
    EXPECT_FLOAT_EQ(H.weight(0.0f, 0.0f),  1.0f);  // affine → w ≡ 1
    EXPECT_EQ(g.tile, 0x0042u);
    EXPECT_EQ(g.atlasPalette,
              packSpriteAtlasPalette(static_cast<AtlasId>(2), static_cast<PaletteId>(9)));
    EXPECT_EQ(g.flags, packSpriteFlags(true, false));  // flipX → 1
    EXPECT_EQ(g.size, (32u << 16) | 32u);
}

TEST(GpuSprite, MakeCarriesRotationInFlags) {
    Sprite s{.key = "s"};
    s.flipX = true;
    s.rotation = Rotation::Rot270;
    const GpuSprite g = makeGpuSprite(s, 128, 128, 0, 0);
    EXPECT_EQ(g.flags, packSpriteFlags(true, false, Rotation::Rot270));  // flipX bit + rotation bits 2..3
}

TEST(GpuSprite, MakeAppliesScrollAndViewport) {
    Sprite s{.key = "s"};
    s.x = 64;
    s.y = 0;
    s.size = AssetDimensions::GameBoy8x8;
    // No scroll, square viewport: x at the centre maps to clip 0.
    const Transform a = spriteHomography(makeGpuSprite(s, 128, 128, 0, 0));
    EXPECT_FLOAT_EQ(a.applyX(0.0f, 0.0f), 0.0f);   // 64/128*2 - 1
    EXPECT_FLOAT_EQ(a.applyY(0.0f, 0.0f), 1.0f);   // 1 - 0/128*2  (top edge)
    // Scrolling right by 64 moves the same sprite a full half-screen left in clip space.
    const Transform b = spriteHomography(makeGpuSprite(s, 128, 128, 64, 0));
    EXPECT_FLOAT_EQ(b.applyX(0.0f, 0.0f), -1.0f);  // (64-64)/128*2 - 1
}

TEST(GpuSprite, MakeIsConstexpr) {
    constexpr Sprite s{.key = "s"};  // a required key; Sprite stays constexpr
    constexpr GpuSprite g = makeGpuSprite(s, 160, 144, 0, 0);
    static_assert(g.flags == 0u && g.size == ((8u << 16) | 8u));
    EXPECT_EQ(g.size, (8u << 16) | 8u);
}

// ── Sub-pixel placement: the float-position overload (output-resolution smoothness) ──────

TEST(GpuSprite, FloatPositionOverloadMatchesTheSpriteIntegerPosition) {
    // The explicit-position overload placed at the sprite's own whole-pixel x/y is byte-identical to the
    // convenience overload — the regression lock: nothing about the OFF/faithful path changed.
    Sprite s{.key = "s"};
    s.x = 40;
    s.y = 24;
    s.size = AssetDimensions{16, 16};
    const GpuSprite viaConvenience = makeGpuSprite(s, 160, 144, 8, 4);            // reads s.x/s.y
    const GpuSprite viaExplicit     = makeGpuSprite(s, 160, 144, 40.0f, 24.0f, 8.0f, 4.0f);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(viaConvenience.row0[i], viaExplicit.row0[i]);
        EXPECT_EQ(viaConvenience.row1[i], viaExplicit.row1[i]);
        EXPECT_EQ(viaConvenience.row2[i], viaExplicit.row2[i]);
    }
}

TEST(GpuSprite, FractionalPositionShiftsClipSubPixel) {
    // A sub-pixel position (the interpolated placement) shifts the baked clip quad by the corresponding
    // sub-viewport-pixel amount — which, rasterized onto a scale× target, lands on a different output
    // pixel. Half a viewport pixel on a 160-wide viewport is 0.5·(2/160) in clip x.
    Sprite s{.key = "s"};
    s.size = AssetDimensions{8, 8};
    const Transform whole = spriteHomography(makeGpuSprite(s, 160, 144, 10.0f, 20.0f, 0.0f, 0.0f));
    const Transform half  = spriteHomography(makeGpuSprite(s, 160, 144, 10.5f, 20.0f, 0.0f, 0.0f));
    // NEAR, not FLOAT_EQ: differencing two clip values near −0.87 cancels away the ULP budget.
    EXPECT_NEAR(half.applyX(0.0f, 0.0f) - whole.applyX(0.0f, 0.0f), 0.5f * 2.0f / 160.0f, 1e-5f);
    EXPECT_NEAR(half.applyY(0.0f, 0.0f) - whole.applyY(0.0f, 0.0f), 0.0f, 1e-5f);  // y unchanged
}

// ── SpriteContent variant wiring ──────────────────────────────────────────────────────

TEST(SpriteContent, IsTheSpritesAlternativeOfLayerContent) {
    LayerContent c = SpriteContent{};
    EXPECT_EQ(contentKind(c), LayerContentKind::Sprites);
    LayerContent t = TileContent{};
    EXPECT_EQ(contentKind(t), LayerContentKind::Tiles);
}

}  // namespace retropp
