#include "gbcpp/draw_state.h"
#include "gbcpp/palette.h"

#include <array>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

namespace gbcpp {

// ── SpriteSize presets (the value-as-data, self-type-constant idiom) ──────────────────

TEST(SpriteSize, PresetsCarryTheirDimensions) {
    EXPECT_EQ(SpriteSize::GameBoy8x8, (SpriteSize{8, 8}));
    EXPECT_EQ(SpriteSize::GameBoy8x16, (SpriteSize{8, 16}));
    EXPECT_EQ(SpriteSize::GameBoyColor8x8, (SpriteSize{8, 8}));
    EXPECT_EQ(SpriteSize::GameBoyColor8x16, (SpriteSize{8, 16}));
    EXPECT_EQ(SpriteSize::GameBoyAdvance8x8, (SpriteSize{8, 8}));
    EXPECT_EQ(SpriteSize::Nes8x8, (SpriteSize{8, 8}));
    EXPECT_EQ(SpriteSize::Nes8x16, (SpriteSize{8, 16}));
    EXPECT_EQ(SpriteSize::MasterSystem8x8, (SpriteSize{8, 8}));
    EXPECT_EQ(SpriteSize::MasterSystem8x16, (SpriteSize{8, 16}));
    EXPECT_EQ(SpriteSize::Snes8x8, (SpriteSize{8, 8}));
    EXPECT_EQ(SpriteSize::Snes16x16, (SpriteSize{16, 16}));
    EXPECT_EQ(SpriteSize::Snes32x32, (SpriteSize{32, 32}));
    EXPECT_EQ(SpriteSize::Snes64x64, (SpriteSize{64, 64}));
    EXPECT_EQ(SpriteSize::Genesis32x32, (SpriteSize{32, 32}));
}

TEST(SpriteSize, FieldsReadBack) {
    EXPECT_EQ(SpriteSize::GameBoy8x16.width, 8);
    EXPECT_EQ(SpriteSize::GameBoy8x16.height, 16);
    EXPECT_EQ(SpriteSize::Snes64x64.width, 64);
    EXPECT_EQ(SpriteSize::Snes64x64.height, 64);
}

TEST(SpriteSize, EqualityComparesBothAxes) {
    EXPECT_EQ((SpriteSize{16, 16}), (SpriteSize{16, 16}));
    EXPECT_NE((SpriteSize{8, 16}), (SpriteSize{16, 8}));
    EXPECT_NE((SpriteSize{8, 8}), (SpriteSize{8, 16}));
}

TEST(SpriteSize, ArbitrarySizeIsAllowed) {
    constexpr SpriteSize custom{24, 40};
    static_assert(custom.width == 24 && custom.height == 40);
    EXPECT_EQ(custom.width, 24);
    EXPECT_EQ(custom.height, 40);
}

// ── Sprite defaults ───────────────────────────────────────────────────────────────────

TEST(Sprite, DefaultsToGameBoy8x8AtOriginOpaque) {
    const Sprite s;
    EXPECT_EQ(s.size, SpriteSize::GameBoy8x8);
    EXPECT_EQ(s.x, 0);
    EXPECT_EQ(s.y, 0);
    EXPECT_EQ(s.tile, 0u);
    EXPECT_EQ(s.palette, 0u);
    EXPECT_FALSE(s.flipX);
    EXPECT_FALSE(s.flipY);
    EXPECT_FALSE(s.priority);
}

// ── packSpriteFlags (the GpuSprite.flags bit layout) ──────────────────────────────────

TEST(SpriteFlags, BitPositionsForEveryCombination) {
    EXPECT_EQ(packSpriteFlags(false, false, false), 0u);
    EXPECT_EQ(packSpriteFlags(true, false, false), 1u);   // flipX → bit 0
    EXPECT_EQ(packSpriteFlags(false, true, false), 2u);   // flipY → bit 1
    EXPECT_EQ(packSpriteFlags(false, false, true), 4u);   // priority → bit 2
    EXPECT_EQ(packSpriteFlags(true, true, false), 3u);
    EXPECT_EQ(packSpriteFlags(true, false, true), 5u);
    EXPECT_EQ(packSpriteFlags(false, true, true), 6u);
    EXPECT_EQ(packSpriteFlags(true, true, true), 7u);
}

TEST(SpriteFlags, IsConstexpr) {
    static_assert(packSpriteFlags(true, false, true) == 5u);
    SUCCEED();
}

// ── GpuSprite layout (the CPU↔GPU mirror) ─────────────────────────────────────────────

// Reconstruct the baked clip-space homography from a GpuSprite's three rows — the matrix the
// vertex shader applies (clip = H · (cx,cy,1); placement = clip.xy / w). Lets a test evaluate the
// quad at its unit-corner positions the same way the GPU does. (ENG-2.D.2 generalized the record
// from an axis-aligned (clipX,clipY,clipW,clipH) rect to this homography.)
[[nodiscard]] constexpr Transform spriteHomography(const GpuSprite& g) noexcept {
    return Transform{g.row0[0], g.row0[1], g.row0[2],
                     g.row1[0], g.row1[1], g.row1[2],
                     g.row2[0], g.row2[1], g.row2[2]};
}

TEST(GpuSprite, LayoutIs64Bytes) {
    static_assert(sizeof(GpuSprite) == 64);
    EXPECT_EQ(sizeof(GpuSprite), 64u);
}

TEST(SpriteSizePacking, PacksWidthHighHeightLow) {
    EXPECT_EQ(packSpriteSize(SpriteSize::GameBoy8x8), (8u << 16) | 8u);
    EXPECT_EQ(packSpriteSize(SpriteSize::GameBoy8x16), (8u << 16) | 16u);
    EXPECT_EQ(packSpriteSize(SpriteSize::Snes64x64), (64u << 16) | 64u);
    EXPECT_EQ(packSpriteSize(SpriteSize{24, 40}), (24u << 16) | 40u);
}

TEST(GpuSprite, MakeBakesClipTransformAndMapsFields) {
    Sprite s;
    s.x = 32;
    s.y = 64;
    s.size = SpriteSize::Snes32x32;
    s.tile = 0x0042;
    s.palette = 3;
    s.flipX = true;
    s.flipY = false;
    s.priority = true;

    // viewport 128×128, scroll (0,32) → screen-space top-left (32, 32). Powers of two keep the
    // clip math exactly representable. With identity transforms the corners reproduce the pre-D.2
    // axis-aligned rect: top-left (32px) → clip (-0.5, 0.5); the 32px quad spans +0.5 in x and
    // −0.5 in y (the top-left-origin V-flip); w ≡ 1 (affine).
    const GpuSprite g = makeGpuSprite(s, 9u, 128, 128, 0, 32);
    const Transform H = spriteHomography(g);
    EXPECT_FLOAT_EQ(H.applyX(0.0f, 0.0f), -0.5f);  // 32/128*2 - 1            (was clipX)
    EXPECT_FLOAT_EQ(H.applyX(1.0f, 0.0f),  0.0f);  // + 32/128*2              (was clipX+clipW)
    EXPECT_FLOAT_EQ(H.applyY(0.0f, 0.0f),  0.5f);  // 1 - 32/128*2            (was clipY)
    EXPECT_FLOAT_EQ(H.applyY(0.0f, 1.0f),  0.0f);  // - 32/128*2 (V-flip)     (was clipY+clipH)
    EXPECT_FLOAT_EQ(H.weight(0.0f, 0.0f),  1.0f);  // affine → w ≡ 1
    EXPECT_EQ(g.tile, 0x0042u);
    EXPECT_EQ(g.paletteRow, 9u);
    EXPECT_EQ(g.flags, packSpriteFlags(true, false, true));  // flipX + priority → 5
    EXPECT_EQ(g.size, (32u << 16) | 32u);
}

TEST(GpuSprite, MakeAppliesScrollAndViewport) {
    Sprite s;
    s.x = 64;
    s.y = 0;
    s.size = SpriteSize::GameBoy8x8;
    // No scroll, square viewport: x at the centre maps to clip 0.
    const Transform a = spriteHomography(makeGpuSprite(s, 0u, 128, 128, 0, 0));
    EXPECT_FLOAT_EQ(a.applyX(0.0f, 0.0f), 0.0f);   // 64/128*2 - 1
    EXPECT_FLOAT_EQ(a.applyY(0.0f, 0.0f), 1.0f);   // 1 - 0/128*2  (top edge)
    // Scrolling right by 64 moves the same sprite a full half-screen left in clip space.
    const Transform b = spriteHomography(makeGpuSprite(s, 0u, 128, 128, 64, 0));
    EXPECT_FLOAT_EQ(b.applyX(0.0f, 0.0f), -1.0f);  // (64-64)/128*2 - 1
}

TEST(GpuSprite, MakeIsConstexpr) {
    constexpr Sprite s{};
    constexpr GpuSprite g = makeGpuSprite(s, 0u, 160, 144, 0, 0);
    static_assert(g.flags == 0u && g.size == ((8u << 16) | 8u));
    EXPECT_EQ(g.size, (8u << 16) | 8u);
}

// ── spritePaletteRow (select → palette-store row) ─────────────────────────────────────

TEST(SpritePaletteRow, ResolvesSelectViaTheSet) {
    const std::array<PaletteId, 3> set{PaletteId{5}, PaletteId{2}, PaletteId{9}};
    const std::span<const PaletteId> s(set);
    EXPECT_EQ(spritePaletteRow(s, 0), 5u);
    EXPECT_EQ(spritePaletteRow(s, 1), 2u);
    EXPECT_EQ(spritePaletteRow(s, 2), 9u);
}

TEST(SpritePaletteRow, OutOfRangeSelectIsRowZero) {
    const std::array<PaletteId, 2> set{PaletteId{4}, PaletteId{6}};
    const std::span<const PaletteId> s(set);
    EXPECT_EQ(spritePaletteRow(s, 2), 0u);    // == size → out of range
    EXPECT_EQ(spritePaletteRow(s, 200), 0u);  // far out of range
}

TEST(SpritePaletteRow, EmptySetIsRowZero) {
    EXPECT_EQ(spritePaletteRow(std::span<const PaletteId>{}, 0), 0u);
    EXPECT_EQ(spritePaletteRow(std::span<const PaletteId>{}, 7), 0u);
}

TEST(SpritePaletteRow, AllowsRepeatedHandles) {
    const std::array<PaletteId, 3> set{PaletteId{8}, PaletteId{8}, PaletteId{1}};
    const std::span<const PaletteId> s(set);
    EXPECT_EQ(spritePaletteRow(s, 0), 8u);
    EXPECT_EQ(spritePaletteRow(s, 1), 8u);
    EXPECT_EQ(spritePaletteRow(s, 2), 1u);
}

// ── SpriteContent variant wiring ──────────────────────────────────────────────────────

TEST(SpriteContent, IsTheSpritesAlternativeOfLayerContent) {
    LayerContent c = SpriteContent{};
    EXPECT_EQ(contentKind(c), LayerContentKind::Sprites);
    LayerContent t = TileContent{};
    EXPECT_EQ(contentKind(t), LayerContentKind::Tiles);
}

}  // namespace gbcpp
