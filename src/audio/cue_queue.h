#pragma once

// The main→production cue channel.
//
// Production runs on a dedicated thread, so the game's play()/stop() calls do not touch the VM directly —
// the VM lives on the production thread. Instead they marshal a tiny command onto this lock-free SPSC
// queue, which the production thread drains and applies (it owns the VM, so the materialize / startDriver
// work happens there). This keeps the audio data path mutex-free: the same SpscRingBuffer that carries
// PCM frames carries these commands. The condvar in audio_system.cpp governs only the production thread's
// wait/wake, never this data.
//
// SPSC contract: exactly the main (game) thread pushes (play()/stop()); exactly the production thread
// pops (its loop). A second consumer or producer is undefined, same as the PCM ring.
//
// INTERNAL — under src/audio/, never include/retropp/. Header-only.
#ifndef RETROPP_SRC_AUDIO_CUE_QUEUE_H
#define RETROPP_SRC_AUDIO_CUE_QUEUE_H

#include "retropp/audio_library.h"  // AudioId
#include "src/audio/ring_buffer.h"

namespace retropp::audio {

// A cue marshaled from the game thread to the production thread. Trivially copyable so it rides the
// existing SpscRingBuffer<T> unchanged. `id` is meaningful only for Play (Stop ignores it).
struct AudioCommand {
    enum class Op { Play, Stop };
    Op      op;
    AudioId id;
};

// The main→production cue channel: the same lock-free ring the PCM path uses, instantiated for commands.
using CueQueue = SpscRingBuffer<AudioCommand>;

}  // namespace retropp::audio

#endif  // RETROPP_SRC_AUDIO_CUE_QUEUE_H
