// The audio-pack backend, device-free. Two halves: the file decoder (src/audio/pcm_decode.h) turns WAV /
// OGG bytes into stereo int16 frames at a target rate, and the AudioSystem dispatches a Pcm AudioId to
// those decoded frames (the VM/APU dormant) exactly as it dispatches a chiptune to the VM. The decoder
// cases run on in-memory and fixture bytes; the play-through drives the synchronous production seam.
#include <cstdint>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/asset_policy.h"
#include "retropp/audio.h"
#include "retropp/audio_library.h"
#include "retropp/audio_system.h"
#include "retropp/gb_audio.h"  // sameboy::diagnosticTone — a chiptune AudioId, for the dispatch case
#include "retropp/isa.h"
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — synchronous production seam
#include "src/audio/pcm_decode.h"            // detail::decodePcm
#include "mock_platform.h"                   // test::CaptureAudioSink

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;

// Build a minimal canonical PCM WAV (44-byte header + interleaved int16 samples) so the decoder can be
// driven on known bytes with no fixture file. dr_wav reads the samples back verbatim.
std::vector<std::uint8_t> makeWav(std::uint16_t channels, std::uint32_t rate,
                                  const std::vector<std::int16_t>& interleaved) {
    const std::uint32_t dataBytes  = static_cast<std::uint32_t>(interleaved.size()) * 2;
    const std::uint32_t byteRate   = rate * channels * 2;
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * 2);
    std::vector<std::uint8_t> w;
    auto u32 = [&](std::uint32_t v) {
        w.push_back(v & 0xff); w.push_back((v >> 8) & 0xff);
        w.push_back((v >> 16) & 0xff); w.push_back((v >> 24) & 0xff);
    };
    auto u16 = [&](std::uint16_t v) { w.push_back(v & 0xff); w.push_back((v >> 8) & 0xff); };
    auto tag = [&](const char* t) { for (int i = 0; i < 4; ++i) w.push_back(static_cast<std::uint8_t>(t[i])); };
    tag("RIFF"); u32(36 + dataBytes); tag("WAVE");
    tag("fmt "); u32(16); u16(1); u16(channels); u32(rate); u32(byteRate); u16(blockAlign); u16(16);
    tag("data"); u32(dataBytes);
    for (std::int16_t s : interleaved) u16(static_cast<std::uint16_t>(s));
    return w;
}

std::vector<std::uint8_t> readFixture(const char* name) {
    std::ifstream in{std::string(RETROPP_FIXTURES_DIR) + "/" + name, std::ios::binary};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

// ── decoder ──────────────────────────────────────────────────────────────────────────────────────────

// A mono WAV at the target rate decodes verbatim, duplicated into both stereo channels.
TEST(PcmDecode, MonoWavDecodesVerbatimToStereo) {
    const std::vector<std::int16_t> samples{100, -100, 200, -200, 300, -300, 400, -400};
    const std::vector<AudioFrame> out = detail::decodePcm(makeWav(1, 48'000, samples), 48'000);
    ASSERT_EQ(out.size(), samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        EXPECT_EQ(out[i].left, samples[i]);
        EXPECT_EQ(out[i].right, samples[i]);  // mono → L == R
    }
}

// A source below the sink rate is resampled up: a 24 kHz source doubles its frame count at 48 kHz.
TEST(PcmDecode, ResamplesToTargetRate) {
    std::vector<std::int16_t> ramp;
    for (int i = 0; i < 100; ++i) ramp.push_back(static_cast<std::int16_t>(i * 100));
    const std::vector<AudioFrame> out = detail::decodePcm(makeWav(1, 24'000, ramp), 48'000);
    EXPECT_EQ(out.size(), ramp.size() * 2);  // 24 kHz → 48 kHz doubles the frames
}

// A real OGG Vorbis fixture decodes to a non-empty stereo stream at the target rate.
TEST(PcmDecode, OggFixtureDecodes) {
    const std::vector<std::uint8_t> ogg = readFixture("tone.ogg");
    ASSERT_FALSE(ogg.empty());
    const std::vector<AudioFrame> out = detail::decodePcm(ogg, 48'000);
    EXPECT_GT(out.size(), 0u);  // the 0.02 s tone decodes to a few hundred frames
}

// Bytes that are neither a WAV nor an OGG container are rejected, not silently mis-decoded.
TEST(PcmDecode, RejectsUnknownContainer) {
    const std::vector<std::uint8_t> garbage{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    EXPECT_THROW(detail::decodePcm(garbage, 48'000), std::runtime_error);
}

// ── AudioSystem dispatch (a system is typed by its kind) ────────────────────────────────────────────────

// A Pcm AudioSystem plays a Pcm file: the embedded WAV decodes and its frames reach the production ring —
// no VM runs (a Pcm system has none). (A Pcm cue produced nothing before this backend existed — the
// red→green.) The fixture is shorter than the refill target, so one pass drains the whole buffer and the
// one-shot stops.
TEST(AudioPackBackend, PcmSystemPlaysFileIntoTheRing) {
    const AudioId pcm = AudioLibrary::instance().registerAudio(
        "tests/fixtures/tone.wav", AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);
    test::CaptureAudioSink sink;
    auto audio = Access::makeManual(AudioKind::Pcm, sink);
    audio->play(pcm);
    EXPECT_TRUE(audio->isPlaying());
    Access::step(*audio);
    EXPECT_GT(audio->framesBuffered(), 0u);    // decoded frames reached the ring (no VM ran)
    EXPECT_FALSE(audio->isPlaying());          // the short one-shot exhausted and stopped
}

// One kind per system: cueing an id of the OTHER backend throws (the ISA-mismatch precedent). A Pcm system
// rejects a chiptune id; a Chiptune system rejects a Pcm id — neither plays the wrong kind.
TEST(AudioPackBackend, CueingTheWrongKindThrows) {
    const AudioId chiptune = sameboy::diagnosticTone();
    const AudioId pcm      = AudioLibrary::instance().registerAudio(
        "tests/fixtures/tone.wav", AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);

    test::CaptureAudioSink pcmSink;
    auto pcmSystem = Access::makeManual(AudioKind::Pcm, pcmSink);
    EXPECT_THROW(pcmSystem->play(chiptune), std::runtime_error);

    test::CaptureAudioSink chipSink;
    auto chipSystem = Access::makeManual(AudioKind::Chiptune, chipSink);
    EXPECT_THROW(chipSystem->play(pcm), std::runtime_error);
}

}  // namespace
}  // namespace retropp
