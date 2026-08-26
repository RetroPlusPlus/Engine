// What a hosted machine's handle reports about its own starvation.
//
// A machine that falls behind the output mutes itself and nothing else: the mix substitutes silence for
// the frames it owed and counts them against that machine. `HostedDriver::underflowFrames()` is how the
// game asks how ITS machine did — one number per machine, distinct from AudioStats::outputUnderflow,
// which is the whole mix arriving late at the device.
//
// Device-free: hand-assembled SM83 drivers (NO ROM) hosted on a manual system, whose internal seam
// advances one machine at a time. Stepping the machines unevenly is what starves one of them exactly,
// without waiting on a real machine to fall behind a real one.
#include "retropp/audio_system.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/audio_library.h"
#include "retropp/driver_binding.h"
#include "retropp/gb.h"
#include "retropp/vm.h"
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — synchronous production seam
#include "mock_platform.h"                    // test::CaptureAudioSink

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;

// More frames than any pass here produces, so a drain always takes everything the output holds.
constexpr std::size_t kDrainAll = 1u << 20;

// The game-facing slot struct: one readable byte, so every step has a slot to publish — a resident
// machine reaches for its snapshot on every step, which is where its starvation count rides.
struct ToneSlots {
    std::optional<std::uint8_t> lastPitch;  // 0: $C020 — the pitch this machine last played
};

std::vector<std::uint8_t> assemble(const std::string& source) {
    Vm probe{VMPlatform::GameBoyColor};
    return probe.assemble(source);
}

// Zero the driver's state RAM, enable the APU, load a wave into CH3 and switch its DAC on. Zeroing
// first is not optional: a machine powers on with work RAM already holding values, and a garbage
// mailbox reads as a spurious play on the very first tick.
const char* kInitSource = R"(
  ld hl, $C010
  ld c, $21
  xor a
.clr:
  ld [hl+], a
  dec c
  jr nz, .clr
  ld a, $80
  ldh [$FF26], a
  xor a
  ldh [$FF1A], a
  ld a, $01
  ldh [$FF30], a
  ld a, $23
  ldh [$FF31], a
  ld a, $45
  ldh [$FF32], a
  ld a, $67
  ldh [$FF33], a
  ld a, $89
  ldh [$FF34], a
  ld a, $AB
  ldh [$FF35], a
  ld a, $CD
  ldh [$FF36], a
  ld a, $EF
  ldh [$FF37], a
  ld a, $FF
  ldh [$FF25], a
  ld a, $44
  ldh [$FF24], a
  ld a, $80
  ldh [$FF1A], a
  ld a, $20
  ldh [$FF1C], a
  ret
)";

// Poll the pitch mailbox; when it carries one, record it in the readable slot, trigger CH3 there, and
// clear the mailbox so the trigger happens once. The tone then drones until the next play.
const char* kTickSource = R"(
  ld a, [$C010]
  or a
  jr z, .done
  ld [$C020], a
  ld b, a
  ld a, $80
  ldh [$FF1A], a
  ld a, b
  ldh [$FF1D], a
  ld a, $86
  ldh [$FF1E], a
  xor a
  ld [$C010], a
.done:
  ret
)";

// One registration per machine: hosting each id gives it a machine of its own.
DriverId<ToneSlots> registerToneDriver() {
    const std::vector<std::uint8_t> init = assemble(kInitSource);
    const std::vector<std::uint8_t> tick = assemble(kTickSource);
    DriverBinding binding;
    binding.images    = {DriverImage{.bytes = init, .base = 0x6000},
                         DriverImage{.bytes = tick, .base = 0x6100}};
    binding.tickEntry = 0x6100;
    binding.init      = Instruction::call(0x6000, gb::A, /*fixedValue=*/0);
    const DriverVerbs verbs{
        .play = {.music = Instruction::write(Location::memory(0xC010), 1)},
    };
    return AudioLibrary::instance().uploadDriver(
        binding, verbs, slots(slot(&ToneSlots::lastPitch, 0xC020, SlotDirection::Read)));
}

// Ids live for the whole file: a registration is minted once and can be hosted on any system.
std::vector<DriverId<ToneSlots>>& toneIds(std::size_t atLeast) {
    static std::vector<DriverId<ToneSlots>> ids;
    while (ids.size() < atLeast) {
        ids.push_back(registerToneDriver());
    }
    return ids;
}

class HostedDriverUnderflow : public ::testing::Test {
protected:
    void SetUp() override { audio = Access::makeManual(AudioKind::Chiptune, sink); }

    // Host `count` machines, each on its own pitch, in the order their voices are built. The machines
    // themselves are built by the first step below — every seam call takes delivery of the hosted ones
    // first — so nothing here produces a frame, and a machine that is never stepped never produces one.
    std::vector<HostedDriver<ToneSlots>> hostTones(std::size_t count) {
        std::vector<DriverId<ToneSlots>>&    ids = toneIds(count);
        std::vector<HostedDriver<ToneSlots>> live;
        for (std::size_t i = 0; i < count; ++i) {
            live.push_back(audio->host(ids[i]));
            live.back().play(static_cast<std::uint32_t>(0x40 + 0x08 * i));
        }
        return live;
    }

    // Put the output above the level where the mix stops waiting, out of both machines' own frames, and
    // leave the two lanes level and empty. Both have produced by the end of it, so what follows measures
    // how a straggler is treated rather than how a machine starting up is.
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

// A machine that keeps up reads zero, however long it plays: there is nothing to substitute for while
// its lane has the frames the mix asks for.
TEST_F(HostedDriverUnderflow, AMachineThatKeepsUpReportsNoStarvation) {
    const std::vector<HostedDriver<ToneSlots>> live = hostTones(1);

    for (int pass = 0; pass < 8; ++pass) {
        Access::stepVoice(*audio, 0);
        Access::mix(*audio);
        ASSERT_FALSE(sink.drain(kDrainAll).empty()) << "at pass " << pass;
    }

    EXPECT_EQ(live[0].underflowFrames(), 0u);
}

// A machine that falls behind reports the silence the mix substituted for it — the same frames the
// engine counted against its voice, read through the handle the game holds.
TEST_F(HostedDriverUnderflow, AStarvedMachineReportsTheSilenceSubstitutedForIt) {
    const std::vector<HostedDriver<ToneSlots>> live = hostTones(2);
    fillPastTheWaitingFloor();
    ASSERT_EQ(Access::voiceCount(*audio), 2u);

    // One machine runs a whole step further than the other, and the output is down to its floor — so
    // the pass advances by what the deeper lane holds and the straggler is silent for the difference.
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 1);
    const std::size_t deep    = Access::laneFrames(*audio, 0);
    const std::size_t shallow = Access::laneFrames(*audio, 1);
    ASSERT_GT(deep, shallow);
    sink.drain(kDrainAll);
    ASSERT_LE(audio->audioStats().framesBuffered, Access::waitingFloor(*audio));

    Access::mix(*audio, deep);

    EXPECT_EQ(live[1].underflowFrames(), deep - shallow);
    EXPECT_EQ(live[1].underflowFrames(), Access::laneUnderflowFrames(*audio, 1))
        << "the handle and the engine disagree about the same machine";
}

// The count is the machine's own. A machine playing beside a starved one reports nothing, which is what
// makes the number worth asking for: it names WHICH machine is not keeping up.
TEST_F(HostedDriverUnderflow, AMachineIsNotChargedForItsNeighboursSilence) {
    const std::vector<HostedDriver<ToneSlots>> live = hostTones(2);
    fillPastTheWaitingFloor();

    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 1);
    const std::size_t deep = Access::laneFrames(*audio, 0);
    sink.drain(kDrainAll);

    Access::mix(*audio, deep);

    EXPECT_GT(live[1].underflowFrames(), 0u) << "the straggler was not starved, so nothing is proved";
    EXPECT_EQ(live[0].underflowFrames(), 0u);
}

// A machine still building itself is not lagging, so nothing is counted against it: the count begins at
// the first frame it produces.
TEST_F(HostedDriverUnderflow, AMachineThatHasNotProducedYetReportsNothing) {
    const std::vector<HostedDriver<ToneSlots>> live = hostTones(2);

    // Only the first machine runs; the second has never been stepped.
    Access::stepVoice(*audio, 0);
    Access::mix(*audio);
    ASSERT_FALSE(sink.drain(kDrainAll).empty());

    EXPECT_EQ(live[1].underflowFrames(), 0u);
}

// A closed machine is gone, and its handle is spent: the count reads zero rather than the last figure
// the machine happened to reach.
TEST_F(HostedDriverUnderflow, AClosedMachineReportsNothing) {
    const std::vector<HostedDriver<ToneSlots>> live = hostTones(2);
    fillPastTheWaitingFloor();

    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 1);
    const std::size_t deep = Access::laneFrames(*audio, 0);
    sink.drain(kDrainAll);
    Access::mix(*audio, deep);
    ASSERT_GT(live[1].underflowFrames(), 0u);

    live[1].close();

    EXPECT_EQ(live[1].underflowFrames(), 0u);
}

}  // namespace
}  // namespace retropp
