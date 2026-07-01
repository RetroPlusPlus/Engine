#include "retropp/frame_timing.h"

namespace retropp {

namespace {
// Per-thread storage. A function-local thread_local so there is no global-init-order dependency.
FrameTiming& slot() noexcept {
    thread_local FrameTiming timing;
    return timing;
}
}  // namespace

void        publishFrameTiming(FrameTiming timing) noexcept { slot() = timing; }
FrameTiming frameTiming() noexcept { return slot(); }

}  // namespace retropp
