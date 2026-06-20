#pragma once

#include <chrono>

namespace retropp {

// Monotonic time source for the run loop. RunLoop reads time through this interface
// rather than calling a clock directly, so a test can inject a deterministic clock and
// drive the loop tick-by-tick with no real waiting. now() is called once per host frame —
// a virtual call there is negligible, and it keeps RunLoop a plain, inspectable class.
class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual std::chrono::nanoseconds now() const noexcept = 0;
};

// Default production clock, backed by std::chrono::steady_clock (monotonic).
class SteadyClock final : public Clock {
public:
    [[nodiscard]] std::chrono::nanoseconds now() const noexcept override;
};

}  // namespace retropp
