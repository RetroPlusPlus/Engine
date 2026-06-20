#pragma once

// ENG-4.A — the engine's audio-output boundary.
//
// This is the public seam between the engine's PCM production and the host audio device. It is
// SYSTEM-AGNOSTIC, like the rest of the engine: a frame of PCM is a frame of PCM whatever console's
// sound chip produced it. The v1 producer is the Game Boy APU (chiptune backend), but the surface
// names no console — a future SNES (SPC700 + S-DSP) or Genesis (Z80 + YM2612) audio backend produces
// into the SAME AudioFrame / AudioSink, exactly as the VM host (retropp/vm.h) generalizes over VMPlatform
// backends. The console-specific synthesis lives behind the per-system VM backend; what crosses HERE is
// only finished stereo PCM.
//
// The audio production thread (ENG-4.D.1) pushes finished frames; an AudioSink drains them to the
// speakers on its own audio thread. The two sides meet through one single-producer / single-consumer
// hand-off (see src/audio/ring_buffer.h); a second SPSC hand-off (src/audio/cue_queue.h) carries the
// game's play()/stop() cues to that production thread. The sim/render loop itself stays single-threaded.
//
// What this is: just the sink + the PCM frame format the whole chain speaks. The cue surface a game
// drives is the AudioSystem (play/stop) over the AudioLibrary catalog (ENG-4.B, shipped); the dual
// live-synthesis / audio-pack backend selector is ENG-4.C (planned).
//
// No machine or SDL type crosses this boundary — a frame is two int16 samples, a sink is open/pull/
// close. The production sink is SdlAudioSink (owned by SdlPlatform); tests drive a capture sink.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace retropp {

// One stereo PCM sample: signed 16-bit left + right — the format every console's sound chip resamples
// to and SDL audio speaks. Trivially copyable so it rides the lock-free ring buffer.
struct AudioFrame {
    std::int16_t left = 0;
    std::int16_t right = 0;
};

// The one rate the whole chain runs at. The active backend's synthesizer (the Game Boy APU in v1) is
// set to output at EXACTLY the sink rate, so it resamples internally — the three-rate-alignment
// problem collapses to one number (see the ENG-4 partition §3). 48 kHz stereo is the faithful default.
inline constexpr unsigned kAudioSampleRate = 48'000;
inline constexpr int      kAudioChannels   = 2;

// The consumer-side pull the sink invokes on its audio thread: fill up to `out.size()` frames, return
// how many were actually written. The sink silence-fills the remainder (a short return = underflow).
// The engine side supplies this (it pops the chain's ring); the sink never sees the ring directly, so
// a future multi-instance mixer (ENG-4.D) can replace the pull without touching the sink.
using AudioPullFn = std::function<std::size_t(std::span<AudioFrame> out)>;

// The host audio device boundary. open with start(), which begins draining via the supplied pull on
// the implementation's audio thread; stop() ends draining and releases the device. A sink is a
// platform resource — the production one wraps an SDL audio stream; a headless test sink captures.
// Implementations: SdlAudioSink (src/sdl_platform.cpp), test::CaptureAudioSink (tests/mock_platform.h).
class AudioSink {
public:
    virtual ~AudioSink() = default;

    // Open the device at `rate` Hz / `channels` and begin pulling frames from `pull` on the audio
    // thread. Idempotent-safe: a second start() restarts with the new pull. Throws on device failure.
    virtual void start(unsigned rate, int channels, AudioPullFn pull) = 0;

    // Stop draining and release the device. Safe to call when not started. After stop(), the pull is
    // no longer invoked, so the engine may tear down whatever the pull captured.
    virtual void stop() = 0;
};

}  // namespace retropp
