#pragma once

#include <chrono>

#include "gbcpp/clock.h"

namespace gbcpp::test {

// Deterministic test clock: now() returns whatever time was last set. Drives the run
// loop to an exact elapsed without real wall-clock waits, so tick counts and edges are
// reproducible.
class ManualClock final : public Clock {
public:
    [[nodiscard]] std::chrono::nanoseconds now() const noexcept override { return t_; }

    void set(std::chrono::nanoseconds t) noexcept { t_ = t; }
    void advanceBy(std::chrono::nanoseconds d) noexcept { t_ += d; }

private:
    std::chrono::nanoseconds t_{};
};

}  // namespace gbcpp::test
