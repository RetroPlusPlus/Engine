// Rom-run demo — a hosted cartridge living its own life, watched and steered while it runs.
//
// A game that hosts a whole cartridge can do more than read it: run() boots the image and gives it
// a thread of its own, holding the platform's hardware cadence however this program's loop behaves.
// This is that story end to end:
//
//   * The CARTRIDGE is authored right here — its program assembled by the engine's own SM83
//     assembler — so nothing comes from anyone's ROM. It is a commercial-shaped little game: an
//     interrupt vector, a halt loop, a frame counter, and an echo cell it copies every frame.
//   * The PLACES are declared before run(), because while the machine runs they are the observable
//     set: reads answer a coherent per-step publish, writes land at step boundaries.
//   * SPEED is a rational fraction of the hardware's own — the demo runs a stretch at {1,1}, doubles
//     it, halves it, pauses it, and the measured rates land on the arithmetic each time.
//   * The ECHO proves the round trip: this program writes a value into the running machine, the
//     GUEST's own loop copies it, and the copy comes back out through the publish.
//
// Headless: no window, no GPU, no audio device. Safe to run anywhere, including over SSH.
//
// With --verify, the demo also ASSERTS what it measures — each moving phase within ±5% of its
// target rate, the paused phase at exactly zero, the echo byte exact — and exits nonzero on any
// violation. CI runs it in that mode on every platform, so the printed numbers are captured in the
// test log and a governor that stops holding cadence turns the suite red.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "retropp/memory_region.h"  // MemoryRegion — where a place is
#include "retropp/vm.h"             // Vm — hosting the cartridge, running it, watching it

namespace {

// The cartridge's own program. Each VBlank it advances its frame counter and copies the echo cell —
// the whole "game loop" of this little cartridge, and enough to see it living from outside.
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
    ld [$C001], a       ; echo: whatever the outside wrote, the game copies
    jr loop
)";

// The places this program watches. Never instantiated — the struct exists so the places have names,
// and naming one wrong is a compile error rather than a bad address.
struct Places {
    retropp::MemoryRegion frames;  // the guest's own frame counter, in high RAM
    retropp::MemoryRegion echo;    // [0] = what this program writes, [1] = the guest's copy
};

// One authored cartridge: entry jumps to the program at $0150, the VBlank vector returns.
std::vector<std::uint8_t> authorGame(retropp::Vm& assembler) {
    std::vector<std::uint8_t> rom(0x8000, 0x00);
    rom[0x0143] = 0x80;  // CGB-flagged, so a Color machine boots it in CGB mode
    rom[0x0147] = 0x00;  // ROM only
    rom[0x0148] = 0x00;  // 32 KiB
    rom[0x0040] = 0xD9;  // VBlank vector: reti
    rom[0x0100] = 0x00;                                        // entry: nop
    rom[0x0101] = 0xC3; rom[0x0102] = 0x50; rom[0x0103] = 0x01;  // jp $0150
    const std::vector<std::uint8_t> code = assembler.assemble(std::string(kGameSource));
    std::copy(code.begin(), code.end(), rom.begin() + 0x150);
    return rom;
}

}  // namespace

int main(int argc, char** argv) {
    const bool verify = argc > 1 && std::strcmp(argv[1], "--verify") == 0;
    int failures = 0;

    retropp::Vm::GBC machine;
    machine.hostRom(authorGame(machine));

    const auto places = machine.registerRegions(retropp::regions(
        retropp::region(&Places::frames, retropp::MemoryRegion{.at = 0xFF80, .size = 1},
                        "frame counter"),
        retropp::region(&Places::echo, retropp::MemoryRegion{.at = 0xC000, .size = 1, .count = 2},
                        "echo cells")));

    const auto frames = [&] { return machine.read(places, &Places::frames).at(0); };

    std::printf("rom_run: an authored cartridge, booted and running on its own thread\n\n");
    machine.run();
    // Let the game reach its loop before measuring: power-on RAM is randomized (faithfully), and
    // the counter cell only means something once the game's own first instructions have zeroed it.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Watch it live at each speed, against the rate the factor owes. The counter is the guest's own
    // 8-bit cell, so a phase's frame count is reconstructed from mod-256 deltas sampled well inside
    // the wrap interval. Under --verify a moving phase must land within ±5% of its target — an
    // order of magnitude wider than the governor's measured error, an order of magnitude tighter
    // than any real defect — and the paused phase must not move at all.
    const auto watch = [&](const char* label, int seconds, double targetHz) {
        std::uint8_t  last  = frames();
        std::uint64_t total = 0;
        const auto    begin = std::chrono::steady_clock::now();
        for (int s = 0; s < seconds; ++s) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const std::uint8_t now = frames();
            total += static_cast<std::uint8_t>(now - last);
            last = now;
        }
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - begin;
        const auto [num, den] = machine.speed();
        const double measured = static_cast<double>(total) / elapsed.count();
        std::printf("  %-14s {%u,%u}  %5.1f frames/s over %.1f s (target %5.1f)\n", label, num, den,
                    measured, elapsed.count(), targetHz);
        if (verify) {
            const bool held = targetHz == 0.0 ? total == 0
                                              : std::fabs(measured - targetHz) <= targetHz * 0.05;
            if (!held) {
                std::printf("  VERIFY FAILED: %s measured %.2f against target %.2f\n", label,
                            measured, targetHz);
                ++failures;
            }
        }
    };

    constexpr double kHardwareHz = 4'194'304.0 / 70'224.0;  // the platform's own 59.7275
    watch("hardware speed", 6, kHardwareHz);
    machine.speed(2, 1);
    watch("doubled", 6, kHardwareHz * 2.0);
    machine.speed(1, 2);
    watch("halved", 6, kHardwareHz / 2.0);
    machine.speed(0, 1);
    watch("paused", 4, 0.0);             // a paused machine lives, frozen
    machine.speed(1, 1);

    // The round trip: write into the running machine, and read the guest's own copy back out. The
    // write lands at a step boundary; the guest's next loop iteration copies it; the publish after
    // that step carries the copy back.
    machine.write(places, &Places::echo, std::vector<std::uint8_t>{0x5A}, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::uint8_t echoed = machine.read(places, &Places::echo, 1).at(0);
    std::printf("\n  echo: wrote $5A into the running machine; the game copied back $%02X\n", echoed);
    if (verify && echoed != 0x5A) {
        std::printf("  VERIFY FAILED: the echo round trip returned $%02X\n", echoed);
        ++failures;
    }

    // Stopping parks the machine exactly where it is; running again resumes its life mid-count.
    machine.stop();
    const std::uint8_t parked = frames();
    machine.run();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    machine.stop();
    const std::uint8_t resumed = frames();
    std::printf("  resume: parked at frame %u, woke and lived on to %u\n\n", parked, resumed);
    if (verify && static_cast<std::uint8_t>(resumed - parked) == 0) {
        std::printf("  VERIFY FAILED: the resumed machine did not advance\n");
        ++failures;
    }

    std::printf("done — the cartridge ran its own loop the whole time; this program only watched\n");
    return failures == 0 ? 0 : 1;
}
