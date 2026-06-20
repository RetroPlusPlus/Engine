// ENG-4.A follow-on — one-shot SFX auto-close. A finished one-shot SFX (output gone exact-zero past the
// threshold) must stop being stepped (clear `playing`); a still-sounding voice and any Music must NOT.
// Device-free: a CaptureAudioSink opens no device, and the test drives production by ticking + draining.
#include "retropp/audio_system.h"

#include <cstddef>
#include <filesystem>

#include <gtest/gtest.h>

#include "retropp/asset_policy.h"    // AssetPolicy::LoadFromPath
#include "retropp/asset_registry.h"  // setAssetRoot — the single LoadFromPath base for the literal names
#include "retropp/audio_library.h"   // AudioLibrary, AudioType, AudioId
#include "src/audio/auto_close.h"    // detail::shouldAutoStop — the pure decision under test
#include "mock_platform.h"           // test::CaptureAudioSink

namespace retropp {
namespace {

void setTonesRoot() {
    setAssetRoot(std::filesystem::path(RETROPP_ASSETS_DIR) / "tones");
}

// Tick + drain (so the ring keeps emptying and the VM keeps producing) until the system auto-closes or the
// cap is hit. Returns the tick count taken.
int driveUntilIdleOrCap(AudioSystem& audio, test::CaptureAudioSink& sink, int cap) {
    int ticks = 0;
    for (; ticks < cap && audio.isPlaying(); ++ticks) {
        audio.tick();
        sink.drain(1u << 20);
    }
    return ticks;
}

// Tick + drain a fixed number of times regardless of state (to prove something does NOT auto-close).
void driveFor(AudioSystem& audio, test::CaptureAudioSink& sink, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        audio.tick();
        sink.drain(1u << 20);
    }
}

// ── The pure decision ──────────────────────────────────────────────────────────────────────────
TEST(AudioAutoClose, ShouldAutoStopPredicate) {
    using detail::shouldAutoStop;
    EXPECT_TRUE(shouldAutoStop(12'000, 12'000, AudioType::Sfx));        // exactly at threshold
    EXPECT_TRUE(shouldAutoStop(20'000, 12'000, AudioType::Sfx));        // past threshold
    EXPECT_FALSE(shouldAutoStop(11'999, 12'000, AudioType::Sfx));       // one frame short
    EXPECT_FALSE(shouldAutoStop(1'000'000, 12'000, AudioType::Music));  // Music NEVER, however silent
}

// ── A finished one-shot SFX auto-closes ──────────────────────────────────────────────────────────
TEST(AudioAutoClose, FinishedSfxAutoCloses) {
    setTonesRoot();
    test::CaptureAudioSink sink;
    AudioSystem audio{sink};
    const AudioId blip = AudioLibrary::instance().registerAudio("sfx_blip.asm", AudioType::Sfx, Isa::Sm83,
                                                                AssetPolicy::LoadFromPath);
    audio.play(blip);
    EXPECT_TRUE(audio.isPlaying());  // cued and sounding

    const int ticks = driveUntilIdleOrCap(audio, sink, 2'000);
    EXPECT_FALSE(audio.isPlaying()) << "a finished one-shot SFX did not auto-close after going silent";
    EXPECT_GT(ticks, 1) << "auto-closed before the sound could play (premature)";
}

// ── A continuously-sounding SFX does NOT auto-close ───────────────────────────────────────────────
TEST(AudioAutoClose, ContinuousSfxDoesNotAutoClose) {
    setTonesRoot();
    test::CaptureAudioSink sink;
    AudioSystem audio{sink};
    const AudioId tone = AudioLibrary::instance().registerAudio("tone_sustain.asm", AudioType::Sfx,
                                                                Isa::Sm83, AssetPolicy::LoadFromPath);
    audio.play(tone);
    driveFor(audio, sink, 200);  // well past the silence threshold (rate/4)
    EXPECT_TRUE(audio.isPlaying()) << "a continuously-sounding SFX must not auto-close";
}

// ── Music is NEVER auto-closed, even when its output goes silent ──────────────────────────────────
TEST(AudioAutoClose, SilentMusicDoesNotAutoClose) {
    setTonesRoot();
    test::CaptureAudioSink sink;
    AudioSystem audio{sink};
    // The same decaying blip, but registered as Music: it DOES go silent, yet must NOT auto-close — the
    // game owns a Music track's lifetime (play/stop on demand). This is the AudioType gate.
    const AudioId asMusic = AudioLibrary::instance().registerAudio("sfx_blip.asm", AudioType::Music,
                                                                   Isa::Sm83, AssetPolicy::LoadFromPath);
    audio.play(asMusic);
    driveFor(audio, sink, 200);  // past the decay + threshold
    EXPECT_TRUE(audio.isPlaying()) << "Music must never auto-close, even when silent";
}

// ── Re-cueing after an auto-close restarts cleanly ────────────────────────────────────────────────
TEST(AudioAutoClose, ReplayAfterAutoCloseWorks) {
    setTonesRoot();
    test::CaptureAudioSink sink;
    AudioSystem audio{sink};
    const AudioId blip = AudioLibrary::instance().registerAudio("sfx_blip.asm", AudioType::Sfx, Isa::Sm83,
                                                                AssetPolicy::LoadFromPath);
    audio.play(blip);
    driveUntilIdleOrCap(audio, sink, 2'000);
    ASSERT_FALSE(audio.isPlaying());

    audio.play(blip);                 // re-cue the same id
    EXPECT_TRUE(audio.isPlaying());   // plays again from scratch
    const int ticks = driveUntilIdleOrCap(audio, sink, 2'000);
    EXPECT_FALSE(audio.isPlaying());  // and auto-closes again
    EXPECT_GT(ticks, 1);
}

}  // namespace
}  // namespace retropp
