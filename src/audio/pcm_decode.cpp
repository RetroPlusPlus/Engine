// PCM file decoding. The vendored decoders live ONLY here (dr_wav for WAV, stb_vorbis for OGG Vorbis);
// every other engine TU stays free of their headers. Both decode from memory and yield interleaved int16
// samples, which fold to stereo AudioFrames and resample to the sink rate.
#include "src/audio/pcm_decode.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>

// Declarations only: the implementations compile in the retropp-audiodecode static library
// (third_party/dr_libs/dr_wav.c and third_party/stb/stb_vorbis.c).
#include "dr_wav.h"
extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
}

namespace retropp::detail {

namespace {

// One decoded source: interleaved int16, its channel count, and its native rate. The decoder fills it;
// foldAndResample turns it into the engine's stereo frames.
struct DecodedSource {
    std::vector<std::int16_t> interleaved;
    int                       channels = 0;
    unsigned                  rate     = 0;
};

// Fold to stereo and linearly resample to `targetRate`. Mono duplicates L=R; more than two channels
// keeps the front two (front-L / front-R) — the simplest correct fold, no surround model. Linear
// interpolation is adequate for the rate conversion; a higher-order resampler is a future refinement.
std::vector<AudioFrame> foldAndResample(const DecodedSource& src, unsigned targetRate) {
    const std::size_t frames =
        src.channels > 0 ? src.interleaved.size() / static_cast<std::size_t>(src.channels) : 0;

    std::vector<AudioFrame> folded;
    folded.reserve(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::int16_t* s = &src.interleaved[f * static_cast<std::size_t>(src.channels)];
        const std::int16_t left  = s[0];
        const std::int16_t right = src.channels >= 2 ? s[1] : s[0];
        folded.push_back(AudioFrame{left, right});
    }

    if (src.rate == targetRate || folded.size() < 2) {
        return folded;  // already at rate (or too short to interpolate) — pass through
    }

    const std::size_t outFrames = static_cast<std::size_t>(
        static_cast<std::uint64_t>(folded.size()) * targetRate / src.rate);
    auto lerp = [](std::int16_t a, std::int16_t b, double t) {
        return static_cast<std::int16_t>(std::lround(a + (b - a) * t));
    };
    std::vector<AudioFrame> out;
    out.reserve(outFrames);
    for (std::size_t i = 0; i < outFrames; ++i) {
        const double      pos  = static_cast<double>(i) * src.rate / targetRate;
        const std::size_t i0   = static_cast<std::size_t>(pos);
        const double      frac = pos - static_cast<double>(i0);
        const std::size_t i1   = std::min(i0 + 1, folded.size() - 1);
        out.push_back(AudioFrame{lerp(folded[i0].left, folded[i1].left, frac),
                                 lerp(folded[i0].right, folded[i1].right, frac)});
    }
    return out;
}

// True when the bytes begin with a RIFF/WAVE container header.
bool isWav(std::span<const std::uint8_t> b) noexcept {
    return b.size() >= 12 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' &&
           b[9] == 'A' && b[10] == 'V' && b[11] == 'E';
}

// True when the bytes begin with an Ogg page-capture pattern ("OggS").
bool isOgg(std::span<const std::uint8_t> b) noexcept {
    return b.size() >= 4 && b[0] == 'O' && b[1] == 'g' && b[2] == 'g' && b[3] == 'S';
}

DecodedSource decodeWav(std::span<const std::uint8_t> b) {
    unsigned int   channels   = 0;
    unsigned int   sampleRate = 0;
    drwav_uint64   frameCount = 0;
    drwav_int16*   samples    = drwav_open_memory_and_read_pcm_frames_s16(
        b.data(), b.size(), &channels, &sampleRate, &frameCount, nullptr);
    if (samples == nullptr) {
        throw std::runtime_error("decodePcm: not a decodable WAV container");
    }
    DecodedSource src;
    src.channels = static_cast<int>(channels);
    src.rate     = sampleRate;
    src.interleaved.assign(samples, samples + frameCount * channels);
    drwav_free(samples, nullptr);
    return src;
}

DecodedSource decodeOgg(std::span<const std::uint8_t> b) {
    int     channels   = 0;
    int     sampleRate = 0;
    short*  samples    = nullptr;
    const int frameCount = stb_vorbis_decode_memory(
        b.data(), static_cast<int>(b.size()), &channels, &sampleRate, &samples);
    if (frameCount < 0 || samples == nullptr) {
        throw std::runtime_error("decodePcm: not a decodable OGG Vorbis container");
    }
    DecodedSource src;
    src.channels = channels;
    src.rate     = static_cast<unsigned>(sampleRate);
    src.interleaved.assign(samples, samples + static_cast<std::size_t>(frameCount) * channels);
    std::free(samples);
    return src;
}

}  // namespace

std::vector<AudioFrame> decodePcm(std::span<const std::uint8_t> fileBytes, unsigned targetRate) {
    if (isWav(fileBytes)) {
        return foldAndResample(decodeWav(fileBytes), targetRate);
    }
    if (isOgg(fileBytes)) {
        return foldAndResample(decodeOgg(fileBytes), targetRate);
    }
    throw std::runtime_error(
        "decodePcm: unrecognized audio container (only WAV and OGG Vorbis are decoded)");
}

}  // namespace retropp::detail
