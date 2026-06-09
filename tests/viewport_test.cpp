#include <gtest/gtest.h>

#include "gbcpp/viewport.h"

namespace gbcpp {
namespace {

TEST(Viewport, DefaultsToGameBoyResolution) {
    constexpr ViewportConfig config{};
    EXPECT_EQ(config.width, 160);
    EXPECT_EQ(config.height, 144);
}

TEST(Viewport, OverrideIsReportedBack) {
    constexpr ViewportConfig wide{320, 144};
    EXPECT_EQ(wide.width, 320);
    EXPECT_EQ(wide.height, 144);
}

}  // namespace
}  // namespace gbcpp
