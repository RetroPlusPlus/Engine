// The mix never waits on a machine. Each pass advances by as many frames as the most-advanced voice
// produced; a voice with fewer contributes silence for the shortfall and that shortfall is counted
// against it, so a machine that falls behind mutes itself and nothing else, and its late frames still
// play — after a gap, intact.
//
// Device-free: CaptureAudioSink + the internal synchronous seam, whose selective stepping advances one
// voice at a time. Stepping the voices unevenly is what produces a short lane deterministically, without
// waiting on a real machine to fall behind a real one.
#include "retropp/audio_system.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <thread>
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

// Wait for something the machines' own threads bring about.
template <typename Predicate>
bool waitFor(Predicate done, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return done();
}

AudioId sustainedTone() {
    return AudioLibrary::instance().registerAudio("tone_sustain.asm", AudioType::Music, Isa::Sm83,
                                                  AssetPolicy::LoadFromPath);
}

// What one voice produces, step by step, on a system with nothing to substitute for — the reference a
// starved mix is measured against. Every machine is deterministic and starts from reset, so a second
// voice of the same tone produces this same sequence.
std::vector<std::vector<AudioFrame>> soloSteps(int steps) {
    setTonesRoot();
    test::CaptureAudioSink            sink;
    const std::unique_ptr<AudioSystem> audio = Access::makeManual(AudioKind::Chiptune, sink);
    audio->play(sustainedTone());
    std::vector<std::vector<AudioFrame>> out;
    for (int i = 0; i < steps; ++i) {
        Access::stepVoice(*audio, 0);
        Access::mix(*audio);
        out.push_back(sink.drain(kDrainAll));
    }
    return out;
}

class LaneUnderflow : public ::testing::Test {
protected:
    void SetUp() override {
        setTonesRoot();
        audio = Access::makeManual(AudioKind::Chiptune, sink);
    }

    // Two voices of the same tone, so each one's own audio is the reference for what the other's
    // silence must leave untouched.
    void playTwo() {
        const AudioId tone = sustainedTone();
        audio->play(tone);
        audio->play(tone);
    }

    std::vector<AudioFrame> mixAndDrain() {
        Access::mix(*audio);
        return sink.drain(kDrainAll);
    }

    test::CaptureAudioSink       sink;
    std::unique_ptr<AudioSystem> audio;
};

// A voice that produced nothing this pass contributes silence, and what reaches the ring is the
// running voice's own audio, unchanged. The shortfall is counted against the voice that owed it.
TEST_F(LaneUnderflow, AStarvedLaneContributesSilenceAndTheMixAdvances) {
    const std::vector<std::vector<AudioFrame>> solo = soloSteps(2);
    ASSERT_FALSE(solo[0].empty());
    ASSERT_FALSE(solo[1].empty());

    playTwo();
    ASSERT_EQ(Access::voiceCount(*audio), 2u);

    // Both run once, so both have produced and what follows measures starvation rather than startup.
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 1);
    ASSERT_EQ(mixAndDrain().size(), solo[0].size());

    // Now only the first one runs.
    Access::stepVoice(*audio, 0);
    const std::vector<AudioFrame> alone = mixAndDrain();

    ASSERT_EQ(alone.size(), solo[1].size());
    for (std::size_t i = 0; i < alone.size(); ++i) {
        EXPECT_EQ(alone[i].left, solo[1][i].left) << "at frame " << i;
        EXPECT_EQ(alone[i].right, solo[1][i].right) << "at frame " << i;
        if (HasFailure()) {
            break;  // one mismatch position is enough to diagnose
        }
    }
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 1), alone.size());
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), 0u);
}

// The starved voice's stream resumes exactly where it left off: the gap is inserted, nothing is
// dropped. Its next frames are the ones a voice that never fell behind would have produced.
TEST_F(LaneUnderflow, TheLateFramesPlayIntactAfterTheGap) {
    const std::vector<std::vector<AudioFrame>> solo = soloSteps(2);
    ASSERT_FALSE(solo[1].empty());

    playTwo();
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 1);
    ASSERT_FALSE(mixAndDrain().empty());

    Access::stepVoice(*audio, 0);  // the second voice misses this pass
    ASSERT_FALSE(mixAndDrain().empty());

    Access::stepVoice(*audio, 1);  // and catches up on the next one
    const std::vector<AudioFrame> late = mixAndDrain();

    ASSERT_EQ(late.size(), solo[1].size());
    for (std::size_t i = 0; i < late.size(); ++i) {
        EXPECT_EQ(late[i].left, solo[1][i].left) << "at frame " << i;
        EXPECT_EQ(late[i].right, solo[1][i].right) << "at frame " << i;
        if (HasFailure()) {
            break;
        }
    }
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), late.size());  // this pass, the first one owed
}

// When no voice has produced anything, the pass produces nothing. Silence is substituted for a voice
// that is behind the others, never for the whole mix being behind the ring's appetite — frames that
// are merely not produced yet are waited for, and the ring's own underflow covers the wait.
TEST_F(LaneUnderflow, EveryLaneEmptyProducesNothing) {
    playTwo();
    ASSERT_EQ(audio->framesBuffered(), 0u);

    Access::mix(*audio);

    EXPECT_EQ(audio->framesBuffered(), 0u);
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), 0u);
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 1), 0u);
}

// A voice that has not produced its first frame is still building its machine. The count measures
// starvation, not startup, so it begins at that first frame.
TEST_F(LaneUnderflow, AVoiceThatHasNotProducedYetIsNotCounted) {
    playTwo();

    Access::stepVoice(*audio, 0);
    EXPECT_FALSE(mixAndDrain().empty());

    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 1), 0u);
}

// Running further ahead than a neighbour is not starving the neighbour. Machines on their own clocks
// run ahead in whole steps, so at any moment one of them holds a step more than another; the mix keeps
// the OUTPUT's pace, and every voice stocked past that pace delivers in full. Pacing off the deepest
// lane instead would read every shallower voice as starving and pad it with a step of silence — worse
// the more machines there are, until the mix is mostly silence.
TEST_F(LaneUnderflow, ADeeperLaneDoesNotStarveAShallowerOne) {
    playTwo();

    Access::stepVoice(*audio, 0);  // one voice a whole step further ahead than the other
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 1);

    // The output wants a fraction of a step; both voices hold at least that much.
    const std::size_t wanted = 200;
    Access::mix(*audio, wanted);

    EXPECT_EQ(sink.drain(kDrainAll).size(), wanted);
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), 0u);
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 1), 0u);
}

// With one voice there is nothing to be behind: the substitution is inert by construction, which is
// what keeps a lone sound's bytes exactly as its machine produced them.
TEST_F(LaneUnderflow, ALoneVoiceNeverSubstitutes) {
    audio->play(sustainedTone());

    for (int i = 0; i < 8; ++i) {
        Access::stepVoice(*audio, 0);
        ASSERT_FALSE(mixAndDrain().empty()) << "at pass " << i;
    }

    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), 0u);
}

// ── On real threads ───────────────────────────────────────────────────────────────────────────────
// The cases above pin the substitution RULE; this one pins the pace the threaded produce pass keeps it
// at. Several machines run on their own threads while the sink pulls at the device's rate: each machine
// stays stocked well past what the output asks for per pass, so the mix substitutes for none of them.
// A pass that took whatever the machines had instead would read all but the deepest as starving, and
// the shortfall would grow with the number of machines until the mix was mostly silence.
TEST(LaneUnderflowThreaded, MachinesOnTheirOwnThreadsDoNotStarveEachOther) {
    using namespace std::chrono_literals;
    setTonesRoot();
    test::CaptureAudioSink sink;
    AudioSystem            audio{AudioKind::Chiptune, sink};  // a production thread, and a thread each

    const AudioId tone = sustainedTone();
    for (int i = 0; i < 4; ++i) {
        audio.play(tone);
    }
    ASSERT_TRUE(waitFor([&] { return audio.framesBuffered() > 0; }, 2000ms))
        << "the machines produced nothing";

    // Pull at just under the device's rate for half a second, so the ring stays fed and what the mix
    // does is paced by the output rather than by a starving consumer.
    std::size_t drained = 0;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(10ms);
        drained += sink.drain(400).size();
    }

    ASSERT_GT(drained, std::size_t{5000}) << "the output barely ran";
    EXPECT_LT(Access::laneUnderflowTotal(audio), drained / 10)
        << "machines were read as starving while they were merely at different points";
}

// Tearing a system down while several machines are mid-step. Closing a voice releases its hold on its
// runner before it waits for that runner's thread, so anything the thread reaches for during the wait
// must be something it holds itself rather than something the voice was holding for it. Reaching the
// end without a crash is the signal.
TEST(LaneUnderflowThreaded, ASystemWithManyMachinesTearsDownWhileTheyRun) {
    using namespace std::chrono_literals;
    setTonesRoot();
    for (int round = 0; round < 3; ++round) {
        test::CaptureAudioSink sink;
        AudioSystem            audio{AudioKind::Chiptune, sink};
        const AudioId          tone = sustainedTone();
        for (int i = 0; i < 6; ++i) {
            audio.play(tone);
        }
        ASSERT_TRUE(waitFor([&] { return audio.framesBuffered() > 0; }, 2000ms))
            << "the machines produced nothing in round " << round;
        sink.drain(kDrainAll);
        // and out of scope, with every machine still running
    }
    SUCCEED();
}

}  // namespace
}  // namespace retropp
