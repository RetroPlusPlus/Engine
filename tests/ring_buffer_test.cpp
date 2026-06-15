// ENG-4.A — the SPSC PCM ring buffer (src/audio/ring_buffer.h). The one cross-thread hand-off in the
// audio chain; these are single-threaded functional tests of its index/wrap/full/empty accounting (the
// memory-ordering correctness is a property of the atomics, exercised live by the audio thread).
#include "src/audio/ring_buffer.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "gbcpp/audio.h"  // AudioFrame (for the PCM-frame case)

namespace gbcpp::audio {
namespace {

TEST(RingBuffer, CapacityRoundsUpToPowerOfTwoMinusOne) {
    // minCapacity 3216 → next power of two ≥ 3217 is 4096; one slot reserved → 4095 usable.
    SpscRingBuffer<int> ring(3216);
    EXPECT_EQ(ring.capacity(), 4095u);
    // An exact power of two still rounds (the +1 for the reserved slot pushes it up).
    SpscRingBuffer<int> pow2(1024);
    EXPECT_EQ(pow2.capacity(), 2047u);
}

TEST(RingBuffer, PushPopSingleRoundTrips) {
    SpscRingBuffer<int> ring(8);
    EXPECT_EQ(ring.sizeApprox(), 0u);
    EXPECT_TRUE(ring.push(42));
    EXPECT_TRUE(ring.push(7));
    EXPECT_EQ(ring.sizeApprox(), 2u);

    std::array<int, 4> out{};
    EXPECT_EQ(ring.pop(std::span<int>(out.data(), 4)), 2u);
    EXPECT_EQ(out[0], 42);
    EXPECT_EQ(out[1], 7);
    EXPECT_EQ(ring.sizeApprox(), 0u);
}

TEST(RingBuffer, EmptyPopReturnsZero) {
    SpscRingBuffer<int> ring(8);
    std::array<int, 4> out{};
    EXPECT_EQ(ring.pop(std::span<int>(out.data(), 4)), 0u);
}

TEST(RingBuffer, FullPushIsDroppedNotBlocked) {
    SpscRingBuffer<int> ring(3);  // → capacity 3 (next pow2 of 4 = 4, minus 1)
    EXPECT_EQ(ring.capacity(), 3u);
    EXPECT_TRUE(ring.push(1));
    EXPECT_TRUE(ring.push(2));
    EXPECT_TRUE(ring.push(3));
    EXPECT_FALSE(ring.push(4));  // full — dropped, not blocked
    EXPECT_EQ(ring.sizeApprox(), 3u);
}

TEST(RingBuffer, SpanPushReturnsCountAndStopsAtCapacity) {
    SpscRingBuffer<int> ring(3);  // capacity 3
    const std::array<int, 5> in{1, 2, 3, 4, 5};
    EXPECT_EQ(ring.push(std::span<const int>(in.data(), in.size())), 3u);  // only 3 fit
    EXPECT_EQ(ring.sizeApprox(), 3u);

    std::array<int, 5> out{};
    EXPECT_EQ(ring.pop(std::span<int>(out.data(), out.size())), 3u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[1], 2);
    EXPECT_EQ(out[2], 3);
}

TEST(RingBuffer, WrapsAroundCorrectly) {
    SpscRingBuffer<int> ring(3);  // capacity 3, internal size 4
    // Fill, drain, refill across the wrap boundary many times — the indices must wrap cleanly.
    int next = 0;
    std::vector<int> seen;
    for (int round = 0; round < 10; ++round) {
        EXPECT_TRUE(ring.push(next++));
        EXPECT_TRUE(ring.push(next++));
        std::array<int, 2> out{};
        EXPECT_EQ(ring.pop(std::span<int>(out.data(), 2)), 2u);
        seen.push_back(out[0]);
        seen.push_back(out[1]);
    }
    // Everything pushed comes back out in order, undamaged by the wrap.
    for (std::size_t i = 0; i < seen.size(); ++i) {
        EXPECT_EQ(seen[i], static_cast<int>(i));
    }
}

TEST(RingBuffer, CarriesAudioFrames) {
    SpscRingBuffer<AudioFrame> ring(8);
    EXPECT_TRUE(ring.push(AudioFrame{100, -200}));
    std::array<AudioFrame, 1> out{};
    EXPECT_EQ(ring.pop(std::span<AudioFrame>(out.data(), 1)), 1u);
    EXPECT_EQ(out[0].left, 100);
    EXPECT_EQ(out[0].right, -200);
}

}  // namespace
}  // namespace gbcpp::audio
