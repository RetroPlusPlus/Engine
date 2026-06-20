# Audio

Headers: `retropp/audio_system.h`, `retropp/audio.h` (+ `retropp/sdl_platform.h` for the production output)

The engine plays sound through an **`AudioSystem`**: you register audio with it and cue it by handle.
Everything underneath — synthesizing the waveform, running it at the right speed, feeding the audio
device — the AudioSystem handles for you. You work in audio terms; you never touch the machinery that
makes the sound.

```cpp
#include "retropp/audio_system.h"
#include "retropp/sdl_platform.h"   // SdlAudioSink — the real audio output

retropp::SdlAudioSink sink;                       // one output stream
retropp::AudioSystem  audio{sink};                // a Game Boy Color audio system (the default)

const retropp::AudioId song = audio.registerAudio("sound/overworld.asm", retropp::AudioType::Music);
audio.play(song);

// That's all — production runs on the AudioSystem's own thread. You cue with play()/stop();
// you never step the audio from your game loop.
```

## The model

- **Register, then cue.** `registerAudio(source, type)` hands the AudioSystem a piece of audio and
  returns an `AudioId`. `play(id)` cues it; `stop()` silences playback. Registered audio stays
  registered — cue it again whenever you like.
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

## Registering audio

```cpp
// On the generic AudioSystem (console-agnostic):
AudioId registerAudio(std::string_view asmFilePath, AudioType type);

// Game Boy preset (retropp/gb_audio.h) — a free function over an AudioSystem, NOT a method:
namespace retropp::sameboy { AudioId diagnosticTone(AudioSystem&, AudioType = AudioType::Sfx); }
```

`registerAudio` takes a **sound-driver `.asm` source** — the assembly that synthesizes a track or
effect (for a Game Boy port, your extracted/authored sound code). It is assembled in *your audio
system's console ISA* automatically: a Game Boy / Game Boy Color system assembles SM83; another console
backend assembles that console's instruction set. You write audio source in your console's assembly and
never pick an assembler.

> **Coming with audio packs:** the same `registerAudio` call will also accept an **audio file** (for a
> PCM-playback audio system), so a registered sound can be either synthesized chiptune or a decoded
> file. That backend is planned; today `registerAudio` takes a `.asm` driver.

`AudioType` tags each registration as **`Music`** or **`Sfx`**. Today (single output engine) the tag is
stored but playback is one sound at a time — starting a new one preempts the current, exactly as the
original hardware's channel-stealing does. The tag is what a future anti-channel-stealing mode uses to
route music and effects to separate engines so they don't cut each other off.

`registerDiagnosticTone` registers a built-in gentle test tone (a soft, low-pitch triangle) — handy to
confirm your audio output is wired up before you have real sound data.

> **Small routines vs. real drivers.** The `.asm` form above is assembled by the engine's built-in
> SM83 assembler — a small instruction encoder, good for short routines like the diagnostic tone. A
> *real* sound driver is much bigger and written in full RGBDS assembly (macros, sections, banked data):
> Game Boy music is composed in a **tracker** — the de-facto standard is
> [hUGETracker](https://superdisk.github.io/hUGETracker/) (the engine GB Studio uses), which exports
> [hUGEDriver](https://github.com/SuperDisk/hUGEDriver) (a public-domain RGBDS driver) + per-song data,
> driven by an init + a once-per-frame `dosound` call. A faithful *port* runs the original game's own
> sound engine (e.g. pokecrystal's `audio/engine.asm` + its song/SFX data). Those big drivers are
> assembled to bytes by **RGBDS** (their native toolchain) at your project's build, and the audio
> system hosts the bytes — the engine runs assembled machine code, it does not reassemble a full RGBDS
> driver itself. That **pre-assembled-driver + data path is planned (ENG-4.B)**; today the `.asm` form
> covers short routines, and the production thread's once-per-frame driver cadence is already the
> `hUGE_dosound` shape.

## Output: the `AudioSink`

An `AudioSystem` drains to an **`AudioSink`** you hand it — the boundary to the host audio device. The
production sink is **`SdlAudioSink`** (an SDL audio stream on the default device):

```cpp
retropp::SdlAudioSink sink;
retropp::AudioSystem  audio{sink};
```

`SdlAudioSink` is freely constructible — make one per AudioSystem. The sink opens its device stream when
the AudioSystem starts and releases it when the AudioSystem is destroyed; you don't drive it directly.
(It needs SDL audio initialised, which `SdlPlatform` does in its constructor — a game already has a
platform for its window.)

## Many audio systems at once

An `AudioSystem` is freely instantiable, like everything else in the engine — **run as many as you
want.** Each owns its own resources and its own output stream, and the OS mixes the streams, so you can
have one audio system for chiptune music and another (when the PCM backend lands) for sampled effects,
or several layered however your game needs:

```cpp
retropp::SdlAudioSink musicSink, sfxSink;
retropp::AudioSystem  music{musicSink};
retropp::AudioSystem  sfx{sfxSink};
```

There is no global audio singleton and no fixed channel budget beyond what each system's backend models.

## Choosing the console

```cpp
AudioSystem(AudioSink& sink,
            VMPlatform    platform = VMPlatform::GameBoyColor,
            TimingProfile timing   = TimingProfile::GameBoyColor,
            unsigned      sampleRate = kAudioSampleRate /* 48 kHz */);
```

The console is a `VMPlatform`. `GameBoy` / `GameBoyColor` are the backends available today; the defaults
reproduce the faithful Game Boy Color baseline. Other consoles (SNES, Genesis, …) are drop-in backends —
the same `AudioSystem` surface drives them, with that console's sound chip and assembler behind it.

## What works today / what's planned

| Capability | Status |
|---|---|
| Chiptune `AudioSystem` (register a sound-driver `.asm`, cue by handle, autonomous production thread) | available |
| Multiple independent audio systems | available |
| `SdlAudioSink` output (48 kHz stereo) | available |
| `AudioType` Music/Sfx tag on registration | available (stored; routing is planned) |
| PCM audio-pack backend (register an audio *file*) | planned |
| Anti-channel-stealing routing (Music/Sfx on separate engines) | planned |

When a planned capability lands, the same registration/cue surface gains it — registering an audio file
instead of a driver, or opting a system into anti-stealing routing — without changing how you call it.
