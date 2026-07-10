// Multi-voice cueing: play() NEVER cuts off audio that is already playing. Every cued sound is its own
// voice — a chiptune voice on its own VM+APU, a PCM voice on its own cursor — and the production pass
// mixes every voice's post-gain PCM (saturating sum) into the system's one output. Channel contention
// exists only INSIDE a single voice's VM (the console's sound channels, allocated by the driver running
// there); the system level never steals, never preempts. Device-free: CaptureAudioSink + the internal
// synchronous seam (AudioSystemTestAccess::makeManual + step), so voice lifecycles and the mixdown are
// exercised deterministically.
#include "retropp/audio_system.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/asset_policy.h"    // AssetPolicy::LoadFromPath
#include "retropp/asset_registry.h"  // setAssetRoot — the single LoadFromPath base for the literal names
#include "retropp/audio_library.h"   // AudioLibrary, AudioType, AudioId
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — synchronous production seam
#include "src/audio/produce_step.h"  // detail::mixFrames / clampMixedSample — the pure mixdown under test
#include "mock_platform.h"           // test::CaptureAudioSink

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;

void setTonesRoot() {
    setAssetRoot(std::filesystem::path(RETROPP_ASSETS_DIR) / "tones");
}

// Produce + drain `passes` times, appending every drained frame to `out`.
void driveCapturing(AudioSystem& audio, test::CaptureAudioSink& sink, int passes,
                    std::vector<AudioFrame>& out) {
    for (int i = 0; i < passes; ++i) {
        Access::step(audio);
        const std::vector<AudioFrame> got = sink.drain(1u << 20);
        out.insert(out.end(), got.begin(), got.end());
    }
}

// Produce + drain until the system goes idle or the cap is hit; returns the passes taken.
int driveUntilIdleOrCap(AudioSystem& audio, test::CaptureAudioSink& sink, int cap) {
    int passes = 0;
    for (; passes < cap && audio.isPlaying(); ++passes) {
        Access::step(audio);
        sink.drain(1u << 20);
    }
    return passes;
}

// ── The pure mixdown ──────────────────────────────────────────────────────────────────────────────
TEST(AudioMultiVoice, ClampMixedSampleSaturates) {
    using detail::clampMixedSample;
    EXPECT_EQ(clampMixedSample(0), 0);
    EXPECT_EQ(clampMixedSample(1234), 1234);
    EXPECT_EQ(clampMixedSample(-1234), -1234);
    EXPECT_EQ(clampMixedSample(32767), 32767);
    EXPECT_EQ(clampMixedSample(-32768), -32768);
    EXPECT_EQ(clampMixedSample(32768), 32767);       // saturates high
    EXPECT_EQ(clampMixedSample(-32769), -32768);     // saturates low
    EXPECT_EQ(clampMixedSample(70'000), 32767);      // two full-scale voices
    EXPECT_EQ(clampMixedSample(-70'000), -32768);
}

TEST(AudioMultiVoice, MixFramesSumsPerPosition) {
    using detail::mixFrames;
    const AudioFrame a{100, -200};
    const AudioFrame b{50, 75};
    const AudioFrame two[] = {a, b};
    const AudioFrame mixed = mixFrames(two);
    EXPECT_EQ(mixed.left, 150);
    EXPECT_EQ(mixed.right, -125);

    // A single voice is the exact identity — mixing costs nothing until a second voice plays.
    const AudioFrame one[] = {a};
    const AudioFrame same = mixFrames(one);
    EXPECT_EQ(same.left, a.left);
    EXPECT_EQ(same.right, a.right);

    // Empty mixes to silence.
    const AudioFrame silence = mixFrames({});
    EXPECT_EQ(silence.left, 0);
    EXPECT_EQ(silence.right, 0);
}

// ── play() does not preempt: an SFX layered over Music leaves the Music playing ───────────────────
TEST(AudioMultiVoice, SfxOverMusicDoesNotCutTheMusic) {
    setTonesRoot();
    test::CaptureAudioSink sink;
    auto audioOwner = Access::makeManual(AudioKind::Chiptune, sink);
    AudioSystem& audio = *audioOwner;
    const AudioId music = AudioLibrary::instance().registerAudio("tone_sustain.asm", AudioType::Music,
                                                                 Isa::Sm83, AssetPolicy::LoadFromPath);
    const AudioId blip = AudioLibrary::instance().registerAudio("sfx_blip.asm", AudioType::Sfx, Isa::Sm83,
                                                                AssetPolicy::LoadFromPath);
    audio.play(music);
    std::vector<AudioFrame> pre;
    driveCapturing(audio, sink, 20, pre);
    ASSERT_TRUE(audio.isPlaying());

    audio.play(blip);  // layer a one-shot over the sustained music — the music must survive it
    ASSERT_TRUE(audio.isPlaying());

    // Drive well past the blip's decay + the auto-close threshold: the blip voice closes ITSELF, the
    // music voice plays on — a finished one-shot takes down only its own voice, never the system.
    std::vector<AudioFrame> post;
    driveCapturing(audio, sink, 400, post);
    EXPECT_TRUE(audio.isPlaying()) << "the finished SFX took the music down with it (preemption)";

    // And the music is still actually SOUNDING, not merely flagged: fresh frames keep coming.
    std::vector<AudioFrame> tail;
    driveCapturing(audio, sink, 5, tail);
    EXPECT_FALSE(tail.empty()) << "the surviving music voice produced nothing";
}

// ── Two simultaneous voices of the same tone sum sample-for-sample ────────────────────────────────
TEST(AudioMultiVoice, TwoIdenticalVoicesSumExactly) {
    setTonesRoot();
    const AudioId tone = AudioLibrary::instance().registerAudio("tone_sustain.asm", AudioType::Music,
                                                                Isa::Sm83, AssetPolicy::LoadFromPath);

    // Reference: ONE voice of the tone, captured deterministically.
    test::CaptureAudioSink refSink;
    auto refOwner = Access::makeManual(AudioKind::Chiptune, refSink);
    refOwner->play(tone);
    std::vector<AudioFrame> ref;
    driveCapturing(*refOwner, refSink, 30, ref);
    ASSERT_FALSE(ref.empty());

    // Dual: TWO voices of the same tone, cued before any stepping — both VMs start from reset, so both
    // produce the reference stream and the mix must be exactly clamp(2 × ref) at every position.
    test::CaptureAudioSink dualSink;
    auto dualOwner = Access::makeManual(AudioKind::Chiptune, dualSink);
    dualOwner->play(tone);
    dualOwner->play(tone);
    std::vector<AudioFrame> dual;
    driveCapturing(*dualOwner, dualSink, 30, dual);
    ASSERT_FALSE(dual.empty());

    const std::size_t n = std::min(ref.size(), dual.size());
    ASSERT_GT(n, 1000u);
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(dual[i].left, detail::clampMixedSample(2 * static_cast<std::int32_t>(ref[i].left)))
            << "at frame " << i;
        EXPECT_EQ(dual[i].right, detail::clampMixedSample(2 * static_cast<std::int32_t>(ref[i].right)))
            << "at frame " << i;
        if (HasFailure()) {
            break;  // one mismatch position is enough to diagnose
        }
    }
}

// ── stop() silences the whole system ──────────────────────────────────────────────────────────────
TEST(AudioMultiVoice, StopClosesEveryVoice) {
    setTonesRoot();
    test::CaptureAudioSink sink;
    auto audioOwner = Access::makeManual(AudioKind::Chiptune, sink);
    AudioSystem& audio = *audioOwner;
    const AudioId tone = AudioLibrary::instance().registerAudio("tone_sustain.asm", AudioType::Music,
                                                                Isa::Sm83, AssetPolicy::LoadFromPath);
    audio.play(tone);
    audio.play(tone);
    std::vector<AudioFrame> some;
    driveCapturing(audio, sink, 10, some);
    ASSERT_TRUE(audio.isPlaying());

    audio.stop();
    Access::step(audio);   // the release fades (~8 ms) are produced, then every voice closes
    EXPECT_FALSE(audio.isPlaying());
    sink.drain(1u << 20);  // drain what was produced, fade tails included
    Access::step(audio);   // an idle pass produces nothing new
    EXPECT_TRUE(sink.drain(1u << 20).empty()) << "voices kept producing after stop()";
}

// ── A closing voice fades — no truncation click ───────────────────────────────────────────────────
TEST(AudioMultiVoice, StopFadesToSilence) {
    setTonesRoot();
    test::CaptureAudioSink sink;
    auto audioOwner = Access::makeManual(AudioKind::Chiptune, sink);
    AudioSystem& audio = *audioOwner;
    const AudioId tone = AudioLibrary::instance().registerAudio("tone_sustain.asm", AudioType::Music,
                                                                Isa::Sm83, AssetPolicy::LoadFromPath);
    audio.play(tone);
    std::vector<AudioFrame> body;
    driveCapturing(audio, sink, 20, body);
    ASSERT_TRUE(audio.isPlaying());
    sink.drain(1u << 20);  // empty the ring so the next drain is exactly the post-stop output

    audio.stop();
    Access::step(audio);  // produces the release fade, then the voice closes
    EXPECT_FALSE(audio.isPlaying());
    const std::vector<AudioFrame> tail = sink.drain(1u << 20);
    ASSERT_FALSE(tail.empty()) << "stop() produced no release fade — a hard cut clicks";

    // The tail lands at (near) zero: the step into silence after it is inaudible.
    const AudioFrame& last = tail.back();
    EXPECT_LE(std::abs(static_cast<std::int32_t>(last.left)), 96) << "fade did not reach silence";
    EXPECT_LE(std::abs(static_cast<std::int32_t>(last.right)), 96) << "fade did not reach silence";
}

// ── PCM voices layer (exact sum) and finish independently ─────────────────────────────────────────
TEST(AudioMultiVoice, PcmVoicesLayerAndFinishIndependently) {
    const AudioId wav = AudioLibrary::instance().registerAudio(
        "tests/fixtures/tone.wav", AudioType::Sfx, AssetPolicy::Embed);  // no-ISA PCM door

    // Reference: ONE voice of the decoded tone, streamed to completion.
    test::CaptureAudioSink refSink;
    auto refOwner = Access::makeManual(AudioKind::Pcm, refSink);
    refOwner->play(wav);
    std::vector<AudioFrame> ref;
    driveCapturing(*refOwner, refSink, 50, ref);
    ASSERT_FALSE(ref.empty());
    EXPECT_FALSE(refOwner->isPlaying()) << "the one-shot PCM voice never finished";

    // Dual: TWO voices of the same tone cued together — the streams sum position-for-position, both
    // finish on their own, and the system goes idle by itself (no preemption, no lingering voice).
    test::CaptureAudioSink dualSink;
    auto dualOwner = Access::makeManual(AudioKind::Pcm, dualSink);
    dualOwner->play(wav);
    dualOwner->play(wav);
    ASSERT_TRUE(dualOwner->isPlaying());
    std::vector<AudioFrame> dual;
    driveCapturing(*dualOwner, dualSink, 50, dual);
    EXPECT_FALSE(dualOwner->isPlaying()) << "layered PCM voices never finished";
    ASSERT_EQ(dual.size(), ref.size());  // same tone, same length — layering adds no frames
    for (std::size_t i = 0; i < ref.size(); ++i) {
        EXPECT_EQ(dual[i].left, detail::clampMixedSample(2 * static_cast<std::int32_t>(ref[i].left)))
            << "at frame " << i;
        EXPECT_EQ(dual[i].right, detail::clampMixedSample(2 * static_cast<std::int32_t>(ref[i].right)))
            << "at frame " << i;
        if (HasFailure()) {
            break;  // one mismatch position is enough to diagnose
        }
    }

    // And a finished system re-cues cleanly.
    dualOwner->play(wav);
    EXPECT_TRUE(dualOwner->isPlaying());
    driveUntilIdleOrCap(*dualOwner, dualSink, 200);
    EXPECT_FALSE(dualOwner->isPlaying());
}

// ── Re-cueing layers instead of restarting ────────────────────────────────────────────────────────
TEST(AudioMultiVoice, RecueLayersANewVoice) {
    setTonesRoot();
    test::CaptureAudioSink sink;
    auto audioOwner = Access::makeManual(AudioKind::Chiptune, sink);
    AudioSystem& audio = *audioOwner;
    const AudioId blip = AudioLibrary::instance().registerAudio("sfx_blip.asm", AudioType::Sfx, Isa::Sm83,
                                                                AssetPolicy::LoadFromPath);
    audio.play(blip);
    std::vector<AudioFrame> some;
    driveCapturing(audio, sink, 3, some);
    ASSERT_TRUE(audio.isPlaying());

    audio.play(blip);  // rapid-fire the same id: a SECOND blip voice, the first plays on
    EXPECT_TRUE(audio.isPlaying());

    // Both one-shots decay and auto-close on their own; the system then goes idle by itself.
    driveUntilIdleOrCap(audio, sink, 4'000);
    EXPECT_FALSE(audio.isPlaying()) << "layered one-shots did not both auto-close";
}

// ── CueMode::Retrigger replaces only the same id ──────────────────────────────────────────────────
TEST(AudioMultiVoice, RetriggerReplacesOnlyTheSameId) {
    setTonesRoot();
    test::CaptureAudioSink sink;
    auto audioOwner = Access::makeManual(AudioKind::Chiptune, sink);
    AudioSystem& audio = *audioOwner;
    const AudioId music = AudioLibrary::instance().registerAudio("tone_sustain.asm", AudioType::Music,
                                                                 Isa::Sm83, AssetPolicy::LoadFromPath);
    const AudioId blip = AudioLibrary::instance().registerAudio("sfx_blip.asm", AudioType::Sfx, Isa::Sm83,
                                                                AssetPolicy::LoadFromPath);
    audio.play(music);
    audio.play(blip);
    std::vector<AudioFrame> some;
    driveCapturing(audio, sink, 3, some);
    ASSERT_TRUE(audio.isPlaying());

    // Re-fire the blip as a retrigger: the blip restarts; the music is NOT the same id and plays on.
    audio.play(blip, CueMode::Retrigger);
    EXPECT_TRUE(audio.isPlaying());
    std::vector<AudioFrame> post;
    driveCapturing(audio, sink, 400, post);  // past the restarted blip's decay + auto-close
    EXPECT_TRUE(audio.isPlaying()) << "Retrigger cut a DIFFERENT sound (the music) — it must only "
                                      "replace voices of the same id";
}

// ── CueMode::Retrigger restarts instead of layering ───────────────────────────────────────────────
TEST(AudioMultiVoice, RetriggerDoesNotDouble) {
    setTonesRoot();
    const AudioId tone = AudioLibrary::instance().registerAudio("tone_sustain.asm", AudioType::Music,
                                                                Isa::Sm83, AssetPolicy::LoadFromPath);

    // Reference amplitude: one voice of the sustained tone.
    test::CaptureAudioSink refSink;
    auto refOwner = Access::makeManual(AudioKind::Chiptune, refSink);
    refOwner->play(tone);
    std::vector<AudioFrame> ref;
    driveCapturing(*refOwner, refSink, 30, ref);
    std::int32_t refPeak = 0;
    for (const AudioFrame& f : ref) {
        refPeak = std::max({refPeak, std::abs(static_cast<std::int32_t>(f.left)),
                            std::abs(static_cast<std::int32_t>(f.right))});
    }
    ASSERT_GT(refPeak, 0);

    // Retrigger the same tone mid-play: ONE voice remains (restarted), so the output never exceeds the
    // single-voice peak. A layered second copy would double it.
    test::CaptureAudioSink sink;
    auto owner = Access::makeManual(AudioKind::Chiptune, sink);
    owner->play(tone);
    std::vector<AudioFrame> head;
    driveCapturing(*owner, sink, 10, head);
    owner->play(tone, CueMode::Retrigger);
    std::vector<AudioFrame> post;
    driveCapturing(*owner, sink, 30, post);
    ASSERT_GT(post.size(), 1'000u);
    // Skip the release window: the replaced voice fades out (~8 ms ≈ 384 frames) under the fresh
    // attack, so the very start may briefly sum above a single voice. Past it, ONE voice remains.
    post.erase(post.begin(), post.begin() + 600);
    for (const AudioFrame& f : post) {
        ASSERT_LE(std::abs(static_cast<std::int32_t>(f.left)), refPeak) << "retrigger layered instead";
        ASSERT_LE(std::abs(static_cast<std::int32_t>(f.right)), refPeak) << "retrigger layered instead";
    }
}

}  // namespace
}  // namespace retropp
