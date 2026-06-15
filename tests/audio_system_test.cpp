// ENG-4.A — the AudioSystem end-to-end, device-free. Drives the public surface (gbcpp/audio_system.h)
// against a headless CaptureAudioSink: register the built-in diagnostic tone, play it, tick the
// system, and inspect the PCM it produced. This is the red→green proof that the hardware-speed throttle
// is realized — registering a HardwareSpeed driver threw before ENG-4.A; here it produces real samples.
// No Vm, no Routine, no throttle appears in the test — proof the VM is fully hidden behind audio terms.
#include "gbcpp/audio_system.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "gbcpp/audio.h"
#include "gbcpp/gb_audio.h"  // sameboy::diagnosticTone (GB preset)
#include "gbcpp/timing.h"
#include "gbcpp/vm.h"
#include "mock_platform.h"  // test::CaptureAudioSink

namespace gbcpp {
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

}  // namespace
}  // namespace gbcpp
