#include <gtest/gtest.h>

#include "gbcpp/geometry.h"

namespace gbcpp {
namespace {

constexpr PixelSize kGb{160, 144};

TEST(Geometry, ExactIntegerMultipleFillsWithNoBars) {
    // 4× the GB viewport exactly: full coverage, origin at (0,0), no letterbox.
    EXPECT_EQ(integerScaleToFitRect(PixelSize{640, 576}, kGb), (IntRect{0, 0, 640, 576}));
}

TEST(Geometry, OneToOneWhenDrawableEqualsViewport) {
    EXPECT_EQ(integerScaleToFitRect(kGb, kGb), (IntRect{0, 0, 160, 144}));
}

TEST(Geometry, NonExactPicksLargestFittingMultipleAndCentres) {
    // 700×600 fits 4× (640×576); remainder split as symmetric bars (30 px / 12 px).
    EXPECT_EQ(integerScaleToFitRect(PixelSize{700, 600}, kGb), (IntRect{30, 12, 640, 576}));
}

TEST(Geometry, BindingDimensionLimitsTheScale) {
    // Width allows 2× (320/160), height allows 4× (600/144) → scale is the min, 2×.
    EXPECT_EQ(integerScaleToFitRect(PixelSize{320, 600}, kGb), (IntRect{0, 156, 320, 288}));
}

TEST(Geometry, SmallerThanViewportClampsToOneXAndOverflows) {
    // Below 1× the content is shown at 1× and the centred rect goes negative rather
    // than collapsing — the window is simply smaller than a single GB frame.
    EXPECT_EQ(integerScaleToFitRect(PixelSize{100, 100}, kGb), (IntRect{-30, -22, 160, 144}));
}

TEST(Geometry, DegenerateSizesYieldEmptyRect) {
    EXPECT_EQ(integerScaleToFitRect(PixelSize{0, 0}, kGb), (IntRect{}));
    EXPECT_EQ(integerScaleToFitRect(PixelSize{640, 576}, PixelSize{0, 144}), (IntRect{}));
    EXPECT_EQ(integerScaleToFitRect(PixelSize{-10, -10}, kGb), (IntRect{}));
}

// Compile-time confirmation the helper is usable in constant expressions.
static_assert(integerScaleToFitRect(PixelSize{640, 576}, kGb) == IntRect{0, 0, 640, 576});

}  // namespace
}  // namespace gbcpp
