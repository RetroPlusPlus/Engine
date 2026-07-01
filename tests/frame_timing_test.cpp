#include "retropp/frame_timing.h"

#include <gtest/gtest.h>

#include <thread>

namespace retropp {
namespace {

TEST(FrameTiming, PublishThenReadReturnsTheSameValue) {
    publishFrameTiming(FrameTiming{0.25f, true});
    const FrameTiming t = frameTiming();
    EXPECT_FLOAT_EQ(t.alpha, 0.25f);
    EXPECT_TRUE(t.tickAdvanced);

    publishFrameTiming(FrameTiming{0.0f, false});
    const FrameTiming u = frameTiming();
    EXPECT_FLOAT_EQ(u.alpha, 0.0f);
    EXPECT_FALSE(u.tickAdvanced);
}

TEST(FrameTiming, DefaultIsZeroAndNoTickOnAThreadThatNeverPublished) {
    // The channel is per-thread, so a thread whose loop has not published yet reads the default — which the
    // renderer composites as the submission verbatim. Read on a fresh thread to observe that default
    // regardless of what this test thread published earlier.
    FrameTiming seen{1.0f, true};
    std::thread([&] { seen = frameTiming(); }).join();
    EXPECT_FLOAT_EQ(seen.alpha, 0.0f);
    EXPECT_FALSE(seen.tickAdvanced);
}

}  // namespace
}  // namespace retropp
