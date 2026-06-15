#include <gtest/gtest.h>

#include "retropp/geometry.h"

namespace retropp {
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

// ── fitWindowScale: target window scale clamped to the usable display (ENG-2.C.1) ──

TEST(Geometry, FitWindowScaleUsesTargetWhenItFits) {
    // 4× the GB viewport (640×576) fits comfortably in a 2560×1440 desktop → the target stands.
    EXPECT_EQ(fitWindowScale(kGb, PixelSize{2560, 1440}, 4), 4);
}

TEST(Geometry, FitWindowScaleClampsDownByHeight) {
    // 4× height (576) exceeds a 500-tall usable area → step down to the largest that fits: 3× (432).
    EXPECT_EQ(fitWindowScale(kGb, PixelSize{2560, 500}, 4), 3);
}

TEST(Geometry, FitWindowScaleClampsDownByWidth) {
    // 4× width (640) exceeds a 500-wide usable area → 3× (480) fits.
    EXPECT_EQ(fitWindowScale(kGb, PixelSize{500, 1440}, 4), 3);
}

TEST(Geometry, FitWindowScaleFloorsAtOneForHugeViewport) {
    // A viewport bigger than the whole usable display still yields 1× (never 0); the window opens at
    // the OS limit and the content letterboxes.
    EXPECT_EQ(fitWindowScale(PixelSize{3000, 2000}, PixelSize{2560, 1440}, 4), 1);
}

TEST(Geometry, FitWindowScaleDegenerateReturnsTarget) {
    // Unknown display (non-positive usable) can't clamp → the target (min 1) passes through.
    EXPECT_EQ(fitWindowScale(kGb, PixelSize{0, 0}, 4), 4);
    EXPECT_EQ(fitWindowScale(kGb, PixelSize{0, 0}, 0), 1);  // floor at 1×
}

// Compile-time confirmation the clamp is a constant expression.
static_assert(fitWindowScale(kGb, PixelSize{2560, 1440}, 4) == 4);
static_assert(fitWindowScale(kGb, PixelSize{2560, 500}, 4) == 3);

}  // namespace
}  // namespace retropp
