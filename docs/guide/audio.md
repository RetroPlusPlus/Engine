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

retropp::AudioSystem audio;     // a Game Boy Color audio system — owns its output, no sink to declare

retropp::AudioLibrary& library = retropp::AudioLibrary::instance();
const retropp::AudioId song =
    library.registerAudio("sound/overworld.asm", retropp::AudioType::Music, retropp::Isa::Sm83);

audio.play(song);

// That's all — production runs on the AudioSystem's own thread. You cue with play()/stop();
// you never step the audio from your game loop.
```

## The model

- **Register on the library, cue on a system.** Registration is program-wide and lives on the single
  `AudioLibrary` (`AudioLibrary::instance()`), not on an `AudioSystem`. `registerAudio(...)` hands the
  library a piece of audio and returns an `AudioId`; an `AudioSystem` only **cues** it (`play(id)` cues,
  `stop()` silences). One registration plays on any AudioSystem whose console ISA matches the one you
  selected at registration — register once, cue from anywhere.
- **Production runs on its own thread.** Once you cue audio, the AudioSystem produces it autonomously on
  a dedicated thread, self-paced to the audio device, and feeds the PCM the device drains — you never
  step it from your game loop, and a slow simulation frame can't starve the sound. Cue with
  `play()`/`stop()`; that is the whole game-facing surface.
- **One-shot SFX close themselves; Music you close.** An `AudioType::Sfx` cue stops on its own once its
  sound has finished — the AudioSystem notices the output has gone silent and stops producing for it, so
  you never call `stop()` for a fire-and-forget effect (and it stops costing anything once quiet).
  `AudioType::Music` is yours to manage: it plays until you `stop()` it, and the engine never cuts it
  short (a track may rest mid-song). `isPlaying()` reports whether a cued sound is still being produced.
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
// decides whether the assembled bytes are baked into the binary or the file ships beside it.
AudioId AudioLibrary::registerAudio(LiteralPath resourcePath, AudioType type, Isa isa,
                                    std::optional<AssetPolicy> policy = {});
```

Both mint an `AudioId` whose lifetime is the library's (the whole program). The chiptune-vs-PCM *kind* is
inferred automatically (element type for bytes, extension for a path) and frozen into the entry; the
call you write is identical for both.

- **`isa`** — the instruction set the chiptune driver is written for, **selected by you** at registration
  (`Isa::Sm83` for the Game Boy family). It is the true compatibility unit: `play()` throws if you cue
  the id on an AudioSystem whose console runs a different ISA, so a mismatch fails loudly, not silently.
- **`type`** — `AudioType::Music` or `AudioType::Sfx` (the routing tag; see below).
- **`policy`** (sugar door only) — `AssetPolicy::Embed` bakes the assembled bytes into the binary;
  `AssetPolicy::LoadFromPath` ships the `.asm` beside the binary and assembles it at registration. Omit
  it to take the default (chiptune → `Embed`). Precedence: per-call > `EngineConfig::defaultRoutinePolicy`
  > per-type default.

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

> **Coming with audio packs:** the same `registerAudio` call will also accept an **audio file** (for a
> PCM-playback audio system), so a registered sound can be either synthesized chiptune or a decoded
> file. The PCM kind is already a tagged seam (a `.wav` / `.ogg` / `.flac` / `.mp3` path is recognized);
> the backend that plays it is planned. Today the realized kind is chiptune.

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

### `AudioType`: Music vs Sfx

`AudioType` tags each registration as **`Music`** or **`Sfx`**. Today (single output per system) the tag
is stored but playback is one sound at a time — starting a new one preempts the current, exactly as the
original hardware's channel-stealing does. The tag is what a future anti-channel-stealing mode (ENG-4.D)
uses to route music and effects so they don't cut each other off.

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
void AudioSystem::play(AudioId id);   // begin producing it from the next tick (preempts any current sound in v1)
void AudioSystem::stop();             // stop producing; the output drains to silence (the audio stays registered)
```

You cue an `AudioId` minted by the library. `play()` verifies the entry's ISA matches this system's
console (throws on a mismatch) and then materializes the driver into the VM it owns, lazily — so a single
registration drives whichever AudioSystem cues it.

## Output: the `AudioSink`

The common case needs **no sink at all** — a bare `AudioSystem` owns an internal production
`SdlAudioSink`:

```cpp
retropp::AudioSystem audio;   // owns its SdlAudioSink; opens the device on start, releases it on destruction
```

Hand the system a sink only when you need a *specific* output — a capture sink for tests, or a custom
(file / network) sink:

```cpp
retropp::SdlAudioSink sink;                                    // explicit output stream
retropp::AudioSystem  borrowed{sink};                         // BORROW: `sink` must outlive `borrowed`

retropp::AudioSystem  owned{std::make_unique<MyCustomSink>()}; // OWN: the system ties the sink to its lifetime
```

The auto-created `SdlAudioSink` needs SDL audio initialised, which `SdlPlatform`'s constructor performs —
true for any real game (it already has a platform for its window). Headless / test contexts pass a
`CaptureAudioSink` (borrowed or owned), which opens no device.

## Many audio systems at once

An `AudioSystem` is freely instantiable, like everything else in the engine — **run as many as you
want.** Each owns its own resources and its own output stream, and the OS mixes the streams, so you can
have one audio system for chiptune music and another (when the PCM backend lands) for sampled effects,
or several layered however your game needs:

```cpp
retropp::AudioSystem music;   // each owns its own output stream — no sink to declare
retropp::AudioSystem sfx;
```

There is no global audio singleton and no fixed channel budget beyond what each system's backend models.
(The one program-wide singleton is the `AudioLibrary` *catalog* — where audio is registered — not the
*output*; outputs are per-system.)

## Choosing the console

```cpp
// The sink-less default ctor (preferred). The borrow / own-sink ctors take the sink as the first arg,
// before these.
AudioSystem(VMPlatform    platform   = VMPlatform::GameBoyColor,
            TimingProfile timing     = TimingProfile::GameBoyColor,
            unsigned      sampleRate = kAudioSampleRate /* 48 kHz */);
```

The console is a `VMPlatform`. `GameBoy` / `GameBoyColor` are the backends available today; the defaults
reproduce the faithful Game Boy Color baseline. Other consoles (SNES, Genesis, …) are drop-in backends —
the same `AudioSystem` surface drives them, with that console's sound chip and assembler behind it.

## What works today / what's planned

| Capability | Status |
|---|---|
| Chiptune audio (register a sound-driver `.asm` / bytes, cue by handle, autonomous production thread) | available |
| Two registration doors — raw `uploadAudio` (bytes) / sugar `registerAudio` (literal path) | available |
| `AssetPolicy` Embed / LoadFromPath on the sugar door | available |
| Sink-less default `AudioSystem` (auto-owns an `SdlAudioSink`) | available |
| Multiple independent audio systems | available |
| `SdlAudioSink` output (48 kHz stereo) | available |
| `AudioType` Music/Sfx tag on registration | available (stored; routing is planned) |
| PCM audio-pack backend (register an audio *file*) | planned (kind is a tagged seam today) |
| Anti-channel-stealing routing (Music/Sfx on separate instances) | planned |

When a planned capability lands, the same registration/cue surface gains it — registering an audio file
instead of a driver, or opting a system into anti-stealing routing — without changing how you call it.
