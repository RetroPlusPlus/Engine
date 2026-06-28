#pragma once

// The pure produce-step, factored out so the production loop AND a synchronous test drive the SAME code
// (the auto_close.h precedent: the decision is pure, the thread/device is integration).
//
// One produce pass tops the output ring back up to its small latency target by stepping the driver in
// WHOLE-FRAME cycle units (the frame quantum — 70'224 cycles for the GB family, see
// TimingProfile::cpuCyclesPerTick), then applies the one-shot-SFX auto-close decision. Because the VM core
// is deterministic, stepping a continuous driver in whole-frame units yields the same samples as any
// other chunking of the same total cycles — the frame quantum is a scheduling choice, not an audio one,
// and it matches the once-per-frame cadence a real sound driver (e.g. hUGEDriver's dosound) runs on.
//
// The produced PCM lands in `ring` via the APU sample callback wired on `vm` (that callback also feeds
// `silenceRun`, the run of consecutive exact-zero output frames). This function only schedules the
// stepping and resolves the lifecycle; it never reads or writes a sample directly.
//
// INTERNAL — under src/audio/, never include/retropp/. Header-only.
#ifndef RETROPP_SRC_AUDIO_PRODUCE_STEP_H
#define RETROPP_SRC_AUDIO_PRODUCE_STEP_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "retropp/audio.h"          // AudioFrame
#include "retropp/audio_library.h"  // AudioType
#include "retropp/vm.h"             // Vm
#include "src/audio/auto_close.h"   // detail::shouldAutoStop
#include "src/audio/ring_buffer.h"  // audio::SpscRingBuffer

namespace retropp::detail {

// The fixed knobs of one produce pass. Identity is the named fields.
struct ProduceConfig {
    std::size_t   targetFrames;           // keep the ring filled to ~this (the latency buffer)
    std::uint64_t cyclesPerFrame;         // the frame quantum — one stepDriver() advances this many cycles
    int           maxStepsPerWake;        // safety cap on steps per pass (fill-from-empty + slack)
    std::size_t   autoStopSilenceFrames;  // a one-shot SFX auto-closes after this many exact-zero frames
    AudioType     currentType;            // Music never auto-closes; Sfx does (the gate in shouldAutoStop)
};

// Run one self-pacing produce pass on `vm`'s currently-running driver. Steps the driver in whole-frame
// units until the ring reaches its target (or the cap is hit — the device drains the ring on its own
// clock, so this self-corrects toward the target run to run), then auto-closes a finished one-shot SFX
// by clearing `playing`. `silenceRun` is read for the auto-close decision and reset on close (it is
// otherwise advanced by the APU callback during stepDriver). `playing` is the cross-thread status flag
// (the main thread reads it via isPlaying()); auto-close stores false into it.
inline void produceFrame(Vm& vm, audio::SpscRingBuffer<AudioFrame>& ring, const ProduceConfig& cfg,
                         std::size_t& silenceRun, std::atomic<bool>& playing) {
    for (int n = 0; n < cfg.maxStepsPerWake && ring.sizeApprox() < cfg.targetFrames; ++n) {
        vm.stepDriver(cfg.cyclesPerFrame);  // the APU pushes ~one frame's worth of samples into the ring
    }
    if (shouldAutoStop(silenceRun, cfg.autoStopSilenceFrames, cfg.currentType)) {
        playing.store(false, std::memory_order_relaxed);
        silenceRun = 0;
    }
}

// One PCM produce pass: copy decoded frames from `buf` (resuming at `cursor`) into `ring` up to the
// latency target or the buffer's end, advancing `cursor`. A PCM system runs no driver — the frames are
// already decoded, so this just hands them to the same ring the device drains. When the buffer is
// exhausted the one-shot is finished and `playing` clears (looping is a future refinement). A ring-full
// push stops this pass; the next pass resumes from `cursor` once the device has drained room.
inline void producePcm(const std::vector<AudioFrame>& buf, std::size_t& cursor,
                       audio::SpscRingBuffer<AudioFrame>& ring, std::size_t targetFrames,
                       std::atomic<bool>& playing) {
    while (cursor < buf.size() && ring.sizeApprox() < targetFrames) {
        if (!ring.push(buf[cursor])) {
            break;  // ring full — the device drains on its own clock; resume next pass
        }
        ++cursor;
    }
    if (cursor >= buf.size()) {
        playing.store(false, std::memory_order_relaxed);  // one-shot exhausted
    }
}

}  // namespace retropp::detail

#endif  // RETROPP_SRC_AUDIO_PRODUCE_STEP_H
