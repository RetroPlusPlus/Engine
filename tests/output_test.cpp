#include <gtest/gtest.h>

#include "retropp/engine_config.h"
#include "retropp/output.h"

namespace retropp {
namespace {

// The faithful default sampling mode is Nearest — crisp integer pixels, byte-for-byte the
// pre-C.1 baseline. A regression that flipped the default to Bilinear would silently soften
// every faithful install.
TEST(Output, DefaultSamplingModeIsNearest) {
    EXPECT_EQ(SamplingMode{}, SamplingMode::Nearest);  // value-initialized = first enumerator
    EXPECT_EQ(EnhancementToggles{}.sampling, SamplingMode::Nearest);
}

// The factory-default presentation: 4× window scale, windowed, nearest sampling. windowScale == 4
// is THE enforced default — the window opens at 4× the viewport (clamped to the display at runtime),
// the size that reads well out of the box. A regression that changed this silently resizes every
// default install's window.
TEST(Output, FactoryEnhancementDefaults) {
    constexpr EnhancementToggles toggles{};
    EXPECT_EQ(toggles.windowScale, 4);   // factory default window scale (clamped to fit the display)
    EXPECT_FALSE(toggles.fullscreen);
    EXPECT_EQ(toggles.sampling, SamplingMode::Nearest);
}

}  // namespace
}  // namespace retropp
