#include "retropp/run_loop.h"

namespace retropp {

void RunLoop::advance() {
    const auto t = clock_.now();

    if (!started_) {            // lazy baseline on the first call — nothing to advance yet
        started_ = true;
        last_ = t;
        if (render_) render_(0.0f);
        return;
    }

    auto frame = t - last_;
    last_ = t;
    if (frame < std::chrono::nanoseconds::zero()) {
        frame = std::chrono::nanoseconds::zero();  // monotonic guard (backwards clock)
    }
    if (frame > kMaxFrameTime) {
        frame = kMaxFrameTime;                     // spiral-of-death clamp (Decision #5)
    }

    accumulator_ += frame;
    while (accumulator_ >= tickPeriod_) {          // fixed-step catch-up (profile's period)
        // Sample once per tick (Decision #10/#13): the latest level as held, the union of levels seen
        // since the last tick as the press source (so a sub-tick tap isn't dropped), and the
        // accumulated analog. Then reset the per-tick accumulators to the current level / cleared
        // relatives so the next window starts fresh and a held button doesn't re-fire its press edge.
        input_.sampleTick(rawInput_, heldUnion_, pendingAnalog_);
        if (tick_) tick_(input_);
        ++tickCount_;
        accumulator_ -= tickPeriod_;
        heldUnion_ = rawInput_;
        pendingAnalog_.clearRelatives();
    }

    const float alpha = static_cast<float>(accumulator_.count())
                      / static_cast<float>(tickPeriod_.count());  // [0, 1)
    if (render_) render_(alpha);
}

void RunLoop::run() {
    running_ = true;
    while (running_) {
        advance();
    }
}

}  // namespace retropp
