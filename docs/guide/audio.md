# Audio

Headers: `retropp/audio_system.h` (cue), `retropp/audio_library.h` (register), `retropp/gb_audio.h`
(Game Boy presets) — plus `retropp/sdl_platform.h` only if you hand a system a custom sink.

The engine plays sound in two halves. You **register** audio once on the program-wide **`AudioLibrary`**,
getting back an `AudioId`. You **cue** it by handle on an **`AudioSystem`** — `play(id)` / `stop()`.
Everything underneath — assembling the driver, running it at the right speed on its own thread, feeding
the audio device — the AudioSystem handles for you. You work in audio terms; you never touch the
machinery that makes the sound.

```cpp
#include "retropp/audio_system.h"
#include "retropp/audio_library.h"

retropp::AudioSystem audio{retropp::AudioKind::Chiptune};  // a chiptune system — owns its output, no sink to declare

retropp::AudioLibrary& library = retropp::AudioLibrary::instance();
const retropp::AudioId song =
    library.registerAudio("sound/overworld.asm", retropp::AudioType::Music, retropp::Isa::Sm83);

audio.play(song);

// That's all — production runs on the AudioSystem's own thread. You cue with play()/stop();
// you never step the audio from your game loop.
```

## Contents

- [The model](#the-model)
- [Registering audio — two doors](#registering-audio--two-doors)
  - [The Game Boy diagnostic tone](#the-game-boy-diagnostic-tone)
  - [`AudioType`: Music, Sfx, Vocals](#audiotype-music-sfx-vocals)
- [Cueing: the `AudioSystem`](#cueing-the-audiosystem)
- [Volume: the `AudioMixer`](#volume-the-audiomixer)
- [Output: the `AudioSink`](#output-the-audiosink)
- [Many audio systems at once](#many-audio-systems-at-once)
- [Choosing the console](#choosing-the-console)
- [What works today / what's planned](#what-works-today--whats-planned)

## The model

- **A system is one backend — chiptune or PCM.** The first constructor argument is an `AudioKind`
  (`Chiptune` or `Pcm`, no default), fixed for the system's life. A chiptune system runs a sound driver
  on a small VM it owns; a PCM system decodes and streams an audio file (`.wav` / `.ogg` / `.flac` /
  `.mp3`) and has no VM.
  `play()` throws if you cue an id of the other kind, so a system only ever produces its own kind.
- **Register on the library, cue on a system.** Registration is program-wide and lives on the single
  `AudioLibrary` (`AudioLibrary::instance()`), not on an `AudioSystem`. `registerAudio(...)` hands the
  library a piece of audio and returns an `AudioId`; an `AudioSystem` only **cues** it (`play(id)` cues,
  `stop()` silences). One registration plays on any AudioSystem whose console ISA matches the one you
  selected at registration — register once, cue from anywhere.
- **Production runs on its own thread.** Once you cue audio, the AudioSystem produces it autonomously on
  a dedicated thread, self-paced to the audio device, and feeds the PCM the device drains — you never
  step it from your game loop, and a slow simulation frame can't starve the sound. Cue with
  `play()`/`stop()`; that is the whole game-facing surface.
- **One-shot SFX close themselves; Music and Vocals you close.** An `AudioType::Sfx` cue stops on its own
  once its sound has finished — the AudioSystem notices the output has gone silent and stops producing for
  it, so you never call `stop()` for a fire-and-forget effect (and it stops costing anything once quiet).
  `AudioType::Music` and `AudioType::Vocals` are yours to manage: they play until you `stop()` them, and
  the engine never cuts them short (a track may rest mid-song). `isPlaying()` reports whether a cued sound
  is still being produced.
- **You never see a VM.** An AudioSystem owns whatever it needs internally to make sound — for a
  chiptune system that's a small virtual machine running a sound driver at the original hardware clock,
  so the music plays at the correct pitch. None of that is on the surface: you register *audio*, not a
  routine.

## Registering audio — two doors

Registration lives on `AudioLibrary` and comes in two forms, mirroring the renderer's
`uploadAtlas` / `loadAtlas`:

```cpp
// RAW door — you hand over ready bytes (pre-assembled driver bytecode). No embed/load policy: you
// brought the bytes. The library copies them into its own storage (the span need not outlive the call).
AudioId AudioLibrary::uploadAudio(std::span<const std::uint8_t> bytecode, AudioType type, Isa isa);

// SUGAR door — you hand over a compile-time LITERAL path; the build's Embed / LoadFromPath policy
// decides whether the assembled bytes are baked into the binary or the file ships beside it. The
// with-Isa (chiptune) overload takes a ChiptunePath, whose consteval ctor rejects a PCM extension
// (.wav/.ogg/.flac/.mp3) — an audio file cannot land on the chiptune door by mistake.
AudioId AudioLibrary::registerAudio(ChiptunePath resourcePath, AudioType type, Isa isa,
                                    std::optional<AssetPolicy> policy = {});

// The no-Isa overload takes a plain LiteralPath and registers a PCM audio file
// (.wav/.ogg/.flac/.mp3), decoded and streamed on an AudioKind::Pcm system — no ISA, no driver.
AudioId AudioLibrary::registerAudio(LiteralPath resourcePath, AudioType type,
                                    std::optional<AssetPolicy> policy = {});
```

Both mint an `AudioId` whose lifetime is the library's (the whole program). `uploadAudio` (bytes) is
**always chiptune**; `registerAudio` by path infers the *kind* from the extension (`.asm` → chiptune,
`.wav`/`.ogg`/`.flac`/`.mp3` → PCM), and the chiptune overload's `ChiptunePath` rejects a PCM extension
at compile time — so the kind is frozen into the entry with no way to mis-file it.

- **`isa`** — the instruction set the chiptune driver is written for, **selected by you** at registration
  (`Isa::Sm83` for the Game Boy family). It is the true compatibility unit: `play()` throws if you cue
  the id on an AudioSystem whose console runs a different ISA, so a mismatch fails loudly, not silently.
- **`type`** — `AudioType::Music`, `AudioType::Sfx`, or `AudioType::Vocals` (the routing tag; see below).
- **`policy`** (sugar door only) — `AssetPolicy::Embed` bakes the assembled bytes into the binary;
  `AssetPolicy::LoadFromPath` ships the `.asm` beside the binary and assembles it at registration. Omit
  it to take the per-type default (chiptune → `Embed`). The only way to deviate is the explicit per-call
  token, so the policy is always visible at the call site.

The sugar door takes a **compile-time literal** path (a `LiteralPath`), so a build-time scan can find and
bake it; a genuinely runtime path is not a door — read the bytes yourself and use `uploadAudio`.

```cpp
auto& lib = retropp::AudioLibrary::instance();
const AudioId overworld = lib.registerAudio("audio/overworld.asm", AudioType::Music, Isa::Sm83);                     // Embed (default)
const AudioId hit       = lib.registerAudio("audio/sfx/hit.asm",   AudioType::Sfx,   Isa::Sm83, AssetPolicy::Embed);
const AudioId credits   = lib.registerAudio("audio/credits.asm",   AudioType::Music, Isa::Sm83, AssetPolicy::LoadFromPath);
```

`registerAudio` / `uploadAudio` take a **sound-driver** — the assembly (`.asm`) that synthesizes a track
or effect (for a Game Boy port, your extracted/authored sound code), or its pre-assembled bytes. A path
is assembled in the `isa` you selected: `Isa::Sm83` uses the engine's own SM83 assembler; another console
backend assembles that console's instruction set. You never pick an assembler — the `isa` choice decides it.

> **Audio files (PCM):** the same `registerAudio` name also accepts an **audio file** through its no-Isa
> overload — a `.wav` / `.ogg` / `.flac` / `.mp3` path is recognized as PCM (by extension) and plays on an
> `AudioKind::Pcm` system, which decodes and streams it instead of running a chiptune driver. A registered
> sound is therefore either synthesized chiptune or a decoded file; construct the matching system kind to play it.

### The Game Boy diagnostic tone

```cpp
// retropp/gb_audio.h — a free function that REGISTERS a built-in test tone on the library and returns
// its handle. It is NOT a method on AudioSystem and takes no AudioSystem.
namespace retropp::sameboy { AudioId diagnosticTone(AudioType type = AudioType::Sfx); }
```

`diagnosticTone()` registers the engine's built-in gentle test tone (a soft, low-pitch ~250 Hz triangle
on the wave channel) on the `AudioLibrary` and returns its `AudioId` — cue it on a Game Boy `AudioSystem`
exactly like any registered audio. Handy to confirm your audio output is wired up before you have real
sound data.

### `AudioType`: Music, Sfx, Vocals

`AudioType` tags each registration as **`Music`**, **`Sfx`**, or **`Vocals`**. It does two things: it sets
the auto-close behavior — `Sfx` is fire-and-forget and closes itself when its sound finishes, while `Music`
and `Vocals` are sustained and stay until you `stop()` them — and it picks the **mixer bus** the source is
scaled by (see [Volume](#volume-the-audiomixer)). A track is on whichever bus its `AudioType` names.
`Vocals` is simply a third bus alongside `Music` and `SFX` — a separate volume channel a game can tag
voice/dialogue-style audio with; it is not tied to any particular kind (it works for chiptune or PCM
sources alike). Like `Music`, it is sustained.

> **Small routines vs. real drivers.** A short `.asm` like the diagnostic tone is assembled by the
> engine's built-in SM83 assembler. A *real* sound driver is much bigger and written in full RGBDS
> assembly (macros, sections, banked data): Game Boy music is composed in a **tracker** — the de-facto
> standard is [hUGETracker](https://superdisk.github.io/hUGETracker/) (the engine GB Studio uses), which
> exports [hUGEDriver](https://github.com/SuperDisk/hUGEDriver) (a public-domain RGBDS driver) + per-song
> data, driven by an init + a once-per-frame `dosound` call. A faithful *port* runs the original game's
> own sound engine (e.g. pokecrystal's `audio/engine.asm` + its song/SFX data). Those big drivers are
> assembled to bytes by **RGBDS** (their native toolchain) at your project's build, and you host the
> bytes through the **raw `uploadAudio` door** — the engine runs assembled machine code, it does not
> reassemble a full RGBDS driver itself. The production thread's once-per-frame driver cadence is already
> the `hUGE_dosound` shape.

## Cueing: the `AudioSystem`

```cpp
void AudioSystem::play(AudioId id, CueMode mode = CueMode::Layer);
                                      // start producing it as a NEW voice beside whatever is already sounding
void AudioSystem::stop();             // silence the system: every voice stops; the output drains (the audio stays registered)
```

You cue an `AudioId` minted by the library. **Under the fixed `Layer` default, `play()` never cuts off
audio that is already playing** — every cued sound is its own *voice*, and the system mixes all of its
voices into its output. Cue a one-shot effect over sustained music on the same system and both sound;
cue the same effect twice in quick succession and the two copies layer. Each chiptune voice runs its
driver on its own VM with the console's full set of sound channels, so any channel contention that
exists is a single driver's own doing *inside* its VM — a faithful port hosting the game's original
sound engine as one driver keeps that driver's authentic channel allocation (including its stealing),
and separately cued sounds simply coexist. `play()` verifies the entry's ISA matches this system's
console (throws on a mismatch) and materializes the driver lazily — so a single registration drives
whichever AudioSystem cues it.

**`CueMode::Retrigger` restarts a sound instead of layering it.** Passing it replaces only the voices
already playing the *same id* — the classic arcade re-fire, where a rapid second shot restarts the
shot sound rather than overlapping it — and every other sound on the system plays on untouched. The
choice is per-call and the `Layer` default never changes (the `AssetPolicy` shape): the only way to
deviate is the explicit token at the call site, so the behavior is always visible where it happens.

```cpp
sfx.play(shot, retropp::CueMode::Retrigger);  // a repeat fire restarts the effect
```

`stop()` is system-wide (one-shot `Sfx` voices also close themselves when their sound finishes);
per-voice control arrives with the planned `play()` voice-handle surface.

**A closing voice never clicks.** Cutting a waveform at amplitude is an audible click, so any close that
isn't already at silence rides a short (~8 ms) release fade to zero: the voice a `Retrigger` replaces,
every voice on `stop()`, and a PCM file whose final sample sits off zero (the tail decays from that last
frame). A one-shot chiptune SFX that auto-closed is already silent — nothing to fade.

`isPlaying()` reports whether a cued sound is still being produced. For inspecting the audio path while
debugging, `framesBuffered()` / `framesDropped()` / `underflowFrames()` give the queued PCM depth,
producer-side overflow (the ring filled), and consumer-side underflow (the device starved).

## Volume: the `AudioMixer`

Volume lives on the program-wide **`AudioMixer`** (`AudioMixer::instance()`, one per program like the
`AudioLibrary`). It carries four levels — a **Master** that scales everything, and one per bus: **Music**,
**Sfx**, **Vocals**. Each is a `std::uint8_t` slider position: `0` mutes, `255` is unity (0 dB). Every
`AudioSystem` reads it, scaling each source it produces by `Master` composed with the source's
`AudioType` bus.

```cpp
#include "retropp/audio_mixer.h"

retropp::AudioMixer& mix = retropp::AudioMixer::instance();
mix.setMaster(200);   // pull the whole program down a little
mix.setMusic(128);    // music at the slider's midpoint...
mix.setSfx(255);      // ...effects at full
```

- **Default unity.** Every level starts at `255`, which scales by exactly 1.0 — a mixer you never touch
  reproduces the produced stream sample for sample. This is the faithful default: install the port, change
  nothing, and it sounds exactly as it did with no mixer at all.
- **The slider is perceptual.** The levels between mute and unity follow a half-loudness taper, so the
  midpoint (`128`) sounds like about half — not the near-silence a straight linear scale gives at half.
  You set slider positions; the mixer handles the curve.
- **Set from anywhere, applied on the audio thread.** You set levels wherever your settings UI runs; each
  `AudioSystem` picks up the change on its production thread within a sample. Setting a level never affects
  the simulation — it scales output only.

A settings screen binds its Master / Music / SFX / Vocals sliders straight to these four setters. Values
are `0`–`255`, so a slider maps to a level with no conversion. The matching getters — `master()` /
`music()` / `sfx()` / `vocals()` (each `std::uint8_t`, default `255`) — read the current positions back,
so the settings screen initialises its sliders from them.

## Output: the `AudioSink`

The common case needs **no sink at all** — a bare `AudioSystem` owns an internal production
`SdlAudioSink`:

```cpp
retropp::AudioSystem audio{retropp::AudioKind::Chiptune};   // owns its SdlAudioSink; opens on start, releases on destruction
```

Hand the system a sink only when you need a *specific* output — a capture sink for tests, or a custom
(file / network) sink:

```cpp
retropp::SdlAudioSink sink;                                              // explicit output stream
retropp::AudioSystem  borrowed{retropp::AudioKind::Chiptune, sink};      // BORROW: `sink` must outlive `borrowed`

retropp::AudioSystem  owned{retropp::AudioKind::Chiptune,                // OWN: the system ties the sink to its lifetime
                            std::make_unique<MyCustomSink>()};
```

The auto-created `SdlAudioSink` needs SDL audio initialised, which `SdlPlatform`'s constructor performs —
true for any real game (it already has a platform for its window). Headless / test contexts pass a
`CaptureAudioSink` (borrowed or owned), which opens no device.

A custom sink implements the `AudioSink` interface — two pure virtuals: `start(unsigned rate, int
channels, AudioPullFn pull)` opens the device and begins draining by calling `pull` on the sink's own
audio thread, and `stop()` releases it. `AudioPullFn` is a
`std::function<std::size_t(std::span<AudioFrame>)>` the sink invokes to fill a buffer of `AudioFrame`s
(stereo 16-bit — `kAudioChannels == 2`, `kAudioSampleRate == 48'000`). Implement those two and a file or
network sink drops in wherever `SdlAudioSink` goes.

## Many audio systems at once

An `AudioSystem` is freely instantiable, like everything else in the engine — **run as many as you
want.** Each owns its own resources and its own output stream, and the OS mixes the streams, so you can
have one audio system for chiptune music and another for sampled effects, or several layered however your
game needs:

```cpp
retropp::AudioSystem music{retropp::AudioKind::Chiptune};  // each owns its own output stream — no sink to declare
retropp::AudioSystem sfx{retropp::AudioKind::Pcm};         // e.g. a PCM system for sampled effects
```

There is no global audio singleton and no fixed channel budget beyond what each system's backend models.
(The one program-wide singleton is the `AudioLibrary` *catalog* — where audio is registered — not the
*output*; outputs are per-system.)

## Choosing the console

```cpp
// Every ctor takes the AudioKind FIRST (no default). The sink-less form (preferred) follows it with the
// chiptune-path console knobs; the borrow / own-sink ctors take the sink right after the kind.
AudioSystem(AudioKind     kind,
            VMPlatform    platform   = VMPlatform::GameBoyColor,
            TimingProfile timing     = TimingProfile::GameBoyColor,
            unsigned      sampleRate = kAudioSampleRate /* 48 kHz */);
```

The console is a `VMPlatform`. `GameBoy` / `GameBoyColor` are the backends available today; the defaults
reproduce the faithful Game Boy Color baseline. Other consoles (SNES, Genesis, …) are drop-in backends —
the same `AudioSystem` surface drives them, with that console's sound chip and assembler behind it.
`platform` / `timing` configure the chiptune VM; a PCM system has no VM and ignores them, using only
`sampleRate` as its decode target.

## What works today / what's planned

| Capability | Status |
|---|---|
| Chiptune audio (register a sound-driver `.asm` / bytes, cue by handle, autonomous production thread) | available |
| Two registration doors — raw `uploadAudio` (bytes) / sugar `registerAudio` (literal path) | available |
| `AssetPolicy` Embed / LoadFromPath on the sugar door | available |
| Sink-less default `AudioSystem` (auto-owns an `SdlAudioSink`) | available |
| Multiple independent audio systems | available |
| `SdlAudioSink` output (48 kHz stereo) | available |
| `AudioType` Music/Sfx/Vocals tag on registration | available (sets auto-close + mixer bus) |
| Concurrent voices per system (`play()` never preempts; voices mix into the system's output) | available |
| PCM audio-pack backend (register + play a `.wav` / `.ogg` / `.flac` / `.mp3` file on an `AudioKind::Pcm` system) | available |
| `AudioMixer` volume levels (Master + Music/Sfx/Vocals, perceptual slider, default unity) | available |
| Per-voice gain + pan/balance (developer-owned, the `play()` voice handle) | planned |
| Anti-channel-stealing (splitting ONE driver's channel writes across parallel sound chips, so a driver whose own allocation steals channels stops stealing them) | planned |

When a planned capability lands, the same registration/cue surface gains it without changing how you
call it.
