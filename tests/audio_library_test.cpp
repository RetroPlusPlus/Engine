// ENG-4.B — the AudioLibrary catalog, device-free (no VM, no sink). Proves the single-instance store
// holds portable audio definitions and mints handles through both doors. Because the library is a
// program-wide singleton, EVERY assertion is relative to the current size() — entries accumulate across
// the whole test binary (audio_system_test registers tones into the same library), so absolute id values
// are never asserted; this both keeps the tests order-independent and documents the accumulate-once shape.
#include "retropp/audio_library.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace retropp {
namespace {

TEST(AudioLibrary, InstanceIsASingleton) {
    EXPECT_EQ(&AudioLibrary::instance(), &AudioLibrary::instance());
}

// The RAW door copies the supplied bytes into the library's own storage, tags the entry Chiptune with
// the given ISA, and mints the next ascending id. The stored bytes equal the input; no path is stored.
TEST(AudioLibrary, UploadAudioStoresOwnedBytesAndMintsAscendingId) {
    AudioLibrary& lib = AudioLibrary::instance();
    const std::size_t before = lib.size();
    const std::vector<std::uint8_t> bytes{0x3E, 0x10, 0xE0, 0x24};
    const AudioId id = lib.uploadAudio(bytes, AudioType::Sfx, Isa::Sm83);

    EXPECT_EQ(static_cast<std::size_t>(id), before);  // dense, ascending from the prior size
    EXPECT_EQ(lib.size(), before + 1);

    const AudioLibrary::Entry& e = lib.entry(id);
    EXPECT_EQ(e.kind, AudioKind::Chiptune);
    EXPECT_EQ(e.type, AudioType::Sfx);
    EXPECT_EQ(e.isa, Isa::Sm83);
    EXPECT_EQ(e.bytecode, bytes);  // copied into the library's own storage
    EXPECT_TRUE(e.asmPath.empty());
}

// The copy means the caller's buffer need not outlive the call — the library owns the bytes afterward.
TEST(AudioLibrary, UploadCopiesSoTheCallerSpanNeedNotOutlive) {
    AudioLibrary& lib = AudioLibrary::instance();
    AudioId id{};
    {
        const std::vector<std::uint8_t> scratch{1, 2, 3};
        id = lib.uploadAudio(scratch, AudioType::Music, Isa::Sm83);
    }  // scratch destroyed here
    const std::vector<std::uint8_t> expected{1, 2, 3};
    EXPECT_EQ(lib.entry(id).bytecode, expected);
}

// The SUGAR (path) door records the source path (materialized later, on play) — a path entry, no bytes
// — and infers the KIND from the extension: a `.asm` source is a Chiptune. The ISA selected here and the
// per-call policy are stored on the entry.
TEST(AudioLibrary, RegisterAudioStoresPathNotBytes) {
    AudioLibrary& lib = AudioLibrary::instance();
    const AudioId id = lib.registerAudio("audio/song.asm", AudioType::Music, Isa::Sm83);

    const AudioLibrary::Entry& e = lib.entry(id);
    EXPECT_EQ(e.kind, AudioKind::Chiptune);  // inferred from the `.asm` extension
    EXPECT_EQ(e.type, AudioType::Music);
    EXPECT_EQ(e.isa, Isa::Sm83);
    EXPECT_FALSE(e.policy.has_value());  // omitted → fall through to the per-type / engine default
    EXPECT_EQ(e.asmPath, "audio/song.asm");
    EXPECT_TRUE(e.bytecode.empty());
}

// The no-ISA (PCM) path door infers PCM from an audio-container extension and carries the per-call policy
// through. A PCM file has no ISA — it decodes and plays without the VM — so it registers through the no-ISA
// overload; a PCM-extension literal does not compile through the ISA (chiptune) door.
TEST(AudioLibrary, RegisterAudioInfersPcmFromExtensionAndKeepsPolicy) {
    AudioLibrary& lib = AudioLibrary::instance();
    const AudioId id =
        lib.registerAudio("music/theme.ogg", AudioType::Music, AssetPolicy::LoadFromPath);

    const AudioLibrary::Entry& e = lib.entry(id);
    EXPECT_EQ(e.kind, AudioKind::Pcm);  // inferred from the `.ogg` extension
    ASSERT_TRUE(e.policy.has_value());
    EXPECT_EQ(*e.policy, AssetPolicy::LoadFromPath);
}

// Each registration — either door — mints a distinct, consecutive handle.
TEST(AudioLibrary, DistinctRegistrationsGetConsecutiveIds) {
    AudioLibrary& lib = AudioLibrary::instance();
    const AudioId a = lib.uploadAudio(std::vector<std::uint8_t>{0xAA}, AudioType::Sfx, Isa::Sm83);
    const AudioId b = lib.registerAudio("x.asm", AudioType::Sfx, Isa::Sm83);
    EXPECT_EQ(static_cast<std::size_t>(b), static_cast<std::size_t>(a) + 1);
}

}  // namespace
}  // namespace retropp
