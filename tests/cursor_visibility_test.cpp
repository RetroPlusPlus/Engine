// Device-free coverage for the host-OS cursor-visibility seam (Platform::setCursorVisible /
// cursorVisible). Driven through the abstract Platform interface against MockPlatform — no live window
// or device — so the contract (default-visible, toggles, and ORTHOGONAL to pointer capture) is pinned
// headlessly. The production SdlPlatform realizes it with SDL_ShowCursor/SDL_HideCursor; that live path
// is exercised by the Bongusoid example, not here.

#include <gtest/gtest.h>

#include "retropp/platform.h"

#include "mock_platform.h"

namespace retropp {
namespace {

using test::MockPlatform;

TEST(CursorVisibility, DefaultsToVisible) {
    MockPlatform platform{1};
    Platform& seam = platform;  // exercise through the abstract interface
    EXPECT_TRUE(seam.cursorVisible());
}

TEST(CursorVisibility, SeamTogglesTrackedState) {
    MockPlatform platform{1};
    Platform& seam = platform;

    seam.setCursorVisible(false);
    EXPECT_FALSE(seam.cursorVisible());
    seam.setCursorVisible(true);
    EXPECT_TRUE(seam.cursorVisible());
}

// The headline contract: cursor visibility and pointer capture are independent knobs. Toggling one
// never moves the other — a game can hide the OS cursor without capturing (Bongusoid: absolute cursor,
// no arrow), or capture without touching visibility.
TEST(CursorVisibility, IndependentOfPointerCapture) {
    MockPlatform platform{1};
    Platform& seam = platform;

    seam.setCursorVisible(false);
    seam.setPointerCaptured(true);
    EXPECT_FALSE(seam.cursorVisible());   // capture did not change visibility
    EXPECT_TRUE(seam.pointerCaptured());

    seam.setCursorVisible(true);
    EXPECT_TRUE(seam.cursorVisible());
    EXPECT_TRUE(seam.pointerCaptured());  // showing the cursor did not release capture

    seam.setPointerCaptured(false);
    EXPECT_TRUE(seam.cursorVisible());    // releasing capture did not change visibility
    EXPECT_FALSE(seam.pointerCaptured());
}

}  // namespace
}  // namespace retropp
