#pragma once

#include <chrono>

namespace retropp {

// Monotonic time source for the run loop. A virtual now() called once per host
// frame is free in practice and keeps RunLoop a plain, inspectable class (no
// template, no PIMPL — ENG-0 Issue 2). Tests inject a deterministic clock to drive
// time without real wall-clock waits.
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
