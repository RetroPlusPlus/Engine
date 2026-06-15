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
#include <string_view>

#include "retropp/audio.h"
#include "retropp/timing.h"
#include "retropp/vm.h"  // VMPlatform

namespace retropp {

// How a registered audio is used. The tag rides on every registration and drives anti-channel-stealing
// routing in ENG-4.D: Music gets its own engine instance; Sfx share a preemptable pool. In v1
// (single instance) the tag is stored but routing is natural channel-stealing — the faithful default.
enum class AudioType { Music, Sfx };

// An opaque handle to a registered audio resource on an AudioSystem — cue it with play(). A value
// handle, valid only while the AudioSystem that minted it is alive (the AtlasId / PaletteId contract).
enum class AudioId : std::uint32_t {};

class AudioSystem {
public:
    // Create a chiptune audio system for `platform` (GameBoy / GameBoyColor in v1), draining produced
    // PCM to `sink`. Owns the VM that will host the game's sound driver — the game never sees it.
    // `timing` supplies the per-tick CPU cycle budget (it must carry a CPU block — the GB-family
    // presets do); `sampleRate` is both the sound chip's output rate and the rate `sink` is opened at.
    // Defaults reproduce the faithful Game Boy Color baseline. `sink` must outlive the AudioSystem.
    explicit AudioSystem(AudioSink& sink,
                         VMPlatform platform = VMPlatform::GameBoyColor,
                         TimingProfile timing = TimingProfile::GameBoyColor,
                         unsigned sampleRate = kAudioSampleRate);
    ~AudioSystem();

    AudioSystem(const AudioSystem&)            = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // Register a piece of audio from a sound-driver `.asm` source, tagged Music or Sfx, returning a
    // handle to cue it by. The source is assembled in this system's console ISA (SM83 for the Game Boy
    // family) and hosted on the internal VM at hardware speed — the game supplies audio, never a Vm or
    // a Routine. (When ENG-4.C lands, the same call takes an audio FILE for a PCM-backed system.)
    //
    // This method is console-AGNOSTIC: it assembles + hosts whatever source the system's console
    // backend understands. Anything console-SPECIFIC — a built-in diagnostic tone, a standard-driver
    // adapter like hUGEDriver — is NOT a method here; it lives in that console's preset namespace and
    // is a free function over an AudioSystem (Game Boy presets in retropp/gb_audio.h, the way the Game Boy
    // RNG presets in retropp/gb_routines.h sit over the generic Vm). A SNES / Genesis audio system brings
    // its own presets; none are baked into this generic surface.
    AudioId registerAudio(std::string_view asmFilePath, AudioType type);

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
