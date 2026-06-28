#pragma once

// The internal test seam for driving AudioSystem production SYNCHRONOUSLY, with the production thread
// suppressed.
//
// Production normally runs autonomously on its own thread, so the game just cues with play()/stop() and
// never steps anything — there is deliberately no public stepping method. But device-free tests need
// DETERMINISTIC production: drive one pass, assert the exact buffer level. This seam gives them that
// without polluting the game-facing surface — `makeManual` builds an AudioSystem whose production thread
// is NOT started (play()/stop() apply their cue inline instead of marshaling), and `step` /
// `stepDriverRaw` drive it by hand on the calling thread. Reachable only through this internal header (a
// friend of AudioSystem); never in include/retropp/.
//
// INTERNAL — under src/audio/. Definitions live in audio_system.cpp, where AudioSystem::Impl is complete.
#ifndef RETROPP_SRC_AUDIO_AUDIO_SYSTEM_TESTING_H
#define RETROPP_SRC_AUDIO_AUDIO_SYSTEM_TESTING_H

#include <cstdint>
#include <memory>

#include "retropp/audio.h"          // AudioSink, kAudioSampleRate
#include "retropp/audio_system.h"
#include "retropp/timing.h"         // TimingProfile
#include "retropp/vm.h"             // VMPlatform

namespace retropp::detail {

// Friend-of-AudioSystem accessor: builds and drives a manual-mode (thread-suppressed) AudioSystem so
// tests get deterministic, synchronous production they can step and assert against by hand.
struct AudioSystemTestAccess {
    // Build a `kind` AudioSystem (borrowing `sink`) whose production thread is NOT started. In this mode
    // play()/stop() apply their cue inline on the calling thread, so isPlaying() reflects a cue
    // immediately; the caller produces frames via step()/stepDriverRaw().
    static std::unique_ptr<AudioSystem> makeManual(AudioKind     kind,
                                                   AudioSink&    sink,
                                                   VMPlatform    platform   = VMPlatform::GameBoyColor,
                                                   TimingProfile timing     = TimingProfile::GameBoyColor,
                                                   unsigned      sampleRate = kAudioSampleRate);

    // One production iteration on the calling thread: drain any pending cues, then, if playing, run one
    // frame-quantized refill-to-target produce pass and the auto-close check. The deterministic,
    // synchronous path the device-free AudioSystem tests drive.
    static void step(AudioSystem& sys);

    // Lower-level: drain any pending cues, then, if playing, advance the running driver by exactly
    // `cycles` CPU cycles (one stepDriver call), returning the cycles actually run (0 if not playing).
    // Bypasses refill-to-target so a test can capture a driver's output at a chosen cycle granularity
    // (the golden gate compares two granularities over the same total cycles, asserting identical PCM).
    static std::uint64_t stepDriverRaw(AudioSystem& sys, std::uint64_t cycles);
};

}  // namespace retropp::detail

#endif  // RETROPP_SRC_AUDIO_AUDIO_SYSTEM_TESTING_H
