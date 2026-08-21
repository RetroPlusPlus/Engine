#pragma once

// The two synthetic sound drivers this demo hosts — hand-written SM83 assembled in-process (NO ROM), one
// per driver family the hosting surface supports. They expose the IDENTICAL interaction surface: a music
// lane, an sfx lane, a volume slot, and a stop verb. That is the point of the feature — the call sites are
// the same player verbs for both, so a game speaks play(id) / play(id, Sfx) / stop() / slots(...) without
// caring which family it hosts. The ONE difference lives here, at registration:
//
//   * the RAM-FLAG family (the mailbox lineage) realizes each verb as Instruction::write — the id
//     lands in a memory mailbox the driver polls each tick; and
//   * the ARGUMENT family (the tracker-driver lineage) realizes each verb as Instruction::call — the id rides a
//     CPU register into an entry the engine calls.
//
// Both drivers share one game-facing slot struct (DemoSlots) and drive the same channels the same way, so
// their panels are mirror images. Registration is the one hardware site (image bytes, placement, the
// mailbox / register / entry a verb targets, the memory a slot maps to); after it, no address or register
// idiom appears at any call site. A real game would extract these images from its own driver; here they are
// assembled from source so the demo is self-contained.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "retropp/audio_library.h"
#include "retropp/driver_binding.h"
#include "retropp/gb.h"
#include "retropp/vm.h"

namespace demo {

// Assemble SM83 source to image bytes through a throwaway VM (the engine's own assembler — the same path a
// registered ".asm" driver image takes at host time).
inline std::vector<std::uint8_t> asm83(const std::string& source) {
    retropp::Vm probe{retropp::VMPlatform::GameBoyColor};
    return probe.assemble(source);
}

// The game-facing slot struct BOTH drivers share. A field's TYPE carries the slot's width; declaration
// order is the slot index. Every field is std::optional so a slots(...) write names only the fields it
// changes and a slots() read comes back with only the readable fields engaged.
struct DemoSlots {
    std::optional<std::uint8_t> musicLastSeen;  // 0: $C020 - the last id the music lane played (Read)
    std::optional<std::uint8_t> sfxLastSeen;    // 1: $C021 - the last id the sfx lane played (Read)
    std::optional<std::uint8_t> volume;         // 2: $C022 - the driver's own master volume (ReadWrite)
};

// The static APU + wave-channel setup both drivers run in their .init (the engine calls it once at host()):
// enable the APU, load a triangle into CH3's wave RAM, route every channel L+R, and switch CH3's DAC on. It
// does NOT trigger a tone — the first sound waits for the first play, exactly as a resident driver behaves.
// It also zeroes the driver's state RAM ($C010..$C030): post-reset WRAM is not zero, and a garbage mailbox
// would otherwise read as a spurious play on the first tick.
inline const char* kInitSource() {
    return R"(
  ld hl, $C010
  ld c, $21
  xor a
.clr:
  ld [hl+], a
  dec c
  jr nz, .clr
  ld a, $80
  ldh [$FF26], a        ; NR52 - APU master enable
  ld a, $00
  ldh [$FF1A], a        ; NR30 - CH3 DAC off before writing wave RAM
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
  ld a, $FE
  ldh [$FF38], a
  ld a, $DC
  ldh [$FF39], a
  ld a, $BA
  ldh [$FF3A], a
  ld a, $98
  ldh [$FF3B], a
  ld a, $76
  ldh [$FF3C], a
  ld a, $54
  ldh [$FF3D], a
  ld a, $32
  ldh [$FF3E], a
  ld a, $10
  ldh [$FF3F], a
  ld a, $FF
  ldh [$FF25], a        ; NR51 - route all channels L+R
  ld a, $77
  ldh [$FF24], a        ; NR50 - master volume L+R (the volume slot overrides this)
  ld a, $80
  ldh [$FF1A], a        ; NR30 - CH3 DAC on
  ld a, $20
  ldh [$FF1C], a        ; NR32 - CH3 output level 100%
  ret
)";
}

// Trigger CH3 (wave) at the frequency in A, switching its DAC back on (a prior stop may have cleared it).
inline const char* kPlayMusicBody() {
    return R"(
  ld b, a
  ld a, $80
  ldh [$FF1A], a        ; NR30 - CH3 DAC on
  ld a, b
  ldh [$FF1D], a        ; NR33 - CH3 frequency low = the id (pitch tracks it)
  ld a, $86
  ldh [$FF1E], a        ; NR34 - trigger + frequency high bits
)";
}

// Fire a short decaying CH1 (square) blip at the frequency in A.
inline const char* kPlaySfxBody() {
    return R"(
  ld b, a
  ld a, $80
  ldh [$FF11], a        ; NR11 - CH1 duty 50%
  ld a, $F3
  ldh [$FF12], a        ; NR12 - CH1 envelope: full volume, decaying (a blip)
  ld a, b
  ldh [$FF13], a        ; NR13 - CH1 frequency low = the id
  ld a, $87
  ldh [$FF14], a        ; NR14 - trigger + frequency high bits
)";
}

// Apply the volume slot ($C022) to the master volume when engaged (0 means "leave it alone").
inline const char* kApplyVolumeBody() {
    return R"(
  ld a, [$C022]
  or a
  jr z, .volDone
  ldh [$FF24], a        ; NR50 - master volume from the slot
.volDone:
)";
}

// ── The RAM-flag driver ─────────────────────────────────────────────────────────────────────────────
//
// Each verb writes a mailbox ($C010 music, $C011 sfx, $C012 stop) that the tick polls, acts on, records to
// a last-seen byte, and clears (so a sound does not re-trigger on later idle ticks). The tick also applies
// the volume slot each pass.
inline std::string kRamTickSource() {
    return std::string(R"(
  ld a, [$C010]         ; music mailbox
  or a
  jr z, .sfx
  ld [$C020], a         ; music last-seen
  ld a, [$C010]
)") + kPlayMusicBody() + R"(
  xor a
  ld [$C010], a         ; clear the music mailbox
.sfx:
  ld a, [$C011]         ; sfx mailbox
  or a
  jr z, .stopf
  ld [$C021], a         ; sfx last-seen
  ld a, [$C011]
)" + kPlaySfxBody() + R"(
  xor a
  ld [$C011], a         ; clear the sfx mailbox
.stopf:
  ld a, [$C012]         ; stop mailbox
  or a
  jr z, .vol
  xor a
  ldh [$FF1A], a        ; NR30 - CH3 DAC off
  ldh [$FF12], a        ; NR12 - CH1 DAC off
  ld [$C012], a         ; clear the stop mailbox
.vol:
)" + kApplyVolumeBody() + "  ret\n";
}

// Register the RAM-flag driver on the library. Verbs are Instruction::write (a mailbox). init at $6000,
// tick at $6100.
inline retropp::DriverId<DemoSlots> registerRamDriver() {
    using namespace retropp;
    const std::vector<std::uint8_t> initBytes = asm83(kInitSource());
    const std::vector<std::uint8_t> tickBytes = asm83(kRamTickSource());
    DriverBinding b;
    b.images    = {DriverImage{.bytes = initBytes, .base = 0x6000},
                   DriverImage{.bytes = tickBytes, .base = 0x6100}};
    b.tickEntry = 0x6100;
    b.init      = Instruction::call(0x6000, gb::A, /*fixedValue=*/0);
    const DriverVerbs verbs{
        .play = {.music = Instruction::write(Location::memory(0xC010), 1),
                 .sfx   = Instruction::write(Location::memory(0xC011), 1)},
        .stop = Instruction::write(Location::memory(0xC012), 1, /*fixedValue=*/1),
    };
    return AudioLibrary::instance().uploadDriver(
        b, verbs,
        slots(slot(&DemoSlots::musicLastSeen, 0xC020, SlotDirection::Read),
              slot(&DemoSlots::sfxLastSeen, 0xC021, SlotDirection::Read),
              slot(&DemoSlots::volume, 0xC022, SlotDirection::ReadWrite)));
}

// ── The argument driver ─────────────────────────────────────────────────────────────────────────────
//
// Each verb is an entry the engine CALLS with the id in register A: the music / sfx entries record the id
// and trigger immediately; the stop entry silences both channels. The tick applies the volume slot each
// pass (the same slot the RAM driver has — a slot is memory in both families).
inline std::string kArgMusicSource() {
    return std::string("  ld [$C020], a         ; music last-seen\n") + kPlayMusicBody() + "  ret\n";
}
inline std::string kArgSfxSource() {
    return std::string("  ld [$C021], a         ; sfx last-seen\n") + kPlaySfxBody() + "  ret\n";
}
inline const char* kArgStopSource() {
    return R"(
  xor a
  ldh [$FF1A], a        ; NR30 - CH3 DAC off
  ldh [$FF12], a        ; NR12 - CH1 DAC off
  ret
)";
}
inline std::string kArgTickSource() { return kApplyVolumeBody() + std::string("  ret\n"); }

// Register the argument driver. Verbs are Instruction::call (an entry, id in register A). init $6000,
// music entry $6100, sfx entry $6200, stop entry $6300, tick $6400.
inline retropp::DriverId<DemoSlots> registerArgDriver() {
    using namespace retropp;
    const std::vector<std::uint8_t> initBytes  = asm83(kInitSource());
    const std::vector<std::uint8_t> musicBytes = asm83(kArgMusicSource());
    const std::vector<std::uint8_t> sfxBytes   = asm83(kArgSfxSource());
    const std::vector<std::uint8_t> stopBytes  = asm83(kArgStopSource());
    const std::vector<std::uint8_t> tickBytes  = asm83(kArgTickSource());
    DriverBinding b;
    b.images    = {DriverImage{.bytes = initBytes, .base = 0x6000},
                   DriverImage{.bytes = musicBytes, .base = 0x6100},
                   DriverImage{.bytes = sfxBytes, .base = 0x6200},
                   DriverImage{.bytes = stopBytes, .base = 0x6300},
                   DriverImage{.bytes = tickBytes, .base = 0x6400}};
    b.tickEntry = 0x6400;
    b.init      = Instruction::call(0x6000, gb::A, /*fixedValue=*/0);
    const DriverVerbs verbs{
        .play = {.music = Instruction::call(0x6100, gb::A),
                 .sfx   = Instruction::call(0x6200, gb::A)},
        .stop = Instruction::call(0x6300, gb::A, /*fixedValue=*/0),
    };
    return AudioLibrary::instance().uploadDriver(
        b, verbs,
        slots(slot(&DemoSlots::musicLastSeen, 0xC020, SlotDirection::Read),
              slot(&DemoSlots::sfxLastSeen, 0xC021, SlotDirection::Read),
              slot(&DemoSlots::volume, 0xC022, SlotDirection::ReadWrite)));
}

}  // namespace demo
