#pragma once

// The AudioSystem: a game's developer-facing audio OUTPUT engine.
//
// A game CUES audio HERE, in audio terms: it plays a registered sound by handle and stops it, whenever it
// likes. REGISTRATION IS NOT HERE — audio is registered on the single AudioLibrary
// (retropp/audio_library.h, the program-wide catalog), which mints an AudioId; an AudioSystem only CUES
// that handle. Every cued sound is a VOICE: cue as many as you like and they all sound together, mixed
// into this system's output — play() never cuts off audio that is already playing. The game never
// touches the machinery that makes a voice's sound — for a chiptune voice the AudioSystem owns a VM
// hosting that voice's driver at the hardware CPU clock with the console's sound chip enabled, and steps
// every voice on its OWN dedicated production thread, all internally. The game never steps anything: it
// just cues with play()/stop(); production self-paces on another core, robust to sim hitches. What's
// hidden is the voice plumbing (Vm / Routine / throttle / mixdown) and that thread; what's EXPOSED is
// the cue surface: play() / stop().
//
// FREELY INSTANTIABLE, like a Vm — no singleton. A game runs as MANY AudioSystems as it wants: a
// chiptune one here, a PCM-playback one there, several at once. Each owns its own resources and drains to
// its own AudioSink output stream; the OS mixes the streams. This is the same generalized, no-ceiling
// posture as VMPlatform / ViewportResolution everywhere in the engine.
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

namespace detail {
struct AudioSystemTestAccess;  // the internal synchronous test seam (src/audio/audio_system_testing.h)
}

// How a play() treats voices already playing the SAME AudioId on this system. Layer (the fixed
// default) starts the new voice beside them — re-fires overlap and decay naturally. Retrigger
// restarts the sound: voices of the same id stop and the fresh voice takes their place — the classic
// arcade re-fire — while every OTHER sound on the system plays on untouched. The only way to deviate
// from Layer is this explicit per-call token (the AssetPolicy shape): the choice is always visible at
// the call site, never ambient system state.
enum class CueMode { Layer, Retrigger };

// AudioId / AudioType / AudioKind live in retropp/audio_library.h (the audio vocabulary the single
// AudioLibrary owns); this header consumes them. Registering on an AudioSystem forwards to
// AudioLibrary::instance(); an AudioId's lifetime is the library's (the whole program), not the system's.

class AudioSystem {
public:
    // The first argument of EVERY constructor is `kind` — the system's fixed backend, stated explicitly
    // (there is no default): AudioKind::Chiptune runs the VM-hosted sound driver, AudioKind::Pcm decodes
    // and streams an audio file (its VM never runs). A system is ONE kind for its whole life; play()
    // rejects an id of the other kind.

    // BORROW a sink. Create a `kind` audio system for `platform` (GameBoy / GameBoyColor in v1),
    // draining produced PCM to `sink`, which the system does NOT own — `sink` must outlive the
    // AudioSystem. A Chiptune system owns the VM that hosts the game's sound driver (the game never sees
    // it); a Pcm system has no VM (it decodes a file instead). `timing` supplies the per-tick CPU cycle
    // budget for the chiptune path (it must carry a CPU
    // block — the GB-family presets do); `sampleRate` is both the sound chip's output rate and the rate
    // `sink` is opened at. The tail defaults reproduce the faithful Game Boy Color baseline. Prefer the
    // sink-less ctor below for the common case; this borrow form is for tests (a capture sink) and
    // pre-owned custom sinks.
    explicit AudioSystem(AudioKind kind,
                         AudioSink& sink,
                         VMPlatform platform = VMPlatform::GameBoyColor,
                         TimingProfile timing = TimingProfile::GameBoyColor,
                         unsigned sampleRate = kAudioSampleRate);

    // OWN a sink. As above, but the system TAKES OWNERSHIP of `sink` and ties it to its own lifetime —
    // no caller-side keep-alive. This is the custom-sink path ("hand me a sink, you keep nothing": a
    // network sink, a file-capture sink, …) and the device-free test seam for the ownership machinery
    // (pass a unique_ptr<CaptureAudioSink>). The sink-less ctor below delegates here.
    explicit AudioSystem(AudioKind kind,
                         std::unique_ptr<AudioSink> sink,
                         VMPlatform platform = VMPlatform::GameBoyColor,
                         TimingProfile timing = TimingProfile::GameBoyColor,
                         unsigned sampleRate = kAudioSampleRate);

    // MAKE YOUR OWN sink — the zero-boilerplate default. Owns an internally-constructed production
    // SdlAudioSink, so `AudioSystem music{AudioKind::Chiptune};` just works: no sink to declare, no
    // lifetime to manage. Assumes a Platform exists (the auto-created SdlAudioSink needs SDL_INIT_AUDIO,
    // which SdlPlatform's constructor performs) — true for any real game, and the same assumption the
    // manual SdlAudioSink path already makes. Headless / test contexts use a borrowed or owned
    // CaptureAudioSink (the two ctors above) instead, which open no device.
    explicit AudioSystem(AudioKind kind,
                         VMPlatform platform = VMPlatform::GameBoyColor,
                         TimingProfile timing = TimingProfile::GameBoyColor,
                         unsigned sampleRate = kAudioSampleRate);
    ~AudioSystem();

    AudioSystem(const AudioSystem&)            = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // REGISTRATION LIVES ON AudioLibrary, NOT HERE. An AudioSystem does not register audio — it CUES
    // already-registered audio by handle. A game registers ONCE on the single catalog —
    // `AudioLibrary::instance().uploadAudio(bytes, type, isa)` (raw) /
    // `AudioLibrary::instance().registerAudio("song.asm", type, isa, policy)` (path) — and the resulting
    // AudioId plays on ANY AudioSystem whose VM ISA matches the one selected at registration (play()
    // throws otherwise). Console-specific catalogue helpers (the Game Boy diagnostic tone, a future
    // hUGEDriver adapter) live in that console's preset namespace (retropp/gb_audio.h) and register on
    // the library too — none are methods here, so nothing console-specific leaks into this surface.

    // Cue a registered audio: the production thread begins producing it on its next pass, as a NEW
    // voice beside whatever is already sounding. Under the fixed Layer default play() NEVER cuts off
    // playing audio — every cued sound coexists, mixed into this system's output. Passing
    // CueMode::Retrigger restarts THIS sound: only voices already playing the same id are replaced (the
    // arcade re-fire), everything else plays on. Any channel contention lives INSIDE a single voice's
    // VM (the console's sound channels, allocated by the driver running there, exactly as on the
    // original hardware), never at the system level: a faithful port that hosts the game's original
    // sound engine as ONE driver keeps that driver's authentic channel-stealing, and separately cued
    // sounds simply layer.
    void play(AudioId id, CueMode mode = CueMode::Layer);

    // Silence the system: every voice rides a short release fade (~8 ms) to zero and closes — a hard
    // cut at amplitude would click — and the output drains to silence. Registered audio stays
    // registered — play() again to cue afresh. (One-shot Sfx voices also close themselves when their
    // sound finishes, already at silence; per-voice control arrives with the play() voice-handle
    // surface.)
    void stop();

    // ── Diagnostics (tests / dev) ────────────────────────────────────────────────────────────────
    [[nodiscard]] bool        isPlaying() const noexcept;       // a cued audio is currently being stepped
    [[nodiscard]] std::size_t framesBuffered() const noexcept;   // PCM frames queued for the sink
    [[nodiscard]] std::size_t framesDropped() const noexcept;    // producer-side overflow (ring full)
    [[nodiscard]] std::size_t underflowFrames() const noexcept;  // consumer-side underflow (silence)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Manual (thread-suppressed) construction for the internal test seam
    // (src/audio/audio_system_testing.h): builds the system WITHOUT starting the production thread, so a
    // device-free test drives production synchronously on its own thread. Borrows `sink`.
    struct ManualTag {};
    AudioSystem(ManualTag, AudioKind kind, AudioSink& sink, VMPlatform platform, TimingProfile timing,
                unsigned sampleRate);
    friend struct detail::AudioSystemTestAccess;
};

}  // namespace retropp
