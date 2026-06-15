#pragma once

// Game Boy audio presets (ENG-4.A) — the GB-family catalogue of ready-made audio for an AudioSystem,
// the audio analogue of gbcpp/gb_routines.h (the GB RNG presets over a generic Vm).
//
// These are CONSOLE-SPECIFIC by design. The AudioSystem (gbcpp/audio_system.h) is console-agnostic —
// register-audio / cue / tick work for any VMPlatform. Anything that bakes in Game-Boy specifics (a
// wave-channel diagnostic tone here; a hUGEDriver adapter later) lives HERE, as a free function over an
// AudioSystem, NOT as a method on the generic surface — so a SNES / Genesis audio system brings its own
// presets in its own header, and nothing Game-Boy leaks into the shared API. Use these only with an
// AudioSystem constructed for a Game Boy console (VMPlatform::GameBoy / GameBoyColor).
//
// Eligibility: only PUBLIC-DOMAIN / permissively-licensed standard drivers may ship as engine presets
// (e.g. a future hUGEDriver adapter). A specific game's own copyrighted sound engine is NEVER a preset
// — the game supplies it.

#include "gbcpp/audio_system.h"

namespace gbcpp::sameboy {

// diagnosticTone — register the engine's built-in Game Boy test tone on `audio`: a gentle, steady
// ~250 Hz triangle on the wave channel (a low-harmonic, moderate-volume waveform chosen to be
// accessibility-considerate, not a harsh square). The verification signal for the audio chain — prove
// output works with no game driver. Returns a handle to cue exactly like any registered audio.
// `audio` must be a Game Boy AudioSystem (its source is SM83 wave-channel assembly).
AudioId diagnosticTone(AudioSystem& audio, AudioType type = AudioType::Sfx);

}  // namespace gbcpp::sameboy
