// Catalog registration for a hosted resident sound driver, exercised through the PUBLIC audio vocabulary
// only (retropp/audio_library.h, retropp/driver_binding.h, retropp/gb.h). Each case registers a
// driver on the single AudioLibrary through one of the two registration functions — uploadDriver (inline bytes) or
// registerDriver (per-image paths) — and reads the stored definition back through entry(). No VM, no
// device: registration is pure catalog data, so the round-trip, the validation, the policy resolution, and
// the type-erased slot accessors are all observable without hosting anything.
//
// The AudioLibrary is a program-wide singleton, so ids accumulate across every test in the process; these
// cases never assume an absolute id — they read back through the id the registration returned, and prove density
// only relative to size() captured immediately before a pair of registrations.
#include "retropp/asset_policy.h"
#include "retropp/audio_library.h"
#include "retropp/driver_binding.h"
#include "retropp/gb.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace retropp {
namespace {

// A demo game's slots struct: the plain value type whose engaged optional fields name driver state. The
// field TYPE carries the slot width (uint8_t → 1 byte, uint16_t → 2).
struct DemoSlots {
    std::optional<std::uint8_t>  volume;  // a console flag/word
    std::optional<std::uint16_t> pitch;
    std::optional<std::uint8_t>  fade;
};

// A DriverImage over a static byte array at a base (bank-qualified or flat).
template <std::size_t N>
DriverImage image(const std::array<std::uint8_t, N>& bytes, std::uint32_t base) {
    return DriverImage{.bytes = std::span<const std::uint8_t>(bytes), .base = base};
}

// A minimal flat driver image reused across cases (contents are irrelevant to registration).
constexpr std::array<std::uint8_t, 4> kEngine{0x3E, 0x37, 0xC9, 0x00};
constexpr std::array<std::uint8_t, 2> kData{0xAB, 0xCD};

// A minimal valid verb set reused across cases: a Music play realization (required) and a stop. The
// specific addresses are irrelevant to registration — only that a Music verb is present.
const DriverVerbs kVerbs{
    .play = {.music = Instruction::write(Location::memory(0xC2A9), 1)},
    .stop = Instruction::write(Location::memory(0xC2A9), 1, /*fixedValue=*/0),
};

// ── uploadDriver: inline bytes ────────────────────────────────────────────────────────────────────

// uploadDriver (inline bytes) stores every machine fact and owns a copy of each image's bytes; the entry is tagged
// AudioKind::Driver with the binding's ISA.
TEST(DriverRegistration, UploadDriverStoresMachineFactsAndOwnsBytes) {
    AudioLibrary& lib = AudioLibrary::instance();
    DriverBinding b;
    b.images    = {image(kEngine, 0x6000), image(kData, gb::banked(2, 0x4000))};
    b.mapper    = gb::Mbc3;
    b.tickEntry = 0x6000;
    b.stackTop  = 0xDF00;
    b.init      = Instruction::write(Location::memory(0xC000), 1, /*fixedValue=*/0xC3);
    b.isa       = Isa::Sm83;

    const DriverId<DemoSlots> id = lib.uploadDriver(
        b, kVerbs,
        slots(slot(&DemoSlots::volume, 0xC29A), slot(&DemoSlots::pitch, 0xC2B0, SlotDirection::Write)));

    const AudioLibrary::Entry& e = lib.entry(id.id());
    EXPECT_EQ(e.kind, AudioKind::Driver);
    EXPECT_EQ(e.isa, Isa::Sm83);
    ASSERT_TRUE(e.driver.has_value());
    const DriverDefinition& d = *e.driver;
    EXPECT_EQ(d.mapper.id(), gb::Mbc3.id());
    EXPECT_EQ(d.tickEntry, 0x6000u);
    ASSERT_TRUE(d.stackTop.has_value());
    EXPECT_EQ(*d.stackTop, 0xDF00u);
    ASSERT_TRUE(d.init.has_value());
    EXPECT_EQ(d.init->kind(), Instruction::Kind::Write);

    ASSERT_EQ(d.images.size(), 2u);
    EXPECT_EQ(d.images[0].base, 0x6000u);
    EXPECT_EQ(d.images[0].bytes, std::vector<std::uint8_t>(kEngine.begin(), kEngine.end()));
    EXPECT_TRUE(d.images[0].path.empty());
    EXPECT_EQ(d.images[1].base, gb::banked(2, 0x4000));  // the bank-qualified base rides through untouched
    EXPECT_EQ(d.images[1].bytes, std::vector<std::uint8_t>(kData.begin(), kData.end()));
}

// A driver with no declared slots (the argument-family shape) registers through the default S = NoSlots — no
// slots argument, empty slot + accessor lists.
TEST(DriverRegistration, NoSlotsDriverRegisters) {
    AudioLibrary& lib = AudioLibrary::instance();
    DriverBinding b;
    b.images    = {image(kEngine, 0x6000)};
    b.tickEntry = 0x6000;

    const DriverId<NoSlots> id = lib.uploadDriver(b, kVerbs);

    const AudioLibrary::Entry& e = lib.entry(id.id());
    EXPECT_EQ(e.kind, AudioKind::Driver);
    ASSERT_TRUE(e.driver.has_value());
    EXPECT_TRUE(e.driver->slots.empty());
    EXPECT_TRUE(e.driver->accessors.empty());
}

// Declaring slots on the binding itself at uploadDriver is a usage error — slots come through slots(...).
TEST(DriverRegistration, UploadDriverRejectsSlotsOnTheBinding) {
    AudioLibrary& lib = AudioLibrary::instance();
    DriverBinding b;
    b.images    = {image(kEngine, 0x6000)};
    b.tickEntry = 0x6000;
    b.slots     = {SlotSpec{.address = 0xC29A, .width = 1, .direction = SlotDirection::Read}};

    EXPECT_THROW((void)lib.uploadDriver<NoSlots>(b, kVerbs), std::invalid_argument);
}

// ── registerDriver: per-image paths ─────────────────────────────────────────────────────────────

// registerDriver (per-image paths) stores each image's path and per-image policy (resolved to bytes at host()); no bytes are
// copied at registration.
TEST(DriverRegistration, RegisterDriverStoresPathsAndPolicy) {
    AudioLibrary& lib = AudioLibrary::instance();
    DriverPathBinding pb;
    pb.images    = {DriverImagePath{.base = 0x6000, .path = "drivers/engine.asm",
                                    .policy = AssetPolicy::LoadFromPath},
                    DriverImagePath{.base = gb::banked(2, 0x4000), .path = "drivers/bank2.bin",
                                    .policy = AssetPolicy::Embed}};
    pb.mapper    = gb::Mbc3;
    pb.tickEntry = 0x6000;
    pb.isa       = Isa::Sm83;

    const DriverId<DemoSlots> id = lib.registerDriver(pb, kVerbs, slots(slot(&DemoSlots::volume, 0xC29A)));

    const AudioLibrary::Entry& e = lib.entry(id.id());
    EXPECT_EQ(e.kind, AudioKind::Driver);
    ASSERT_TRUE(e.driver.has_value());
    const DriverDefinition& d = *e.driver;
    ASSERT_EQ(d.images.size(), 2u);
    EXPECT_EQ(d.images[0].base, 0x6000u);
    EXPECT_EQ(d.images[0].path, "drivers/engine.asm");
    EXPECT_TRUE(d.images[0].bytes.empty());  // a path image copies no bytes at registration
    ASSERT_TRUE(d.images[0].policy.has_value());
    EXPECT_EQ(*d.images[0].policy, AssetPolicy::LoadFromPath);
    EXPECT_EQ(d.images[1].path, "drivers/bank2.bin");
    ASSERT_TRUE(d.images[1].policy.has_value());
    EXPECT_EQ(*d.images[1].policy, AssetPolicy::Embed);
}

// A per-image policy resolves through the same two-tier rule the rest of the asset surface uses: a named
// policy wins; an unset one falls to the per-type default (Embed for a driver image — a small blob).
TEST(DriverRegistration, ImagePolicyResolvesOverThePerTypeDefault) {
    AudioLibrary& lib = AudioLibrary::instance();
    DriverPathBinding pb;
    pb.images    = {DriverImagePath{.base = 0x6000, .path = "drivers/engine.asm",
                                    .policy = AssetPolicy::LoadFromPath},
                    DriverImagePath{.base = 0x7000, .path = "drivers/extra.bin"}};  // policy unset
    pb.tickEntry = 0x6000;

    const DriverId<NoSlots> id = lib.registerDriver(pb, kVerbs);
    const DriverDefinition& d = *lib.entry(id.id()).driver;

    EXPECT_EQ(resolveAssetPolicy(d.images[0].policy, AssetPolicy::Embed), AssetPolicy::LoadFromPath);
    EXPECT_EQ(resolveAssetPolicy(d.images[1].policy, AssetPolicy::Embed), AssetPolicy::Embed);
}

// ── Slots: widths, order, and the typed accessors ──────────────────────────────────────────────

// A slot's width comes from its field's optional value type, and the declaration order is the slot order.
TEST(DriverRegistration, SlotWidthsComeFromFieldTypeInDeclarationOrder) {
    AudioLibrary& lib = AudioLibrary::instance();
    DriverBinding b;
    b.images    = {image(kEngine, 0x6000)};
    b.tickEntry = 0x6000;

    const DriverId<DemoSlots> id = lib.uploadDriver(
        b, kVerbs,
        slots(slot(&DemoSlots::volume, 0xC29A, SlotDirection::ReadWrite),
                 slot(&DemoSlots::pitch, 0xC2B0, SlotDirection::Write),
                 slot(&DemoSlots::fade, 0xC2A7, SlotDirection::Read)));

    const DriverDefinition& d = *lib.entry(id.id()).driver;
    ASSERT_EQ(d.slots.size(), 3u);
    EXPECT_EQ(d.slots[0].address, 0xC29Au);
    EXPECT_EQ(d.slots[0].width, 1);  // std::optional<std::uint8_t>
    EXPECT_EQ(d.slots[0].direction, SlotDirection::ReadWrite);
    EXPECT_EQ(d.slots[1].address, 0xC2B0u);
    EXPECT_EQ(d.slots[1].width, 2);  // std::optional<std::uint16_t>
    EXPECT_EQ(d.slots[1].direction, SlotDirection::Write);
    EXPECT_EQ(d.slots[2].address, 0xC2A7u);
    EXPECT_EQ(d.slots[2].width, 1);
    EXPECT_EQ(d.slots[2].direction, SlotDirection::Read);
}

// The type-erased accessors round-trip a field value through the game's slots struct — the substrate the
// typed handle at host() reads and writes. An engaged field reads its value; a disengaged one reads false; a
// write sets the field to a slot value, honoring the field's width on the way in.
TEST(DriverRegistration, TypedAccessorsRoundTripAFieldValue) {
    AudioLibrary& lib = AudioLibrary::instance();
    DriverBinding b;
    b.images    = {image(kEngine, 0x6000)};
    b.tickEntry = 0x6000;

    const DriverId<DemoSlots> id = lib.uploadDriver(
        b, kVerbs,
        slots(slot(&DemoSlots::volume, 0xC29A),                        // index 0, width 1
                 slot(&DemoSlots::pitch, 0xC2B0, SlotDirection::Write)));  // index 1, width 2

    const DriverDefinition& d = *lib.entry(id.id()).driver;
    ASSERT_EQ(d.accessors.size(), 2u);

    DemoSlots s;
    s.volume = 0x42;  // engaged
    // pitch left disengaged
    std::uint64_t out = 0;
    EXPECT_TRUE(d.accessors[0].read(&s, out));  // volume engaged
    EXPECT_EQ(out, 0x42u);
    out = 0xFF;
    EXPECT_FALSE(d.accessors[1].read(&s, out));  // pitch disengaged → false, out untouched by contract
    EXPECT_EQ(out, 0xFFu);

    DemoSlots dst;
    d.accessors[1].write(&dst, 0x1234);  // a 2-byte slot value into the uint16 field
    ASSERT_TRUE(dst.pitch.has_value());
    EXPECT_EQ(*dst.pitch, 0x1234u);
    d.accessors[0].write(&dst, 0x99);
    ASSERT_TRUE(dst.volume.has_value());
    EXPECT_EQ(*dst.volume, 0x99u);
}

// ── Verbs: the player-verb realizations ─────────────────────────────────────────────────────────

// The declared verbs round-trip onto the stored definition: the per-lane play realizations and the stop.
TEST(DriverRegistration, VerbsAreStoredOnTheDefinition) {
    AudioLibrary& lib = AudioLibrary::instance();
    DriverBinding b;
    b.images    = {image(kEngine, 0x6000)};
    b.tickEntry = 0x6000;

    const DriverVerbs verbs{
        .play = {.music  = Instruction::write(Location::memory(0xC010), 1),
                 .sfx    = Instruction::call(0x6000, gb::A),
                 .vocals = std::nullopt},
        .stop = Instruction::write(Location::memory(0xC012), 1, /*fixedValue=*/1),
    };
    const DriverId<NoSlots> id = lib.uploadDriver(b, verbs);

    const DriverDefinition& d = *lib.entry(id.id()).driver;
    ASSERT_TRUE(d.verbs.play.music.has_value());
    EXPECT_EQ(d.verbs.play.music->kind(), Instruction::Kind::Write);
    ASSERT_TRUE(d.verbs.play.sfx.has_value());
    EXPECT_EQ(d.verbs.play.sfx->kind(), Instruction::Kind::Call);
    EXPECT_FALSE(d.verbs.play.vocals.has_value());  // an undeclared lane stays disengaged
    ASSERT_TRUE(d.verbs.stop.has_value());
    EXPECT_EQ(d.verbs.stop->fixedValue().value_or(0), 1u);
}

// A driver must declare its Music play verb — a driver you cannot cue music on is not playable. Both registration functions
// reject a verb set with no Music realization (the existing loud-throw registration posture).
TEST(DriverRegistration, MissingMusicVerbThrows) {
    AudioLibrary& lib = AudioLibrary::instance();
    DriverBinding b;
    b.images    = {image(kEngine, 0x6000)};
    b.tickEntry = 0x6000;
    const DriverVerbs noMusic{.play = {.sfx = Instruction::write(Location::memory(0xC011), 1)}};

    EXPECT_THROW((void)lib.uploadDriver<NoSlots>(b, noMusic), std::invalid_argument);

    DriverPathBinding pb;
    pb.images    = {DriverImagePath{.base = 0x6000, .path = "drivers/engine.asm"}};
    pb.tickEntry = 0x6000;
    EXPECT_THROW((void)lib.registerDriver<NoSlots>(pb, noMusic), std::invalid_argument);
}

// ── Id space ──────────────────────────────────────────────────────────────────────────────────────

// Driver registrations share the library's one dense AudioId space with chiptunes/PCM: two consecutive
// registrations mint consecutive ids, and the first equals the size captured just before it.
TEST(DriverRegistration, DriverIdsAreDenseAndAscending) {
    AudioLibrary& lib = AudioLibrary::instance();
    DriverBinding b;
    b.images    = {image(kEngine, 0x6000)};
    b.tickEntry = 0x6000;

    const std::size_t before = lib.size();
    const DriverId<NoSlots> a = lib.uploadDriver(b, kVerbs);
    const DriverId<NoSlots> c = lib.uploadDriver(b, kVerbs);

    EXPECT_EQ(static_cast<std::uint32_t>(a.id()), static_cast<std::uint32_t>(before));
    EXPECT_EQ(static_cast<std::uint32_t>(c.id()), static_cast<std::uint32_t>(before) + 1u);
}

}  // namespace
}  // namespace retropp
