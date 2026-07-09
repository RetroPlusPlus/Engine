// The AudioMixer end-to-end, device-free. Two layers:
//   * The pure gain math — perceptualGain (the slider taper), applyGain (the per-sample multiply), and the
//     Master-times-bus composition effectiveGain publishes — asserted directly, no device.
//   * The production-side effect — a known tone driven through a manual AudioSystem against a headless
//     CaptureAudioSink, whose captured PCM must equal the unity capture scaled by the composed gain, per
//     bus. The load-bearing gate is default unity: with every level at 255 the captured stream is the
//     exact identity of the unscaled one.
#include "retropp/audio_mixer.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/audio.h"
#include "retropp/audio_library.h"
#include "retropp/audio_system.h"
#include "retropp/gb_audio.h"                 // sameboy::diagnosticTone (GB preset)
#include "src/audio/audio_system_testing.h"   // detail::AudioSystemTestAccess — synchronous production seam
#include "src/audio/auto_close.h"             // detail::shouldAutoStop — the Vocals grouping check
#include "mock_platform.h"                    // test::CaptureAudioSink

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;

// Reset every level to its 255 unity default around each test — the mixer is a program-wide singleton, so
// a level left set would leak into the next test (here and in the other audio suites).
class AudioMixerTest : public ::testing::Test {
protected:
    void resetUnity() {
        AudioMixer& m = AudioMixer::instance();
        m.setMaster(255);
        m.setMusic(255);
        m.setSfx(255);
        m.setVocals(255);
    }
    void SetUp() override { resetUnity(); }
    void TearDown() override { resetUnity(); }
};

// Register the built-in diagnostic tone as `type`, drive one manual produce pass, and return the captured
// PCM. The tone driver is deterministic, so two calls produce the same raw samples — a unity capture is
// therefore the pre-gain reference a scaled capture is checked against.
std::vector<AudioFrame> captureTone(AudioType type) {
    test::CaptureAudioSink sink;
    auto audio = Access::makeManual(AudioKind::Chiptune, sink);
    const AudioId tone = sameboy::diagnosticTone(type);
    audio->play(tone);
    Access::step(*audio);
    return sink.drain(audio->framesBuffered());
}

// ── The gain math (pure, no device) ──────────────────────────────────────────────────────────────────

TEST_F(AudioMixerTest, PerceptualGainPinsItsEndpoints) {
    EXPECT_EQ(perceptualGain(0), 0u);          // hard mute
    EXPECT_EQ(perceptualGain(255), 1u << 16);  // exact unity, independent of the curve
}

TEST_F(AudioMixerTest, PerceptualGainIsStrictlyMonotonic) {
    for (int level = 1; level <= 255; ++level) {
        EXPECT_GT(perceptualGain(static_cast<std::uint8_t>(level)),
                  perceptualGain(static_cast<std::uint8_t>(level - 1)))
            << "level " << level;
    }
}

TEST_F(AudioMixerTest, PerceptualGainMidpointIsAboutHalfLoudness) {
    // Half the slider sounds like half: the midpoint sits near -10 dB (amplitude ~0.316), well above the
    // cube law's -18 dB (0.125) that reads as far too quiet at half.
    const std::uint32_t mid = perceptualGain(128);
    EXPECT_GT(mid, static_cast<std::uint32_t>(0.125 * 65536.0));  // louder than a cube-law half
    EXPECT_GT(mid, static_cast<std::uint32_t>(0.28 * 65536.0));
    EXPECT_LT(mid, static_cast<std::uint32_t>(0.35 * 65536.0));
}

TEST_F(AudioMixerTest, ApplyGainAtUnityIsTheExactIdentity) {
    // The load-bearing property: scaling by 1<<16 returns every 16-bit sample unchanged, so a unity mixer
    // is bit-identical to no mixer at all.
    for (int s = -32768; s <= 32767; ++s) {
        const auto sample = static_cast<std::int16_t>(s);
        EXPECT_EQ(applyGain(sample, 1u << 16), sample);
    }
}

TEST_F(AudioMixerTest, ApplyGainClampsAboveUnity) {
    EXPECT_EQ(applyGain(32767, 2u << 16), 32767);    // +full scale x2 clamps to the max
    EXPECT_EQ(applyGain(-32768, 2u << 16), -32768);  // -full scale x2 clamps to the min
    EXPECT_EQ(applyGain(0, 2u << 16), 0);            // silence stays silence at any gain
}

TEST_F(AudioMixerTest, EffectiveGainDefaultsToUnityOnEveryBus) {
    EXPECT_EQ(AudioMixer::instance().effectiveGain(AudioType::Music), 1u << 16);
    EXPECT_EQ(AudioMixer::instance().effectiveGain(AudioType::Sfx), 1u << 16);
    EXPECT_EQ(AudioMixer::instance().effectiveGain(AudioType::Vocals), 1u << 16);
}

TEST_F(AudioMixerTest, ABusLevelScalesOnlyItsOwnBus) {
    AudioMixer& m = AudioMixer::instance();
    m.setMusic(128);
    EXPECT_EQ(m.effectiveGain(AudioType::Music), perceptualGain(128));  // Master unity, so bus gain stands
    EXPECT_EQ(m.effectiveGain(AudioType::Sfx), 1u << 16);               // untouched
    EXPECT_EQ(m.effectiveGain(AudioType::Vocals), 1u << 16);           // untouched
}

TEST_F(AudioMixerTest, MasterComposesWithEveryBus) {
    AudioMixer& m = AudioMixer::instance();
    m.setMaster(128);
    m.setMusic(128);
    const auto expectedMusic = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(perceptualGain(128)) * perceptualGain(128)) >> 16);
    EXPECT_EQ(m.effectiveGain(AudioType::Music), expectedMusic);       // Master x Music
    EXPECT_EQ(m.effectiveGain(AudioType::Sfx), perceptualGain(128));   // Master x unity bus == Master
}

// ── The production-side effect (device-free, through the manual seam) ─────────────────────────────────

TEST_F(AudioMixerTest, DefaultUnityCaptureIsSampleForSampleIdentity) {
    // The faithful gate: at all-255, a captured tone equals the same tone recaptured — and the red->green
    // proof that the scale path is live: a non-unity level makes the capture diverge, restoring unity
    // reproduces it exactly.
    const std::vector<AudioFrame> baseline = captureTone(AudioType::Sfx);
    ASSERT_GT(baseline.size(), 0u);

    AudioMixer::instance().setSfx(128);  // pull one bus down
    const std::vector<AudioFrame> scaled = captureTone(AudioType::Sfx);
    ASSERT_EQ(scaled.size(), baseline.size());
    bool diverged = false;
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        if (scaled[i].left != baseline[i].left || scaled[i].right != baseline[i].right) {
            diverged = true;
            break;
        }
    }
    EXPECT_TRUE(diverged) << "a non-unity level must change the stream";

    AudioMixer::instance().setSfx(255);  // back to unity
    const std::vector<AudioFrame> restored = captureTone(AudioType::Sfx);
    ASSERT_EQ(restored.size(), baseline.size());
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        EXPECT_EQ(restored[i].left, baseline[i].left) << "frame " << i;
        EXPECT_EQ(restored[i].right, baseline[i].right) << "frame " << i;
    }
}

TEST_F(AudioMixerTest, CapturedPcmEqualsInputScaledByTheBusGain) {
    const std::vector<AudioFrame> baseline = captureTone(AudioType::Music);  // unity reference
    ASSERT_GT(baseline.size(), 0u);

    AudioMixer::instance().setMusic(128);
    const std::uint32_t gain = AudioMixer::instance().effectiveGain(AudioType::Music);
    const std::vector<AudioFrame> scaled = captureTone(AudioType::Music);
    ASSERT_EQ(scaled.size(), baseline.size());
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        EXPECT_EQ(scaled[i].left, applyGain(baseline[i].left, gain)) << "frame " << i;
        EXPECT_EQ(scaled[i].right, applyGain(baseline[i].right, gain)) << "frame " << i;
    }
}

TEST_F(AudioMixerTest, MasterAndBusComposeOnTheProducedStream) {
    const std::vector<AudioFrame> baseline = captureTone(AudioType::Music);  // unity reference
    ASSERT_GT(baseline.size(), 0u);

    AudioMixer::instance().setMaster(128);
    AudioMixer::instance().setMusic(128);
    const std::uint32_t gain = AudioMixer::instance().effectiveGain(AudioType::Music);  // Master x Music
    const std::vector<AudioFrame> scaled = captureTone(AudioType::Music);
    ASSERT_EQ(scaled.size(), baseline.size());
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        EXPECT_EQ(scaled[i].left, applyGain(baseline[i].left, gain)) << "frame " << i;
        EXPECT_EQ(scaled[i].right, applyGain(baseline[i].right, gain)) << "frame " << i;
    }
}

// ── Vocals ────────────────────────────────────────────────────────────────────────────────────────────

TEST_F(AudioMixerTest, VocalsRoutesToItsOwnBus) {
    const std::vector<AudioFrame> baseline = captureTone(AudioType::Vocals);  // unity reference
    ASSERT_GT(baseline.size(), 0u);

    AudioMixer::instance().setVocals(128);
    const std::uint32_t gain = AudioMixer::instance().effectiveGain(AudioType::Vocals);
    EXPECT_EQ(gain, perceptualGain(128));  // Master unity, so the Vocals bus gain stands alone
    const std::vector<AudioFrame> scaled = captureTone(AudioType::Vocals);
    ASSERT_EQ(scaled.size(), baseline.size());
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        EXPECT_EQ(scaled[i].left, applyGain(baseline[i].left, gain)) << "frame " << i;
        EXPECT_EQ(scaled[i].right, applyGain(baseline[i].right, gain)) << "frame " << i;
    }
}

TEST_F(AudioMixerTest, VocalsNeverAutoCloses) {
    // Vocals groups with Music: a sustained bus the game opens and closes, never auto-closed on a silence
    // run — unlike Sfx.
    EXPECT_FALSE(detail::shouldAutoStop(1'000'000, 12'000, AudioType::Vocals));
    EXPECT_FALSE(detail::shouldAutoStop(1'000'000, 12'000, AudioType::Music));
    EXPECT_TRUE(detail::shouldAutoStop(12'000, 12'000, AudioType::Sfx));  // Sfx still does
}

}  // namespace
}  // namespace retropp
