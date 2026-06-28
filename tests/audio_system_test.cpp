// ENG-4.A / ENG-4.D.1 — the AudioSystem end-to-end, device-free. Drives the production chain
// (retropp/audio_system.h) against a headless CaptureAudioSink: register the built-in diagnostic tone,
// play it, produce, and inspect the PCM. This is the red→green proof that the hardware-speed throttle is
// realized — registering a HardwareSpeed driver threw before ENG-4.A; here it produces real samples.
// No Vm, no Routine, no throttle appears in the test — proof the VM is fully hidden behind audio terms.
//
// ENG-4.D.1 relocated production onto a dedicated thread, so the game no longer steps audio (tick() is
// gone). The deterministic buffer-level tests drive production SYNCHRONOUSLY through the internal test
// seam (AudioSystemTestAccess::makeManual + step — the thread suppressed, production by hand), exactly as
// tick() used to. The owned-sink OWNERSHIP tests stay on the real threaded ctor (2) and poll the
// autonomous producer, since their subject IS the owning + threaded teardown path.
#include "retropp/audio_system.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/audio.h"
#include "retropp/gb_audio.h"  // sameboy::diagnosticTone (GB preset)
#include "retropp/timing.h"
#include "retropp/vm.h"
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — synchronous production seam
#include "mock_platform.h"                    // test::CaptureAudioSink

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;

// Count frames whose left or right sample is non-zero — a silent stream is all zeros.
std::size_t nonSilentCount(const std::vector<AudioFrame>& frames) {
    std::size_t n = 0;
    for (const AudioFrame& f : frames) {
        if (f.left != 0 || f.right != 0) {
            ++n;
        }
    }
    return n;
}

// Wait (bounded) for the autonomous production thread to fill the ring toward `atLeast`. Used by the
// threaded owned-sink tests; the deterministic tests use the manual seam instead and never poll.
bool waitForBuffered(const AudioSystem& a, std::size_t atLeast, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (a.framesBuffered() < atLeast && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return a.framesBuffered() >= atLeast;
}

// The output buffer is kept around a small latency target (~sampleRate / 20 ≈ 50 ms). A primed buffer
// is at least a few tens of ms; a bounded one stays well under the ring capacity (it never piles up).
constexpr std::size_t kPrimedLow   = kAudioSampleRate / 40;  // ~25 ms — definitely primed
constexpr std::size_t kBoundedHigh = kAudioSampleRate / 8;   // ~125 ms — bounded, not piled up

TEST(AudioSystem, OpensTheSinkAtTheConfiguredRate) {
    test::CaptureAudioSink sink;
    auto audio = Access::makeManual(AudioKind::Chiptune, sink);  // Game Boy Color default, 48 kHz (thread suppressed)
    EXPECT_TRUE(sink.started());
    EXPECT_EQ(sink.rate(), kAudioSampleRate);
    EXPECT_EQ(sink.channels(), kAudioChannels);
}

TEST(AudioSystem, ProducesNothingUntilSomethingPlays) {
    test::CaptureAudioSink sink;
    auto audio = Access::makeManual(AudioKind::Chiptune, sink);
    Access::step(*audio);  // no driver registered/playing → no production
    EXPECT_EQ(audio->framesBuffered(), 0u);
}

// The headline red→green: a hardware-speed driver, registered through the audio surface and played,
// produces non-silent PCM. (Before ENG-4.A, registering a HardwareSpeed routine threw —
// sameboy::diagnosticTone would have thrown here.) One produce pass primes the latency buffer.
TEST(AudioSystem, DiagnosticToneProducesNonSilentPcm) {
    test::CaptureAudioSink sink;
    auto audio = Access::makeManual(AudioKind::Chiptune, sink);
    const AudioId tone = sameboy::diagnosticTone();
    audio->play(tone);

    Access::step(*audio);  // the deficit is the whole target → the buffer primes to ~its latency target
    const std::size_t buffered = audio->framesBuffered();
    EXPECT_GE(buffered, kPrimedLow);
    EXPECT_LE(buffered, kBoundedHigh);

    // Pull the produced PCM and confirm it is an actual waveform, not silence.
    const std::vector<AudioFrame> produced = sink.drain(buffered);
    EXPECT_GT(produced.size(), kPrimedLow);
    EXPECT_GT(nonSilentCount(produced), std::size_t{100});  // a real waveform, not a flat line
}

// Production tracks the buffer level, not a fixed per-pass amount: producing repeatedly without the
// device draining tops the buffer up to the target ONCE and then stops — it stays bounded and never
// overflows. (The old fixed-budget model piled up and would overflow here.)
TEST(AudioSystem, RefillStaysBoundedAndNeverOverflows) {
    test::CaptureAudioSink sink;
    auto audio = Access::makeManual(AudioKind::Chiptune, sink);
    const AudioId tone = sameboy::diagnosticTone();
    audio->play(tone);

    for (int i = 0; i < 100; ++i) {
        Access::step(*audio);
    }
    EXPECT_GE(audio->framesBuffered(), kPrimedLow);    // primed
    EXPECT_LE(audio->framesBuffered(), kBoundedHigh);  // bounded — did not pile up over 100 passes
    EXPECT_EQ(audio->framesDropped(), 0u);             // never overflowed the ring
}

// After the device drains the buffer, the next produce pass sees the full deficit and refills it — so a
// drain never leaves the stream permanently starved (the drift/underrun self-correction).
TEST(AudioSystem, RefillRecoversAfterDrain) {
    test::CaptureAudioSink sink;
    auto audio = Access::makeManual(AudioKind::Chiptune, sink);
    const AudioId tone = sameboy::diagnosticTone();
    audio->play(tone);
    Access::step(*audio);
    const std::size_t primed = audio->framesBuffered();
    EXPECT_GE(primed, kPrimedLow);

    sink.drain(primed);  // the device takes everything
    EXPECT_EQ(audio->framesBuffered(), 0u);
    Access::step(*audio);  // deficit is the whole target again → refills
    EXPECT_GE(audio->framesBuffered(), kPrimedLow);
}

TEST(AudioSystem, StopHaltsProduction) {
    test::CaptureAudioSink sink;
    auto audio = Access::makeManual(AudioKind::Chiptune, sink);
    const AudioId tone = sameboy::diagnosticTone();
    audio->play(tone);
    Access::step(*audio);
    EXPECT_GT(audio->framesBuffered(), 0u);

    sink.drain(audio->framesBuffered());  // empty the ring
    audio->stop();
    Access::step(*audio);  // stopped → no further production
    EXPECT_EQ(audio->framesBuffered(), 0u);
}

// ── Owned-sink path (ctor 2: unique_ptr) — real threaded production ───────────────────────────────
// These exercise the ownership machinery that the default ctor (3, the SdlAudioSink path) rides on. The
// default ctor itself opens a real device and so is not CI-testable — it is dev-machine-verified via
// audio_keyboard_demo, the established treatment for every SDL device path. What CI covers here is
// everything except the concrete choice of SdlAudioSink: the system owning its sink, opening it, driving
// it on its production thread, and tearing it down in the right order (sink stop → thread join → members).
// Failability: bind the Impl's `sink` reference to the wrong member and these go red.

TEST(AudioSystem, OwnsAnInjectedSinkAndOpensItAtTheConfiguredRate) {
    auto owned = std::make_unique<test::CaptureAudioSink>();
    test::CaptureAudioSink* observer = owned.get();  // keep an observer before the move
    AudioSystem audio{AudioKind::Chiptune, std::move(owned)};             // ctor (2) — the system now owns the sink (threaded)

    EXPECT_TRUE(observer->started());
    EXPECT_EQ(observer->rate(), kAudioSampleRate);
    EXPECT_EQ(observer->channels(), kAudioChannels);

    // The owned sink drives the same chain as the borrowed one: a diagnostic tone produces real PCM. The
    // production thread fills the ring autonomously — poll until primed, then pull and inspect.
    const AudioId tone = sameboy::diagnosticTone();
    audio.play(tone);
    ASSERT_TRUE(waitForBuffered(audio, kPrimedLow, std::chrono::milliseconds(2000)));
    const std::vector<AudioFrame> produced = observer->drain(audio.framesBuffered());
    EXPECT_GT(nonSilentCount(produced), std::size_t{100});  // a real waveform, not a flat line
}

TEST(AudioSystem, OwnedSinkIsStartedThenStoppedOnDestruction) {
    auto owned = std::make_unique<test::CaptureAudioSink>();
    test::CaptureAudioSink* observer = owned.get();
    {
        AudioSystem audio{AudioKind::Chiptune, std::move(owned)};
        EXPECT_TRUE(observer->started());  // start() ran during construction
        // observer (the sink) outlives `audio` only because we kept the raw pointer; the AudioSystem
        // owns the sink object, so leaving this scope destroys it through the AudioSystem.
        const AudioId tone = sameboy::diagnosticTone();
        audio.play(tone);
        waitForBuffered(audio, kPrimedLow, std::chrono::milliseconds(2000));  // let it actually produce
    }
    // After the AudioSystem is gone, the dtor stopped the sink, joined the production thread, and tore
    // down the ring/vm — in that order, no dangling pull, no double-stop, no join race. The observer
    // points at freed memory now, so we don't deref it post-scope; reaching here without a teardown fault
    // is the signal.
    SUCCEED();
}

// Parity: an owned-sink system and a borrowed-sink system reach the same non-silent, primed outcome for
// the same registration + play — proving ctor (2) and ctor (1) share one production code path (both
// threaded).
TEST(AudioSystem, OwnedAndBorrowedSinksProduceEquivalently) {
    test::CaptureAudioSink borrowedSink;
    AudioSystem borrowed{AudioKind::Chiptune, borrowedSink};             // ctor (1)
    auto ownedSinkPtr = std::make_unique<test::CaptureAudioSink>();
    test::CaptureAudioSink* ownedObserver = ownedSinkPtr.get();
    AudioSystem owned{AudioKind::Chiptune, std::move(ownedSinkPtr)};     // ctor (2)

    const AudioId tone = sameboy::diagnosticTone();
    borrowed.play(tone);
    owned.play(tone);
    ASSERT_TRUE(waitForBuffered(borrowed, kPrimedLow, std::chrono::milliseconds(2000)));
    ASSERT_TRUE(waitForBuffered(owned, kPrimedLow, std::chrono::milliseconds(2000)));
    EXPECT_GT(nonSilentCount(borrowedSink.drain(borrowed.framesBuffered())), std::size_t{100});
    EXPECT_GT(nonSilentCount(ownedObserver->drain(owned.framesBuffered())), std::size_t{100});
}

}  // namespace
}  // namespace retropp
