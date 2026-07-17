// Device-free coverage for the native-window-chrome suppression seam
// (Platform::suppressNativeWindowChrome). Driven through the abstract Platform interface against
// MockPlatform — no live window — so the contract (default-not-suppressed, toggles, and ORTHOGONAL to
// fullscreen) is pinned headlessly. The production SdlPlatform realizes the runtime toggle with
// SDL_SetWindowBordered and the from-start (no-flash) path with the SDL_WINDOW_BORDERLESS creation
// flag; those live paths are exercised by running a windowed demo, not here.

#include <gtest/gtest.h>

#include "retropp/platform.h"

#include "mock_platform.h"

namespace retropp {
namespace {

using test::MockPlatform;

TEST(WindowChrome, DefaultsToNotSuppressed) {
    MockPlatform platform{1};
    Platform& seam = platform;  // exercise through the abstract interface
    EXPECT_FALSE(seam.suppressNativeWindowChrome());
}

TEST(WindowChrome, SeamTogglesTrackedState) {
    MockPlatform platform{1};
    Platform& seam = platform;

    seam.suppressNativeWindowChrome(true);
    EXPECT_TRUE(seam.suppressNativeWindowChrome());
    seam.suppressNativeWindowChrome(false);
    EXPECT_FALSE(seam.suppressNativeWindowChrome());
}

// Chrome suppression and fullscreen are independent window knobs — toggling one never moves the other.
TEST(WindowChrome, IndependentOfFullscreen) {
    MockPlatform platform{1};
    Platform& seam = platform;

    seam.suppressNativeWindowChrome(true);
    seam.setFullscreen(true);
    EXPECT_TRUE(seam.suppressNativeWindowChrome());  // going fullscreen did not change chrome state
    EXPECT_TRUE(seam.isFullscreen());

    seam.setFullscreen(false);
    EXPECT_TRUE(seam.suppressNativeWindowChrome());  // leaving fullscreen did not restore chrome
    EXPECT_FALSE(seam.isFullscreen());

    seam.suppressNativeWindowChrome(false);
    EXPECT_FALSE(seam.suppressNativeWindowChrome());
    EXPECT_FALSE(seam.isFullscreen());               // restoring chrome did not touch fullscreen
}

}  // namespace
}  // namespace retropp
