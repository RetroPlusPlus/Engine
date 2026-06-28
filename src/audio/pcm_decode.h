#ifndef RETROPP_SRC_AUDIO_PCM_DECODE_H
#define RETROPP_SRC_AUDIO_PCM_DECODE_H

// PCM file decoding — the audio-pack backend's one job: turn a `.wav` / `.ogg` file's bytes into the
// engine's stereo int16 frames at the sink rate.
//
// INTERNAL — under src/audio/, never include/retropp/. This is the ONLY TU that includes the vendored
// decoders (dr_wav, stb_vorbis): the sameboy_machine.cpp-includes-gb.h precedent — third-party headers
// stay out of every other TU. A consumer never sees a decoder type; it registers a file on the
// AudioLibrary and plays the resulting AudioId, exactly as it plays a chiptune.

#include <cstdint>
#include <span>
#include <vector>

#include "retropp/audio.h"  // AudioFrame

namespace retropp::detail {

// Decode a WAV or OGG Vorbis resource (the whole file's bytes) into stereo int16 frames resampled to
// `targetRate`. The container is detected from the bytes (RIFF/WAVE -> dr_wav; OggS -> stb_vorbis); mono
// duplicates to both channels, more-than-stereo keeps the front two; a source at a different rate is
// linearly resampled to `targetRate`. Throws std::runtime_error on an unsupported or corrupt container.
std::vector<AudioFrame> decodePcm(std::span<const std::uint8_t> fileBytes, unsigned targetRate);

// Indirection that keeps the decoder out of binaries that decode no audio files. decodePcm lives in a
// translation unit that pulls in the vendored dr_wav / stb_vorbis; naming it directly anywhere always-linked
// would drag those decoders into every audio binary. Instead AudioSystem decodes through this pointer, and
// only the audio-file registration door installs it (pointing it at decodePcm). A program that registers no
// audio file never pulls that door's translation unit, leaves the hook null, and links zero decoder code.
using PcmDecodeFn = std::vector<AudioFrame> (*)(std::span<const std::uint8_t>, unsigned);
extern PcmDecodeFn g_pcmDecode;

}  // namespace retropp::detail

#endif  // RETROPP_SRC_AUDIO_PCM_DECODE_H
