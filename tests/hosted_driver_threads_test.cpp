// Hosted machines on their own threads: several resident sound drivers running at once on one system,
// and what happens to them when one is closed or the whole system goes away underneath them.
//
// A resident machine publishes its declared slots after every step, so unlike a cued voice it reaches
// for something on every single step — which makes it the case that finds anything the teardown path
// releases too early. The drivers here are hand-assembled SM83 (NO ROM), one tone each, so the machines
// are real machines and every one costs what every other costs.
#include "retropp/audio_system.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/audio_library.h"
#include "retropp/driver_binding.h"
#include "retropp/gb.h"
#include "retropp/vm.h"
#include "mock_platform.h"  // test::CaptureAudioSink

namespace retropp {
namespace {

using namespace std::chrono_literals;

// The game-facing slot struct: one readable byte, so every step has a slot to publish.
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

// Silence CH3 by switching its DAC off.
const char* kStopSource = R"(
  xor a
  ldh [$FF1A], a
  ret
)";

// One registration per machine: hosting each id gives it a machine of its own.
DriverId<ToneSlots> registerToneDriver() {
    const std::vector<std::uint8_t> init = assemble(kInitSource);
    const std::vector<std::uint8_t> tick = assemble(kTickSource);
    const std::vector<std::uint8_t> stop = assemble(kStopSource);
    DriverBinding binding;
    binding.images    = {DriverImage{.bytes = init, .base = 0x6000},
                         DriverImage{.bytes = tick, .base = 0x6100},
                         DriverImage{.bytes = stop, .base = 0x6200}};
    binding.tickEntry = 0x6100;
    binding.init      = Instruction::call(0x6000, gb::A, /*fixedValue=*/0);
    const DriverVerbs verbs{
        .play = {.music = Instruction::write(Location::memory(0xC010), 1)},
        .stop = Instruction::call(0x6200, gb::A, /*fixedValue=*/0),
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

template <typename Predicate>
bool waitFor(Predicate done, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return done();
}

// The same wait, with the output consuming the way a device does. A machine runs ahead only until the
// frames waiting downstream of it come to the latency target, so an output that takes nothing holds
// every machine parked at that target — including a machine hosted after the buffer filled, which is
// the one a case about many machines arriving at once turns on.
template <typename Predicate>
bool waitWhileDraining(Predicate done, test::CaptureAudioSink& sink, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        sink.drain(1u << 16);
        std::this_thread::sleep_for(1ms);
    }
    return done();
}

// Host `count` machines on `audio`, each on its own pitch, and return the handles.
std::vector<HostedDriver<ToneSlots>> hostTones(AudioSystem& audio, std::size_t count) {
    std::vector<DriverId<ToneSlots>>&    ids = toneIds(count);
    std::vector<HostedDriver<ToneSlots>> live;
    for (std::size_t i = 0; i < count; ++i) {
        live.push_back(audio.host(ids[i]));
        live.back().play(static_cast<std::uint64_t>(0x40 + 0x08 * i));
    }
    return live;
}

// The whole system goes away while every machine is mid-step. Closing a voice releases its hold on its
// runner before it waits for that runner's thread, so whatever the thread reaches for during the wait
// has to be something the thread holds itself — a machine reached through the voice would be gone for
// exactly as long as the wait lasts, on every step the other machines take meanwhile. Reaching the end
// without a crash is the signal.
TEST(HostedDriverThreads, ASystemWithManyMachinesTearsDownWhileTheyRun) {
    for (int round = 0; round < 3; ++round) {
        test::CaptureAudioSink sink;
        AudioSystem            audio{AudioKind::Chiptune, sink};
        const std::vector<HostedDriver<ToneSlots>> live = hostTones(audio, 6);

        ASSERT_TRUE(waitFor([&] { return audio.audioStats().framesBuffered > 0; }, 3000ms))
            << "the machines produced nothing in round " << round;
        sink.drain(1u << 16);
        // and out of scope, with all six still running
    }
    SUCCEED();
}

// Every machine takes delivery of the verb issued to it, however quickly the machines were hosted.
// host() and the verbs that follow it are one ordered sequence on the game thread but reach the audio
// thread through two channels, so a verb can arrive while its machine is still crossing. Each machine
// here reports through its own readable slot what pitch it last played, which is the machine's own
// answer rather than the caller's.
TEST(HostedDriverThreads, EveryMachineTakesDeliveryOfItsVerb) {
    // Enough machines that hosting them is still in flight while the verbs are being applied — with a
    // handful the crossing is over before the first verb arrives and the case proves nothing.
    constexpr std::size_t kMachines = 48;
    test::CaptureAudioSink               sink;
    AudioSystem                          audio{AudioKind::Chiptune, sink};
    std::vector<DriverId<ToneSlots>>&    ids = toneIds(kMachines);
    std::vector<HostedDriver<ToneSlots>> live;

    // Each machine is played the moment it is hosted, with no pause anywhere — the way a game adds one.
    for (std::size_t i = 0; i < kMachines; ++i) {
        live.push_back(audio.host(ids[i]));
        live[i].play(static_cast<std::uint64_t>(0x30 + 0x08 * i));
    }

    const auto allPlaying = [&] {
        for (std::size_t i = 0; i < kMachines; ++i) {
            const std::optional<std::uint8_t> pitch = live[i].slots().lastPitch;
            if (!pitch.has_value() || *pitch != static_cast<std::uint8_t>(0x30 + 0x08 * i)) {
                return false;
            }
        }
        return true;
    };
    EXPECT_TRUE(waitWhileDraining(allPlaying, sink, 3000ms))
        << "a machine never received the pitch it was played";
}

// One machine closes while the others play on: the same window, entered one machine at a time while
// the system stays alive around it.
TEST(HostedDriverThreads, ClosingOneMachineLeavesTheOthersRunning) {
    test::CaptureAudioSink              sink;
    AudioSystem                         audio{AudioKind::Chiptune, sink};
    std::vector<HostedDriver<ToneSlots>> live = hostTones(audio, 4);
    ASSERT_TRUE(waitFor([&] { return audio.audioStats().framesBuffered > 0; }, 3000ms));

    for (int i = 0; i < 2; ++i) {
        live.back().close();
        live.pop_back();
        std::this_thread::sleep_for(20ms);
        sink.drain(1u << 16);
    }

    EXPECT_TRUE(waitFor([&] { return audio.audioStats().framesBuffered > 0; }, 3000ms))
        << "the surviving machines stopped producing";
}

}  // namespace
}  // namespace retropp
