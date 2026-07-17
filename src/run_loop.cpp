#include "retropp/run_loop.h"

#include "retropp/frame_timing.h"

namespace retropp {

void RunLoop::advance() {
    if (exitResolved_) return;  // a guard already resolved the exit to Proceed — pull no further ticks

    const auto t = clock_.now();

    if (!started_) {            // lazy baseline on the first call — nothing to advance yet
        started_ = true;
        last_ = t;
        publishFrameTiming(FrameTiming{0.0f, false});  // no tick yet → renderer composites verbatim
        if (render_) render_(0.0f);
        resolveExitAtBoundary();  // an exit requested before the first frame resolves here, not a frame late
        return;
    }

    auto frame = t - last_;
    last_ = t;
    if (frame < std::chrono::nanoseconds::zero()) {
        frame = std::chrono::nanoseconds::zero();  // monotonic guard (backwards clock)
    }
    if (frame > kMaxFrameTime) {
        frame = kMaxFrameTime;                     // spiral-of-death clamp
    }

    accumulator_ += frame;
    bool tickAdvanced = false;                     // did a sim tick commit this iteration?
    while (accumulator_ >= tickPeriod_) {          // fixed-step catch-up (profile's period)
        // Sample once per tick: the latest level as held, the union of levels seen since the last
        // tick as the press source (so a sub-tick tap isn't dropped), and the accumulated analog —
        // per player slot. Then reset the per-tick accumulators to the current level / cleared
        // relatives so the next window starts fresh and a held action doesn't re-fire its press edge.
        InputSample tickSample = latest_;
        for (int i = 0; i < kMaxPlayers; ++i) {
            tickSample.players[static_cast<std::size_t>(i)].analog =
                pendingAnalog_[static_cast<std::size_t>(i)];
        }
        input_.sampleTick(tickSample, heldUnion_);
        if (tick_) tick_(input_);
        ++tickCount_;
        accumulator_ -= tickPeriod_;
        for (int i = 0; i < kMaxPlayers; ++i) {
            heldUnion_[static_cast<std::size_t>(i)] =
                latest_.players[static_cast<std::size_t>(i)].held;
            pendingAnalog_[static_cast<std::size_t>(i)].clearRelatives();
        }
        tickAdvanced = true;
    }

    const float alpha = static_cast<float>(accumulator_.count())
                      / static_cast<float>(tickPeriod_.count());  // [0, 1)
    // Publish the sub-tick factor + the tick signal for the renderer's per-id interpolation before handing
    // off to the render callback (which reaches the renderer one call away, sharing no reference).
    publishFrameTiming(FrameTiming{alpha, tickAdvanced});
    if (render_) render_(alpha);

    resolveExitAtBoundary();
}

void RunLoop::resolveExitAtBoundary() {
    // Resolve a pending exit at the frame boundary — once per advance() while pending, never mid-tick.
    // The completed tick batch means sim state is coherent when the guard reads it (a resume snapshot /
    // save). No guard registered → Proceed immediately (the zero-ceremony quit).
    if (!exitPending_) return;
    const ExitVerdict verdict = exitGuard_ ? exitGuard_() : ExitVerdict::Proceed;
    switch (verdict) {
        case ExitVerdict::Proceed:
            exitPending_  = false;
            exitResolved_ = true;   // terminal — advance() early-returns hereafter
            running_      = false;  // ends RunLoop::run()
            break;
        case ExitVerdict::NotYet:
            break;                  // keep pending; the sim keeps advancing, ask again next boundary
        case ExitVerdict::Veto:
            exitPending_  = false;  // abandon the exit; the host clears any OS quit latch
            break;
    }
}

void RunLoop::run() {
    running_ = true;
    while (running_) {
        advance();
    }
}

}  // namespace retropp
