#include "gbcpp/clock.h"

namespace gbcpp {

std::chrono::nanoseconds SteadyClock::now() const noexcept {
    return std::chrono::steady_clock::now().time_since_epoch();
}

}  // namespace gbcpp
