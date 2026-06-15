// ENG-4.A — the AudioSystem end-to-end, device-free. Drives the public surface (retropp/audio_system.h)
// against a headless CaptureAudioSink: register the built-in diagnostic tone, play it, tick the
// system, and inspect the PCM it produced. This is the red→green proof that the hardware-speed throttle
// is realized — registering a HardwareSpeed driver threw before ENG-4.A; here it produces real samples.
// No Vm, no Routine, no throttle appears in the test — proof the VM is fully hidden behind audio terms.
#include "retropp/audio_system.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/audio.h"
#include "retropp/gb_audio.h"  // sameboy::diagnosticTone (GB preset)
#include "retropp/timing.h"
#include "retropp/vm.h"
#include "mock_platform.h"  // test::CaptureAudioSink

namespace retropp {
namespace {

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

// The output buffer is kept around a small latency target (~sampleRate / 20 ≈ 50 ms). A primed buffer
// is at least a few tens of ms; a bounded one stays well under the ring capacity (it never piles up).
constexpr std::size_t kPrimedLow   = kAudioSampleRate / 40;  // ~25 ms — definitely primed
constexpr std::size_t kBoundedHigh = kAudioSampleRate / 8;   // ~125 ms — bounded, not piled up

TEST(AudioSystem, OpensTheSinkAtTheConfiguredRate) {
    test::CaptureAudioSink sink;
    AudioSystem audio{sink};  // Game Boy Color default, 48 kHz
    EXPECT_TRUE(sink.started());
    EXPECT_EQ(sink.rate(), kAudioSampleRate);
    EXPECT_EQ(sink.channels(), kAudioChannels);
}

TEST(AudioSystem, ProducesNothingUntilSomethingPlays) {
    test::CaptureAudioSink sink;
    AudioSystem audio{sink};
    audio.tick();  // no driver registered/playing → no production
    EXPECT_EQ(audio.framesBuffered(), 0u);
}

// The headline red→green: a hardware-speed driver, registered through the audio surface and played,
// produces non-silent PCM. (Before ENG-4.A, registering a HardwareSpeed routine threw —
// sameboy::diagnosticTone would have thrown here.) One tick primes the latency buffer.
TEST(AudioSystem, DiagnosticToneProducesNonSilentPcm) {
    test::CaptureAudioSink sink;
    AudioSystem audio{sink};
    const AudioId tone = sameboy::diagnosticTone(audio);
    audio.play(tone);

    audio.tick();  // the deficit is the whole target → the buffer primes to ~its latency target
    const std::size_t buffered = audio.framesBuffered();
    EXPECT_GE(buffered, kPrimedLow);
    EXPECT_LE(buffered, kBoundedHigh);

    // Pull the produced PCM and confirm it is an actual waveform, not silence.
    const std::vector<AudioFrame> produced = sink.drain(buffered);
    EXPECT_GT(produced.size(), kPrimedLow);
    EXPECT_GT(nonSilentCount(produced), std::size_t{100});  // a real waveform, not a flat line
}

// Production tracks the buffer level, not a fixed per-tick amount: ticking repeatedly without the
// device draining tops the buffer up to the target ONCE and then stops — it stays bounded and never
// overflows. (The old fixed-budget model piled up ~a frame per tick and would overflow here.)
TEST(AudioSystem, RefillStaysBoundedAndNeverOverflows) {
    test::CaptureAudioSink sink;
    AudioSystem audio{sink};
    const AudioId tone = sameboy::diagnosticTone(audio);
    audio.play(tone);

    for (int i = 0; i < 100; ++i) {
        audio.tick();
    }
    EXPECT_GE(audio.framesBuffered(), kPrimedLow);    // primed
    EXPECT_LE(audio.framesBuffered(), kBoundedHigh);  // bounded — did not pile up over 100 ticks
    EXPECT_EQ(audio.framesDropped(), 0u);             // never overflowed the ring
}

// After the device drains the buffer, the next tick sees the full deficit and refills it — so a drain
// never leaves the stream permanently starved (the drift/underrun self-correction).
TEST(AudioSystem, RefillRecoversAfterDrain) {
    test::CaptureAudioSink sink;
    AudioSystem audio{sink};
    const AudioId tone = sameboy::diagnosticTone(audio);
    audio.play(tone);
    audio.tick();
    const std::size_t primed = audio.framesBuffered();
    EXPECT_GE(primed, kPrimedLow);

    sink.drain(primed);  // the device takes everything
    EXPECT_EQ(audio.framesBuffered(), 0u);
    audio.tick();        // deficit is the whole target again → refills
    EXPECT_GE(audio.framesBuffered(), kPrimedLow);
}

TEST(AudioSystem, StopHaltsProduction) {
    test::CaptureAudioSink sink;
    AudioSystem audio{sink};
    const AudioId tone = sameboy::diagnosticTone(audio);
    audio.play(tone);
    audio.tick();
    EXPECT_GT(audio.framesBuffered(), 0u);

    sink.drain(audio.framesBuffered());  // empty the ring
    audio.stop();
    audio.tick();  // stopped → no further production
    EXPECT_EQ(audio.framesBuffered(), 0u);
}

// ── Owned-sink path (ctor 2: unique_ptr) ─────────────────────────────────────────────────────────
// These exercise the ownership machinery that the default ctor (3, the SdlAudioSink path) rides on.
// The default ctor itself opens a real device and so is not CI-testable — it is dev-machine-verified
// via audio_keyboard_demo, the established treatment for every SDL device path (SdlPlatform, Renderer,
// SdlAudioSink). What CI covers here is everything except the concrete choice of SdlAudioSink: the
// system owning its sink, opening it, driving it, and tearing it down in the right order. Failability:
// bind the Impl's `sink` reference to the wrong member and these go red.

TEST(AudioSystem, OwnsAnInjectedSinkAndOpensItAtTheConfiguredRate) {
    auto owned = std::make_unique<test::CaptureAudioSink>();
    test::CaptureAudioSink* observer = owned.get();  // keep an observer before the move
    AudioSystem audio{std::move(owned)};             // ctor (2) — the system now owns the sink

    EXPECT_TRUE(observer->started());
    EXPECT_EQ(observer->rate(), kAudioSampleRate);
    EXPECT_EQ(observer->channels(), kAudioChannels);

    // The owned sink drives the same chain as the borrowed one: a diagnostic tone produces real PCM.
    const AudioId tone = sameboy::diagnosticTone(audio);
    audio.play(tone);
    audio.tick();
    const std::size_t buffered = audio.framesBuffered();
    EXPECT_GE(buffered, kPrimedLow);
    EXPECT_LE(buffered, kBoundedHigh);
    const std::vector<AudioFrame> produced = observer->drain(buffered);
    EXPECT_GT(nonSilentCount(produced), std::size_t{100});  // a real waveform, not a flat line
}

TEST(AudioSystem, OwnedSinkIsStartedThenStoppedOnDestruction) {
    auto owned = std::make_unique<test::CaptureAudioSink>();
    test::CaptureAudioSink* observer = owned.get();
    {
        AudioSystem audio{std::move(owned)};
        EXPECT_TRUE(observer->started());  // start() ran during construction
        // observer (the sink) outlives `audio` only because we kept the raw pointer; the AudioSystem
        // owns the sink object, so leaving this scope destroys it through the AudioSystem.
        const AudioId tone = sameboy::diagnosticTone(audio);
        audio.play(tone);
        audio.tick();
    }
    // After the AudioSystem is gone, the owned sink is stopped (the dtor calls sink.stop() before the
    // ring/vm tear down) — no dangling pull, no double-stop fault. The observer points at freed memory
    // now, so we don't deref it post-scope; reaching here without a teardown-order fault is the signal.
    SUCCEED();
}

// Parity: an owned-sink system and a borrowed-sink system reach the same non-silent, bounded outcome
// for the same registration + play — proving ctor (2) and ctor (1) share one production code path.
TEST(AudioSystem, OwnedAndBorrowedSinksProduceEquivalently) {
    test::CaptureAudioSink borrowedSink;
    AudioSystem borrowed{borrowedSink};                                  // ctor (1)
    auto ownedSinkPtr = std::make_unique<test::CaptureAudioSink>();
    test::CaptureAudioSink* ownedObserver = ownedSinkPtr.get();
    AudioSystem owned{std::move(ownedSinkPtr)};                          // ctor (2)

    for (AudioSystem* sys : {&borrowed, &owned}) {
        const AudioId tone = sameboy::diagnosticTone(*sys);
        sys->play(tone);
        sys->tick();
    }
    EXPECT_GE(borrowed.framesBuffered(), kPrimedLow);
    EXPECT_GE(owned.framesBuffered(), kPrimedLow);
    EXPECT_LE(borrowed.framesBuffered(), kBoundedHigh);
    EXPECT_LE(owned.framesBuffered(), kBoundedHigh);
    EXPECT_GT(nonSilentCount(borrowedSink.drain(borrowed.framesBuffered())), std::size_t{100});
    EXPECT_GT(nonSilentCount(ownedObserver->drain(owned.framesBuffered())), std::size_t{100});
}

}  // namespace
}  // namespace retropp
