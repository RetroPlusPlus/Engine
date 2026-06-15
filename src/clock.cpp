#include "retropp/clock.h"

namespace retropp {

std::chrono::nanoseconds SteadyClock::now() const noexcept {
    return std::chrono::steady_clock::now().time_since_epoch();
}

}  // namespace retropp
