#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace retropp {

// The host-selected timing profile for the run loop (ENG-1 follow-on).
//
// ENG-1 hardcoded the loop cadence as Game Boy constants; the engine targets the 8-/16-bit
// family and original games in that idiom, so timing is a developer-selectable profile — the
// same value-as-data, preset-or-raw posture as PaletteSize. The render loop reads the tick
// PERIOD; the future SM83 VM (ENG-3) reads the optional CPU block (cycle budget + double-speed).
// Identity is the named fields throughout.

// Render tick period in NANOSECONDS — named presets whose underlying value IS the exact period
// in ns (integral and drift-free; a frequency like 59.7275 Hz is fractional and cannot be an
// enum value, but its period 16'742'706 ns is exact). Pass a preset or a raw ns period
// interchangeably: static_cast<TickPeriodNs>(16'700'000). The value is the PERIOD, not a rate —
// hence TickPeriodNs, not "tick rate" (a rate is the fractional reciprocal of what is stored).
enum class TickPeriodNs : std::int64_t {
    GameBoy      = 16'742'706,  // 59.7275 Hz — one real GB frame (70'224 cycles @ 4'194'304 Hz)
    GameBoyColor = 16'742'706,  // identical refresh; double-speed is CPU-only (see CpuTiming)
    Hz60         = 16'666'667,  // a clean 60 Hz for an original game that just wants a round rate
};

// GB-family machine timing for the future SM83 VM (ENG-3 — RNG / audio). OPTIONAL: an original
// game with no CPU model omits it. `doubleSpeedCyclesPerFrame` is the CGB KEY1 per-frame CPU
// budget — the PPU / display refresh is unchanged, so double-speed is a cycle budget, NOT a
// cadence change. Identity is the named fields.
struct CpuTiming {
    std::uint32_t cpuClockHz;
    std::uint32_t cyclesPerFrame;
    std::uint32_t doubleSpeedCyclesPerFrame;
    [[nodiscard]] constexpr bool operator==(const CpuTiming&) const noexcept = default;
};

// The timing bundle the host hands the run loop: a render cadence (required) + an optional CPU-
// timing block. RunLoop schedules on tickPeriod(); the VM (ENG-3) reads cpu. Defaults to the
// Game Boy Color cadence so a default-constructed profile reproduces the ENG-1 behaviour.
//
// The named presets are static members of the profile's own type (TimingProfile::GameBoyColor,
// …): declared in-class, defined inline constexpr just below — the self-type-constant idiom,
// fully usable in constexpr contexts (including the RunLoop default argument). GB-family presets
// fill both fields; the CPU block's double-speed budget is exactly 2× the single-speed cycles.
struct TimingProfile {
    TickPeriodNs             tickPeriodNs = TickPeriodNs::GameBoyColor;  // identity, first member
    std::optional<CpuTiming> cpu{};

    // The tick period as chrono::nanoseconds — what RunLoop's fixed step schedules on.
    [[nodiscard]] constexpr std::chrono::nanoseconds tickPeriod() const noexcept {
        return std::chrono::nanoseconds{static_cast<std::int64_t>(tickPeriodNs)};
    }

    // The CPU cycles that elapse in one render tick — one tick is one frame, so this is the CPU
    // block's per-frame budget (e.g. 70'224 for the Game Boy). It is the natural amount to advance an
    // SM83 VM's free-running divider per tick (see vm.h Vm::advanceClock), so a consumer reads it from
    // the profile rather than hardcoding it. Zero if the profile carries no CPU model.
    [[nodiscard]] constexpr std::uint32_t cpuCyclesPerTick() const noexcept {
        return cpu ? cpu->cyclesPerFrame : 0u;
    }

    // How many render ticks span a wall-clock duration, rounded to nearest — so a consumer schedules
    // "every 2 seconds" as ticksForDuration(std::chrono::seconds{2}) instead of hardcoding a tick
    // count derived from the cadence. Accepts any std::chrono duration (it converts to nanoseconds).
    // Returns the same std::uint64_t the run loop counts ticks in (RunLoop::tickCount()), so a
    // consumer never casts; a non-positive duration yields 0.
    [[nodiscard]] constexpr std::uint64_t ticksForDuration(std::chrono::nanoseconds d) const noexcept {
        const std::int64_t period = static_cast<std::int64_t>(tickPeriodNs);
        if (period <= 0 || d.count() <= 0) {
            return 0;
        }
        return static_cast<std::uint64_t>((d.count() + period / 2) / period);  // round to nearest
    }
    [[nodiscard]] constexpr bool operator==(const TimingProfile&) const noexcept = default;

    static const TimingProfile GameBoy;
    static const TimingProfile GameBoyColor;
};

inline constexpr TimingProfile TimingProfile::GameBoy{
    TickPeriodNs::GameBoy,      CpuTiming{4'194'304, 70'224, 140'448}};
inline constexpr TimingProfile TimingProfile::GameBoyColor{
    TickPeriodNs::GameBoyColor, CpuTiming{4'194'304, 70'224, 140'448}};

}  // namespace retropp
