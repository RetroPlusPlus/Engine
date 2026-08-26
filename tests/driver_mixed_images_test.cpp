// One hosted-driver registration carrying BOTH image sources: some images given as bytes, others as a
// path plus its own policy, chosen image by image. A driver commonly needs exactly this — port-authored
// startup code that belongs in the binary, beside a section read at runtime out of content the game has no
// right to ship inside it.
//
// The stored form is per-image, so what these cases pin is the registration surface: that a mixed binding
// lowers to the same two stored shapes a single-source binding produces, that an image with no bytes is
// refused where a game can still catch it, and that the registration owns its copy — the span's producer
// may be destroyed long before host() runs.
#include "retropp/audio_library.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/audio.h"
#include "retropp/audio_system.h"
#include "retropp/driver_binding.h"
#include "retropp/gb.h"
#include "retropp/vm.h"
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — synchronous production seam
#include "mock_platform.h"                    // test::CaptureAudioSink

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;

// The driver's setup half, declared as a path so the build assembles and bakes it. Its sibling — the tick
// — arrives as bytes, which is what makes the binding mixed.
//
// The path is spelled as a literal inside the DriverImagePath initializer below, because that is the form
// LiteralPath accepts and the form the build scan reads. This view is the same text for the assertions to
// compare against.
constexpr std::string_view kInitImagePath = "tests/fixtures/driver_mixed/driver_init.asm";

// The byte half: each tick, consume a non-zero music mailbox ($C010) by recording it, setting CH3's
// frequency from it and triggering, then clearing the mailbox so the sound does not re-trigger while idle.
const char* kTickSource = R"(
  ld a, [$C010]
  or a
  jr z, .done
  ld [$C020], a         ; music last-seen
  ldh [$FF1D], a        ; NR33 — CH3 frequency low = the played id
  ld a, $86
  ldh [$FF1E], a        ; NR34 — trigger + frequency high bits
  xor a
  ld [$C010], a         ; consume the mailbox
.done:
  ret
)";

struct MixedSlots {
    std::optional<std::uint8_t> mailbox;        // 0: $C010 (Read)
    std::optional<std::uint8_t> musicLastSeen;  // 1: $C020 (Read)
};

// Assemble SM83 source into image bytes through a throwaway VM (the engine's own assembler).
std::vector<std::uint8_t> asm83(const std::string& source) {
    Vm probe{VMPlatform::GameBoyColor};
    return probe.assemble(source);
}

std::size_t nonSilentCount(const std::vector<AudioFrame>& frames) {
    std::size_t n = 0;
    for (const AudioFrame& f : frames) {
        if (f.left != 0 || f.right != 0) {
            ++n;
        }
    }
    return n;
}

const DriverVerbs kVerbs{
    .play = {.music = Instruction::write(Location::memory(0xC010), 1)},
    .stop = Instruction::write(Location::memory(0xC012), 1, /*fixedValue=*/1),
};

DriverSlots<MixedSlots> mixedSlots() {
    return slots(slot(&MixedSlots::mailbox, 0xC010, SlotDirection::Read),
                 slot(&MixedSlots::musicLastSeen, 0xC020, SlotDirection::Read));
}

// The mixed binding under test: the setup image by path (Embed), the tick image by bytes.
HostedDriverBinding mixedBinding(std::span<const std::uint8_t> tickBytes) {
    return HostedDriverBinding{
        .images    = {DriverImagePath{.base   = 0x6000,
                                      .path   = "tests/fixtures/driver_mixed/driver_init.asm",
                                      .policy = AssetPolicy::Embed},
                      DriverImage{.bytes = tickBytes, .base = 0x6100}},
        .tickEntry = 0x6100,
        .init      = Instruction::call(0x6000, gb::A, /*fixedValue=*/0),
        .isa       = Isa::Sm83,
    };
}

std::vector<AudioFrame> stepAndDrain(AudioSystem& sys, test::CaptureAudioSink& sink) {
    Access::step(sys);
    return sink.drain(sys.audioStats().framesBuffered);
}

// ── Lowering ────────────────────────────────────────────────────────────────────────────────────────

// The headline: one binding, two sources, and each image lands in the stored shape its own source dictates
// — the byte image owning a copy with no path, the path image carrying its path and policy with no bytes.
TEST(DriverMixedImages, EachImageLowersToTheShapeItsSourceDictates) {
    AudioLibrary& lib = AudioLibrary::instance();
    const std::vector<std::uint8_t> tick = asm83(kTickSource);

    const DriverId<MixedSlots> id = lib.registerDriver(mixedBinding(tick), kVerbs, mixedSlots());

    const DriverDefinition& d = *lib.entry(id.id()).driver;
    ASSERT_EQ(d.images.size(), 2u);

    EXPECT_EQ(d.images[0].base, 0x6000u);
    EXPECT_EQ(d.images[0].path, kInitImagePath);
    EXPECT_TRUE(d.images[0].bytes.empty());  // a path image copies no bytes at registration
    ASSERT_TRUE(d.images[0].policy.has_value());
    EXPECT_EQ(*d.images[0].policy, AssetPolicy::Embed);

    EXPECT_EQ(d.images[1].base, 0x6100u);
    EXPECT_EQ(d.images[1].bytes, tick);      // the registration owns a copy of the span
    EXPECT_TRUE(d.images[1].path.empty());
    EXPECT_FALSE(d.images[1].policy.has_value());  // a byte image resolves through no policy
}

// ── An image whose bytes did not load ───────────────────────────────────────────────────────────────

// An empty span is a source that failed, and it is refused on the thread that registers — where a game
// still has a seam to catch it. Left alone it would reach host() on the audio production thread, which no
// consumer can wrap, as a path image with no path.
TEST(DriverMixedImages, AnEmptyByteImageIsRefusedAtRegisterDriver) {
    AudioLibrary& lib = AudioLibrary::instance();
    HostedDriverBinding b = mixedBinding({});

    try {
        (void)lib.registerDriver(b, kVerbs, mixedSlots());
        FAIL() << "an empty byte image must be refused at registration";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("0x6100"), std::string::npos)
            << "the error names the image that failed: " << e.what();
    }
}

// Both doors share the refusal, so the all-bytes registration answers the same way.
TEST(DriverMixedImages, AnEmptyByteImageIsRefusedAtUploadDriver) {
    AudioLibrary& lib = AudioLibrary::instance();
    DriverBinding b;
    b.images    = {DriverImage{.bytes = {}, .base = 0x6200}};
    b.tickEntry = 0x6200;

    try {
        (void)lib.uploadDriver<NoSlots>(b, kVerbs);
        FAIL() << "an empty byte image must be refused at registration";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("0x6200"), std::string::npos)
            << "the error names the image that failed: " << e.what();
    }
}

// ── Hosting ─────────────────────────────────────────────────────────────────────────────────────────

// A mixed binding hosts and the driver runs: the baked path image supplies the setup that the byte image's
// tick then plays through. Both sources have to arrive for a sound to come out — the tick alone triggers a
// channel whose DAC the setup never switched on.
TEST(DriverMixedImages, AMixedBindingHostsAndTheDriverRuns) {
    const std::vector<std::uint8_t> tick = asm83(kTickSource);
    const DriverId<MixedSlots> id =
        AudioLibrary::instance().registerDriver(mixedBinding(tick), kVerbs, mixedSlots());

    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<MixedSlots> driver = sys->host(id);
    driver.play(0x40);
    const std::vector<AudioFrame> pcm = stepAndDrain(*sys, sink);

    EXPECT_GT(nonSilentCount(pcm), std::size_t{100});  // a real waveform, not a flat line

    const MixedSlots read = driver.slots();
    ASSERT_TRUE(read.musicLastSeen.has_value());
    EXPECT_EQ(*read.musicLastSeen, 0x40u);  // the played id reached the driver's state
}

// Register from a buffer, destroy the buffer, then host. This is what lets a game host a cartridge purely
// to read it and reclaim the machine at the end of startup: nothing that produced a byte image has to
// outlive the registration call. The buffer is CLOBBERED before it dies, so a retained view would be
// observed rather than merely being undefined.
DriverId<MixedSlots> registerFromADoomedBuffer() {
    std::vector<std::uint8_t> scratch = asm83(kTickSource);
    const DriverId<MixedSlots> id =
        AudioLibrary::instance().registerDriver(mixedBinding(scratch), kVerbs, mixedSlots());
    std::fill(scratch.begin(), scratch.end(), 0xFF);
    return id;  // scratch dies here
}

TEST(DriverMixedImages, TheSpansOwnerNeedNotOutliveTheRegistration) {
    const DriverId<MixedSlots> id = registerFromADoomedBuffer();

    test::CaptureAudioSink sink;
    auto sys = Access::makeManual(AudioKind::Chiptune, sink);
    HostedDriver<MixedSlots> driver = sys->host(id);
    driver.play(0x40);
    const std::vector<AudioFrame> pcm = stepAndDrain(*sys, sink);

    EXPECT_GT(nonSilentCount(pcm), std::size_t{100});

    const MixedSlots read = driver.slots();
    ASSERT_TRUE(read.musicLastSeen.has_value());
    EXPECT_EQ(*read.musicLastSeen, 0x40u);  // the tick that ran is the one that was registered
}

}  // namespace
}  // namespace retropp
