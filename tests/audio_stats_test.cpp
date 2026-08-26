// What AudioSystem::audioStats() reports, and what keeps its two underflow counts apart.
//
// One struct answers how the audio path is doing: the queue depth right now, and three running totals.
// The two underflow fields are the reason the struct exists — `outputUnderflow` is the device asking for
// frames the ring did not have, `laneUnderflow` is the mix substituting silence for a machine that was
// late — so a case here starves each one on its own and asserts the other does not move.
//
// Device-free: CaptureAudioSink + the internal synchronous seam, whose selective stepping advances one
// voice at a time. Stepping the voices unevenly starves a lane deterministically, and pulling the sink
// past what the ring holds starves the output, both without a device and without waiting on a real
// machine to fall behind a real one.
#include "retropp/audio_system.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/asset_policy.h"    // AssetPolicy::LoadFromPath
#include "retropp/asset_registry.h"  // setAssetRoot — the single LoadFromPath base for the literal names
#include "retropp/audio_library.h"   // AudioLibrary, AudioType, AudioId
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — the synchronous seam
#include "mock_platform.h"                   // test::CaptureAudioSink

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;

// More frames than any pass here produces, so a drain always takes everything the ring holds.
constexpr std::size_t kDrainAll = 1u << 20;

void setTonesRoot() { setAssetRoot(std::filesystem::path(RETROPP_ASSETS_DIR) / "tones"); }

AudioId sustainedTone() {
    return AudioLibrary::instance().registerAudio("tone_sustain.asm", AudioType::Music, Isa::Sm83,
                                                  AssetPolicy::LoadFromPath);
}

class AudioStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        setTonesRoot();
        audio = Access::makeManual(AudioKind::Chiptune, sink);
    }

    // Two voices of the same tone: one runs a step further than the other, so the mix has a straggler to
    // substitute for.
    void playTwo() {
        const AudioId tone = sustainedTone();
        audio->play(tone);
        audio->play(tone);
    }

    // Put the output above the level where the mix stops waiting, out of both voices' own frames, and
    // leave the two lanes level. Both voices have produced by the end of it, so what follows measures a
    // straggler rather than a machine starting up.
    void fillPastTheWaitingFloor() {
        for (int i = 0; i < 2; ++i) {
            Access::stepVoice(*audio, 0);
            Access::stepVoice(*audio, 1);
        }
        Access::mix(*audio, Access::laneFrames(*audio, 0));
        ASSERT_GT(audio->audioStats().framesBuffered, Access::waitingFloor(*audio));
    }

    test::CaptureAudioSink       sink;
    std::unique_ptr<AudioSystem> audio;
};

// A system that has produced nothing reports nothing: every field starts at zero, including the queue
// depth, so a game reading the struct before anything plays gets an answer rather than uninitialized
// memory.
TEST_F(AudioStatsTest, AFreshSystemReportsFourZeros) {
    const AudioStats stats = audio->audioStats();

    EXPECT_EQ(stats.framesBuffered, 0u);
    EXPECT_EQ(stats.framesDropped, 0u);
    EXPECT_EQ(stats.outputUnderflow, 0u);
    EXPECT_EQ(stats.laneUnderflow, 0u);
}

// `framesBuffered` is what the sink is about to get: the depth it reports is exactly the number of frames
// the next full drain returns. Every produced frame survives to that drain, because one produce pass
// fills toward the latency target and stops there, well inside the ring's capacity.
TEST_F(AudioStatsTest, TheDepthItReportsIsWhatTheSinkThenDrains) {
    audio->play(sustainedTone());
    Access::step(*audio);

    const AudioStats stats = audio->audioStats();
    ASSERT_GT(stats.framesBuffered, 0u) << "the produce pass put nothing in the ring, so nothing is proved";
    EXPECT_EQ(stats.framesDropped, 0u);

    EXPECT_EQ(sink.drain(kDrainAll).size(), stats.framesBuffered);
}

// `laneUnderflow` is the per-machine starvation summed. The engine counts a shortfall against the voice
// that owed it; the field is the same frames, added up, so a game with no per-machine handle still sees
// that machines are falling behind.
TEST_F(AudioStatsTest, LaneUnderflowSumsWhatEveryVoiceWasCharged) {
    playTwo();
    fillPastTheWaitingFloor();

    // One voice runs a whole step further than the other, and the output is drained to its floor — so the
    // pass advances by what the deeper lane holds and the straggler is silent for the difference.
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 1);
    const std::size_t deep = Access::laneFrames(*audio, 0);
    ASSERT_GT(deep, Access::laneFrames(*audio, 1));
    sink.drain(kDrainAll);
    ASSERT_LE(audio->audioStats().framesBuffered, Access::waitingFloor(*audio));

    Access::mix(*audio, deep);

    const std::size_t charged =
        Access::laneUnderflowFrames(*audio, 0) + Access::laneUnderflowFrames(*audio, 1);
    ASSERT_GT(charged, 0u) << "no voice was starved, so the sum proves nothing";
    EXPECT_EQ(audio->audioStats().laneUnderflow, charged);
}

// The two underflow counts are different shortfalls, and this is the case that says so. The sink asks for
// more than the ring holds — the device going hungry — while every machine has delivered everything the
// mix asked of it. `outputUnderflow` takes exactly the shortfall; `laneUnderflow` does not move.
TEST_F(AudioStatsTest, AHungryOutputMovesOutputUnderflowAndLeavesLaneUnderflowAlone) {
    audio->play(sustainedTone());
    Access::step(*audio);

    const AudioStats before = audio->audioStats();
    ASSERT_GT(before.framesBuffered, 0u);

    // Ask for a fixed amount past what the ring holds, so the shortfall is an exact number rather than
    // whatever a pass happened to leave.
    const std::size_t overdraft = 500;
    const std::size_t asked     = before.framesBuffered + overdraft;
    const std::size_t got       = sink.drain(asked).size();
    ASSERT_EQ(got, before.framesBuffered) << "the sink got frames the ring was not holding";

    const AudioStats after = audio->audioStats();
    EXPECT_EQ(after.outputUnderflow - before.outputUnderflow, asked - got);
    EXPECT_EQ(after.laneUnderflow, before.laneUnderflow)
        << "a hungry device was charged to the machines, which had delivered in full";
}

}  // namespace
}  // namespace retropp
