#pragma once

// The tone driver this demo hosts, hand-written SM83 assembled in-process (NO ROM). It is deliberately
// the smallest resident driver that makes a sound: one mailbox carrying a pitch, one tick that acts on
// it, one readable slot reporting what it last played.
//
// The demo hosts MANY copies of it. Each registration mints its own DriverId, and hosting one gives it
// its own machine — so the count of live machines is the demo's only variable, and every machine costs
// the same to run as every other.
//
// A real game would extract its driver's images from its own sound engine; here they are assembled from
// source so the demo is self-contained.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "retropp/audio_library.h"
#include "retropp/driver_binding.h"
#include "retropp/gb.h"
#include "retropp/vm.h"

namespace demo {

// Assemble SM83 source to image bytes through a throwaway VM — the engine's own assembler, the same
// path a registered ".asm" driver image takes at host time.
inline std::vector<std::uint8_t> asm83(const std::string& source) {
    retropp::Vm probe{retropp::VMPlatform::GameBoyColor};
    return probe.assemble(source);
}

// The game-facing slot struct. A field's TYPE carries the slot's width; declaration order is the slot
// index. The field is std::optional so a read comes back with only the readable fields engaged.
struct ToneSlots {
    std::optional<std::uint8_t> lastPitch;  // 0: $C020 - the pitch this machine last played (Read)
};

// The .init the engine performs once when the machine is placed: zero the driver's state RAM, enable
// the APU, load a wave into CH3, route it, and switch its DAC on. The first sound waits for the first
// play, exactly as a resident driver behaves.
//
// Zeroing state RAM first is not optional: a machine powers on with work RAM already holding values,
// and a garbage mailbox reads as a spurious play on the very first tick.
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
  xor a
  ldh [$FF1A], a        ; NR30 - CH3 DAC off while wave RAM is written
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
  ldh [$FF25], a        ; NR51 - route every channel L+R
  ld a, $77
  ldh [$FF24], a        ; NR50 - master volume L+R, at maximum: the machine sounds as loud as it can
                        ; and keeping the summed mix in range is the bus level's job, not the guest's
  ld a, $80
  ldh [$FF1A], a        ; NR30 - CH3 DAC on
  ld a, $20
  ldh [$FF1C], a        ; NR32 - CH3 output level 100%
  ret
)";
}

// The tick: poll the pitch mailbox, and when it carries a pitch, record it, trigger CH3 there, and
// clear the mailbox so the trigger happens once. The tone then drones until the next play.
inline const char* kTickSource() {
    return R"(
  ld a, [$C010]         ; pitch mailbox
  or a
  jr z, .done
  ld [$C020], a         ; last-played slot
  ld b, a
  ld a, $80
  ldh [$FF1A], a        ; NR30 - CH3 DAC on
  ld a, b
  ldh [$FF1D], a        ; NR33 - CH3 frequency low = the pitch
  ld a, $86
  ldh [$FF1E], a        ; NR34 - trigger + frequency high bits
  xor a
  ld [$C010], a         ; clear the mailbox
.done:
  ret
)";
}

// The stop verb: silence CH3 by switching its DAC off.
inline const char* kStopSource() {
    return R"(
  xor a
  ldh [$FF1A], a        ; NR30 - CH3 DAC off
  ret
)";
}

// Register one tone driver on the library and return its id. Call it once per machine the demo wants:
// each id is its own registration, and hosting each one gives it its own machine.
inline retropp::DriverId<ToneSlots> registerToneDriver() {
    using namespace retropp;
    const std::vector<std::uint8_t> initBytes = asm83(kInitSource());
    const std::vector<std::uint8_t> tickBytes = asm83(kTickSource());
    const std::vector<std::uint8_t> stopBytes = asm83(kStopSource());
    DriverBinding b;
    b.images    = {DriverImage{.bytes = initBytes, .base = 0x6000},
                   DriverImage{.bytes = tickBytes, .base = 0x6100},
                   DriverImage{.bytes = stopBytes, .base = 0x6200}};
    b.tickEntry = 0x6100;
    b.init      = Instruction::call(0x6000, gb::A, /*fixedValue=*/0);
    const DriverVerbs verbs{
        .play = {.music = Instruction::write(Location::memory(0xC010), 1)},
        .stop = Instruction::call(0x6200, gb::A, /*fixedValue=*/0),
    };
    return AudioLibrary::instance().uploadDriver(
        b, verbs, slots(slot(&ToneSlots::lastPitch, 0xC020, SlotDirection::Read)));
}

}  // namespace demo
