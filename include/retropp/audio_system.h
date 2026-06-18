#pragma once

// ENG-4.A — the AudioSystem: a game's developer-facing audio engine.
//
// A game configures and drives audio HERE, in audio terms: it registers its audio (a sound-driver
// .asm now; an audio file when ENG-4.C lands) tagged Music or Sfx, then cues it by handle whenever it
// likes. It never touches the VM that actually makes the sound — the AudioSystem OWNS that VM, hosts
// the driver at the hardware CPU clock, enables the console's sound chip, and steps it each tick, all
// internally. What's hidden is the VM plumbing (Vm / Routine / throttle / register bindings); what's
// EXPOSED is the audio: registration and cues. Registering is the developer's job — it is how a game
// configures its sound.
//
// FREELY INSTANTIABLE, like a Vm — no singleton. A game runs as MANY AudioSystems as it wants: a
// chiptune one here, a PCM-playback one (ENG-4.C) there, several at once. Each owns its own resources
// and drains to its own AudioSink output stream; the OS mixes the streams. This is the same
// generalized, no-ceiling posture as VMPlatform / ViewportResolution everywhere in the engine.
//
// SYSTEM-AGNOSTIC: the console is a VMPlatform (GameBoy / GameBoyColor in v1; SNES / Genesis as
// backends land). The console picked at construction decides EVERYTHING console-specific, including the
// ISA a registered .asm is assembled in — SM83 for the Game Boy family (the engine's own assembler),
// the per-console assembler for others. The game writes audio source in its console's assembly; it
// never selects an assembler.
//
// pimpl'd so this public header pulls no VM or ring-buffer type.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "retropp/audio.h"
#include "retropp/audio_library.h"  // AudioId, AudioType, AudioKind — the audio vocabulary the catalog owns
#include "retropp/timing.h"
#include "retropp/vm.h"  // VMPlatform

namespace retropp {

// AudioId / AudioType / AudioKind live in retropp/audio_library.h (the audio vocabulary the single
// AudioLibrary owns); this header consumes them. Registering on an AudioSystem forwards to
// AudioLibrary::instance(); an AudioId's lifetime is the library's (the whole program), not the system's.

class AudioSystem {
public:
    // BORROW a sink. Create a chiptune audio system for `platform` (GameBoy / GameBoyColor in v1),
    // draining produced PCM to `sink`, which the system does NOT own — `sink` must outlive the
    // AudioSystem. Owns the VM that will host the game's sound driver — the game never sees it.
    // `timing` supplies the per-tick CPU cycle budget (it must carry a CPU block — the GB-family
    // presets do); `sampleRate` is both the sound chip's output rate and the rate `sink` is opened at.
    // Defaults reproduce the faithful Game Boy Color baseline. Prefer the sink-less ctor below for the
    // common case; this borrow form is for tests (a capture sink) and pre-owned custom sinks.
    explicit AudioSystem(AudioSink& sink,
                         VMPlatform platform = VMPlatform::GameBoyColor,
                         TimingProfile timing = TimingProfile::GameBoyColor,
                         unsigned sampleRate = kAudioSampleRate);

    // OWN a sink. As above, but the system TAKES OWNERSHIP of `sink` and ties it to its own lifetime —
    // no caller-side keep-alive. This is the custom-sink path ("hand me a sink, you keep nothing": a
    // network sink, a file-capture sink, …) and the device-free test seam for the ownership machinery
    // (pass a unique_ptr<CaptureAudioSink>). The default ctor below delegates here.
    explicit AudioSystem(std::unique_ptr<AudioSink> sink,
                         VMPlatform platform = VMPlatform::GameBoyColor,
                         TimingProfile timing = TimingProfile::GameBoyColor,
                         unsigned sampleRate = kAudioSampleRate);

    // MAKE YOUR OWN sink — the zero-boilerplate default. Owns an internally-constructed production
    // SdlAudioSink, so a bare `AudioSystem music;` just works: no sink to declare, no lifetime to
    // manage. Assumes a Platform exists (the auto-created SdlAudioSink needs SDL_INIT_AUDIO, which
    // SdlPlatform's constructor performs) — true for any real game, and the same assumption the manual
    // SdlAudioSink path already makes. Headless / test contexts use a borrowed or owned CaptureAudioSink
    // (the two ctors above) instead, which open no device.
    explicit AudioSystem(VMPlatform platform = VMPlatform::GameBoyColor,
                         TimingProfile timing = TimingProfile::GameBoyColor,
                         unsigned sampleRate = kAudioSampleRate);
    ~AudioSystem();

    AudioSystem(const AudioSystem&)            = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // REGISTRATION LIVES ON AudioLibrary, NOT HERE. An AudioSystem does not register audio — it CUES
    // already-registered audio by handle. A game registers ONCE on the single catalog —
    // `AudioLibrary::instance().uploadAudio(bytes, type, isa)` (raw) /
    // `AudioLibrary::instance().registerAudio("song.asm", type, isa, policy)` (sugar) — and the resulting
    // AudioId plays on ANY AudioSystem whose VM ISA matches the one selected at registration (play()
    // throws otherwise). Console-specific catalogue helpers (the Game Boy diagnostic tone, a future
    // hUGEDriver adapter) live in that console's preset namespace (retropp/gb_audio.h) and register on
    // the library too — none are methods here, so nothing console-specific leaks into this surface.

    // Cue a registered audio: begin producing it from the next tick. In v1 (single instance) starting
    // one preempts any other currently playing — natural channel-stealing, byte-faithful to the
    // original; ENG-4.D routes Music / Sfx to separate instances so they coexist.
    void play(AudioId id);

    // Stop producing (the output drains to silence). Registered audio stays registered — play() again
    // to resume cueing.
    void stop();

    // Advance the audio one sim tick: top the output buffer back up to its small latency target (the
    // device drains it on its own clock, so production tracks the actual buffer level — this is what
    // keeps the stream from starving into crackle or backing up into lag). Call once per sim tick from
    // the game loop. A no-op while nothing is playing.
    void tick();

    // ── Diagnostics (tests / dev) ────────────────────────────────────────────────────────────────
    [[nodiscard]] std::size_t framesBuffered() const noexcept;   // PCM frames queued for the sink
    [[nodiscard]] std::size_t framesDropped() const noexcept;    // producer-side overflow (ring full)
    [[nodiscard]] std::size_t underflowFrames() const noexcept;  // consumer-side underflow (silence)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace retropp
