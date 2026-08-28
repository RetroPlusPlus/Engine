// Guest-escape demo — a running cartridge hands control to native code and carries on.
//
// Reading and writing a hosted cartridge reach into it from outside. An escape is the other
// direction: a place in the cartridge's OWN code where control leaves the guest, runs this program's
// C++, and resumes. This is that story end to end, in two acts:
//
//   * WATCHING. The cartridge is authored right here, every byte of it this file's own, its program
//     assembled by the engine's own SM83 assembler. It is a small game loop that
//     calls its own routine every frame. An escape declared at that routine counts the calls from
//     C++, while the machine runs on its own thread at hardware cadence.
//   * ANSWERING. Then the routine is replaced by DECLARING it replaced: `.replaces = routine(...)`
//     binds a native function to the routine's own calling convention — the guest's loop carries the
//     seed to it in B and reads the answer out of A, so the binding says exactly that — and the
//     engine answers every call synchronously, in the register the loop was always going to read.
//     The guest goes on running, against a value its own code never produced.
//
// Headless: no window, no GPU, no audio device. Safe to run anywhere, including over SSH.
//
// With --verify the demo asserts what it observes — that the escape fired while the guest ran, that
// the guest's own rule produced its answer before the patch and this program's answer after it — and
// exits nonzero on any violation.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "retropp/gb.h"             // gb::A / gb::B — the routine's own calling convention, named
#include "retropp/guest_escape.h"   // GuestEscape — where control leaves the guest
#include "retropp/memory_region.h"  // MemoryRegion — where a place is
#include "retropp/vm.h"             // Vm — hosting the cartridge, running it, watching it

namespace {

// Where the cartridge's pieces live. The loop and the routine are assembled as separate blocks and
// placed at these addresses, so the loop's `call` names an address both sides agree on.
constexpr std::uint32_t kProgram = 0x0150;  // the game loop
constexpr std::uint32_t kRule    = 0x0180;  // the cartridge's own routine
constexpr std::uint32_t kSeed    = 0xC000;  // what this program asks the guest about
constexpr std::uint32_t kResult  = 0xC001;  // what the loop publishes each frame

// Every VBlank the loop calls the routine — seed in B, answer back in A, the routine's own calling
// convention — and publishes the answer for the outside to read.
constexpr std::string_view kGameSource = R"(
    ld a, $01
    ldh [$FFFF], a      ; IE: VBlank only
    xor a
    ldh [$FF80], a      ; the frame counter starts at zero
    ldh [$FF0F], a      ; count from the NEXT VBlank, not a stale pending one
    ei
loop:
    halt
    nop
    ldh a, [$FF80]
    inc a
    ldh [$FF80], a      ; one more frame lived
    ld a, [$C000]
    ld b, a             ; the seed rides to the routine in B
    call $0180          ; the cartridge's own routine
    ld [$C001], a       ; the answer comes back in A, published for the outside
    jr loop
)";

// The routine, and its convention: seed in B, answer in A. The rule is the cartridge's own.
constexpr std::string_view kRuleSource = R"(
    ld a, b
    add a, a            ; double the seed
    ret
)";

// The places this program watches. Never instantiated — the struct exists so the places have names,
// and naming one wrong is a compile error rather than a bad address.
struct Places {
    retropp::MemoryRegion frames;  // the guest's own frame counter, in high RAM
    retropp::MemoryRegion seed;    // what this program asks the guest about
    retropp::MemoryRegion result;  // what the guest's loop publishes
};

// One authored cartridge: entry jumps to the loop, the VBlank vector returns, the routine sits where
// the loop calls it.
std::vector<std::uint8_t> authorGame(retropp::Vm& assembler) {
    std::vector<std::uint8_t> rom(0x8000, 0x00);
    rom[0x0143] = 0x80;  // CGB-flagged, so a Color machine boots it in CGB mode
    rom[0x0147] = 0x00;  // ROM only
    rom[0x0148] = 0x00;  // 32 KiB
    rom[0x0040] = 0xD9;  // VBlank vector: reti
    rom[0x0100] = 0x00;
    rom[0x0101] = 0xC3;
    rom[0x0102] = 0x50;
    rom[0x0103] = 0x01;  // jp $0150

    const std::vector<std::uint8_t> loop = assembler.assemble(std::string(kGameSource));
    const std::vector<std::uint8_t> rule = assembler.assemble(std::string(kRuleSource));
    std::copy(loop.begin(), loop.end(), rom.begin() + kProgram);
    std::copy(rule.begin(), rule.end(), rom.begin() + kRule);
    return rom;
}

void report(const char* what, bool ok, int& failures) {
    std::printf("  %-46s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool verify   = argc > 1 && std::strcmp(argv[1], "--verify") == 0;
    int        failures = 0;

    retropp::Vm::GBC machine;
    machine.hostRom(authorGame(machine));

    const auto places = machine.registerRegions(retropp::regions(
        retropp::region(&Places::frames, retropp::MemoryRegion{.at = 0xFF80, .size = 1},
                        "frame counter"),
        retropp::region(&Places::seed, retropp::MemoryRegion{.at = kSeed, .size = 1}, "seed"),
        retropp::region(&Places::result, retropp::MemoryRegion{.at = kResult, .size = 1},
                        "result")));

    const auto byteAt = [&](retropp::MemoryRegion Places::*key) {
        return machine.read(places, key).at(0);
    };

    std::printf("guest_escape: an authored cartridge, running, escaping into this program\n");
    std::printf("  the cartridge's own routine is at 0x%04X\n\n", kRule);

    // ── Act one: watching ───────────────────────────────────────────────────────────────────────
    // One escape at the routine's entry. The handler fires on the machine's own thread, so what it
    // touches is atomic.
    std::atomic<int> calls{0};
    machine.registerEscapes(retropp::escapes(retropp::GuestEscape{
        .key     = "rule called",
        .at      = kRule,
        .handler = [&](retropp::Vm&, std::uint32_t) { calls.fetch_add(1); }}));

    machine.run();
    // Seed it once the machine is living: boot fills power-on RAM the way hardware does, with
    // whatever was there, so a value written before boot is not the value the guest reads after it.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    machine.write(places, &Places::seed, std::vector<std::uint8_t>{21});
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    machine.stop();

    // Read everything PARKED, and measure against the guest's own clock. How many frames fit in a
    // wall-clock sleep varies run to run — the sleep's edges do not land on the guest's frame
    // boundaries — but the relation the escape promises is exact: the loop calls its routine once
    // per frame it lives, so the fires match the guest's own frame counter (±1 when the park lands
    // between the counter's increment and the call).
    const int          watched    = calls.load();
    const std::uint8_t guestSays  = byteAt(&Places::result);
    const int          livedOne   = byteAt(&Places::frames);

    std::printf("act one — the guest runs its own rule\n");
    std::printf("  the guest lived %d frames; the escape fired %d times — once per frame\n",
                livedOne, watched);
    std::printf("  seed 21, and the cartridge's rule doubled it to %u\n\n", guestSays);

    // ── Act two: answering ──────────────────────────────────────────────────────────────────────
    // One declaration replaces the routine. The binding transcribes the convention the routine
    // already has — its caller loads the seed into B and reads the answer from A — and the engine
    // does the rest: the routine's entry holds the machine's own return while this is armed, the
    // seed is read out of B (the guest's loop put it there), the lambda's answer is written into A,
    // and the caller reads it in the same step. Switching the escape off restores the routine.
    machine.escapes()["rule called"].remove();

    std::atomic<int> answered{0};
    machine.registerEscapes(retropp::escapes(retropp::GuestEscape{
        .key      = "answer the rule",
        .at       = kRule,
        .replaces = retropp::routine(
            retropp::RoutineBinding{.inputs = {retropp::gb::B}, .output = retropp::gb::A},
            [&](std::uint8_t seed) -> std::uint8_t {
                answered.fetch_add(1);
                return static_cast<std::uint8_t>(seed + 100);  // this program's rule, not the cartridge's
            })}));

    machine.run();  // resumes from the park — the frame counter carries on, not from zero
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    machine.write(places, &Places::seed, std::vector<std::uint8_t>{21});
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    machine.stop();

    const int          natives    = answered.load();
    const std::uint8_t nativeSays = byteAt(&Places::result);
    const int          livedTwo   = (byteAt(&Places::frames) - livedOne) & 0xFF;

    std::printf("act two — this program answers the routine instead\n");
    std::printf("  the guest lived %d more frames; the native rule answered %d times — once per "
                "frame,\n  and the cartridge's rule never executed\n", livedTwo, natives);
    std::printf("  seed 21, and this program's answer came back as %u\n\n", nativeSays);

    if (verify) {
        std::printf("verify\n");
        report("the escape fired while the guest ran", watched > 0, failures);
        report("act one: one fire per frame the guest lived",
               std::abs(watched - livedOne) <= 1, failures);
        report("the cartridge's own rule produced 42", guestSays == 42, failures);
        report("the native handler ran", natives > 0, failures);
        report("act two: one answer per frame the guest lived",
               std::abs(natives - livedTwo) <= 1, failures);
        report("this program's answer produced 121", nativeSays == 121, failures);
        std::printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
    }

    return failures == 0 ? 0 : 1;
}
