#include "gbcpp/timing.h"

#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

namespace gbcpp {

// TickPeriodNs presets: the enumerator VALUE is the exact tick period in nanoseconds, so a
// preset and a raw ns period are interchangeable.

TEST(Timing, TickPeriodPresetValuesAreExactPeriods) {
    EXPECT_EQ(static_cast<std::int64_t>(TickPeriodNs::GameBoy), 16'742'706);
    EXPECT_EQ(static_cast<std::int64_t>(TickPeriodNs::GameBoyColor), 16'742'706);
    EXPECT_EQ(static_cast<std::int64_t>(TickPeriodNs::Hz60), 16'666'667);
    // GB and GBC share the refresh (double-speed is CPU-only).
    EXPECT_EQ(TickPeriodNs::GameBoy, TickPeriodNs::GameBoyColor);
}

TEST(Timing, RawNanosecondPeriodRoundTrips) {
    constexpr auto raw = static_cast<TickPeriodNs>(16'700'000);
    EXPECT_EQ(static_cast<std::int64_t>(raw), 16'700'000);
}

// TimingProfile carries a cadence + an optional CPU block; tickPeriod() is the chrono view.

TEST(Timing, ProfileTickPeriodIsChronoView) {
    constexpr TimingProfile p{TickPeriodNs::GameBoyColor};
    EXPECT_EQ(p.tickPeriod(), std::chrono::nanoseconds{16'742'706});
    static_assert(p.tickPeriod().count() == 16'742'706, "constexpr-usable");
}

TEST(Timing, DefaultProfileIsGameBoyColorCadence) {
    constexpr TimingProfile def{};
    EXPECT_EQ(def.tickPeriodNs, TickPeriodNs::GameBoyColor);
    EXPECT_EQ(def.tickPeriod(), TimingProfile::GameBoyColor.tickPeriod());
}

TEST(Timing, GameBoyPresetsCarryCpuBlock) {
    ASSERT_TRUE(TimingProfile::GameBoyColor.cpu.has_value());
    const CpuTiming& cpu = *TimingProfile::GameBoyColor.cpu;
    EXPECT_EQ(cpu.cpuClockHz, 4'194'304u);
    EXPECT_EQ(cpu.cyclesPerFrame, 70'224u);
    EXPECT_EQ(cpu.doubleSpeedCyclesPerFrame, 140'448u);
    // Double-speed is exactly twice the single-speed budget (the KEY1 cycle budget).
    EXPECT_EQ(cpu.doubleSpeedCyclesPerFrame, 2u * cpu.cyclesPerFrame);
}

TEST(Timing, PresetsAreUsableInConstantExpressions) {
    // The self-type-constant idiom is constexpr-usable (default-arg + static_assert reach).
    static_assert(TimingProfile::GameBoy.tickPeriodNs == TickPeriodNs::GameBoy);
    static_assert(TimingProfile::GameBoyColor.cpu->doubleSpeedCyclesPerFrame == 140'448);
    SUCCEED();
}

TEST(Timing, OriginalGameNeedsNoCpuBlock) {
    // A from-scratch game sets a cadence and omits the CPU model — a valid, complete profile.
    constexpr TimingProfile original{TickPeriodNs::Hz60};
    EXPECT_FALSE(original.cpu.has_value());
    EXPECT_EQ(original.tickPeriod(), std::chrono::nanoseconds{16'666'667});
}

}  // namespace gbcpp
