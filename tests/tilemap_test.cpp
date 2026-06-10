#include "gbcpp/draw_state.h"

#include <gtest/gtest.h>

namespace gbcpp {

// sampleTilemap mirrors the tile fragment shader's per-pixel mapping: an output pixel + the
// layer scroll → the wrapped tile cell + the within-tile offset. tilePx defaults to 8 (the
// GB tile size). A 4×4-tile map is 32×32 px.

TEST(Tilemap, ZeroScrollIdentity) {
    const auto s = sampleTilemap(0, 0, LayerScroll{0, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 0);
    EXPECT_EQ(s.tileY, 0);
    EXPECT_EQ(s.pixelX, 0);
    EXPECT_EQ(s.pixelY, 0);
}

TEST(Tilemap, WithinTileOffsetNoBoundaryCross) {
    const auto s = sampleTilemap(3, 5, LayerScroll{0, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 0);
    EXPECT_EQ(s.tileY, 0);
    EXPECT_EQ(s.pixelX, 3);
    EXPECT_EQ(s.pixelY, 5);
}

TEST(Tilemap, CrossTileBoundary) {
    const auto s = sampleTilemap(8, 9, LayerScroll{0, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 1);
    EXPECT_EQ(s.pixelX, 0);
    EXPECT_EQ(s.tileY, 1);
    EXPECT_EQ(s.pixelY, 1);
}

TEST(Tilemap, PositiveScroll) {
    // world x = 0 + 10 = 10 → tile 1, within-tile pixel 2.
    const auto s = sampleTilemap(0, 0, LayerScroll{10, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 1);
    EXPECT_EQ(s.pixelX, 2);
    EXPECT_EQ(s.tileY, 0);
    EXPECT_EQ(s.pixelY, 0);
}

TEST(Tilemap, NegativeScrollWrapsAndOffsetsCorrectly) {
    // world x = -1 → floorDiv(-1,8) = -1 → wrap floorMod(-1,4) = tile 3; pixel floorMod(-1,8) = 7.
    const auto s = sampleTilemap(0, 0, LayerScroll{-1, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 3);
    EXPECT_EQ(s.pixelX, 7);
}

TEST(Tilemap, ScrollOneFullMapWrapsToOrigin) {
    // map width = 4 tiles * 8 px = 32 px; world x = 32 → tile floorMod(4,4) = 0; pixel 0.
    const auto s = sampleTilemap(0, 0, LayerScroll{32, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 0);
    EXPECT_EQ(s.pixelX, 0);
}

TEST(Tilemap, VerticalAxisWrapsIndependently) {
    // world y = -8 → tile floorDiv(-8,8) = -1 → floorMod(-1,4) = 3; pixel 0. X axis untouched.
    const auto s = sampleTilemap(0, 0, LayerScroll{0, -8}, 4, 4);
    EXPECT_EQ(s.tileX, 0);
    EXPECT_EQ(s.tileY, 3);
    EXPECT_EQ(s.pixelY, 0);
}

TEST(Tilemap, NonSquareMapWrapsPerAxisDimension) {
    // 2 tiles wide (16 px), 4 tiles tall (32 px). world x = 17 → tile floorMod(2,2)=0, pixel 1.
    const auto s = sampleTilemap(1, 0, LayerScroll{16, 0}, 2, 4);
    EXPECT_EQ(s.tileX, 0);
    EXPECT_EQ(s.pixelX, 1);
}

TEST(Tilemap, ConstexprEvaluable) {
    // The mapping is usable in constant evaluation (it must match the shader exactly).
    constexpr auto s = sampleTilemap(0, 0, LayerScroll{-1, 0}, 4, 4);
    static_assert(s.tileX == 3, "negative-scroll wrap must be constexpr-correct");
    static_assert(s.pixelX == 7);
    EXPECT_EQ(s.tileX, 3);
    EXPECT_EQ(s.pixelX, 7);
}

}  // namespace gbcpp
