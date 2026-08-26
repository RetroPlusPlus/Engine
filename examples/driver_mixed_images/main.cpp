// Mixed driver images — one hosted driver built from two kinds of source at once.
//
// A game extending a cartridge often has a sound driver that is partly its own and partly the player's.
// This demo hosts one of those. Its two images are declared side by side on a single HostedDriverBinding
// and arrive by completely different routes:
//
//   * The TICK routine comes out of a cartridge at runtime. It is given as BYTES, so it carries no path
//     and no policy — no build step ever sees it, and nothing about it is written into the binary. That
//     is the posture for content a game has no right to ship inside its own executable.
//   * The SETUP routine is assets/driver_init.asm, declared as an Embed DriverImagePath. The build reads
//     that literal, assembles it, and bakes the bytecode in. It is the project's own code, so it ships.
//
// The cartridge is hosted, read, and DESTROYED before the driver is ever hosted — the point of copying at
// registration. Nothing that produced a byte image has to stay alive: a game may host a cartridge purely
// to read it and reclaim the machine as soon as the reads return.
//
// The cartridge here is authored in-process, so this demo ships nobody's ROM. A real game hosts one the
// player supplied. Sound comes out of the default audio device; there is no window.

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

#include "retropp/audio_library.h"  // HostedDriverBinding — a binding whose images name their own source
#include "retropp/audio_system.h"   // AudioSystem + HostedDriver — hosting and driving the driver
#include "retropp/engine_config.h"  // EngineConfig — where LoadFromPath resolves
#include "retropp/gb.h"             // gb::A — the register the init call rides
#include "retropp/memory_region.h"  // MemoryRegion — where the cartridge keeps its audio section
#include "retropp/sdl_platform.h"   // SdlAudioSink — a real device stream
#include "retropp/vm.h"             // Vm — hosting the cartridge and reading it

namespace {

using namespace retropp;

// Where the driver's images sit in the machine it runs on.
constexpr std::uint32_t kSetupBase = 0x6000;
constexpr std::uint32_t kTickBase  = 0x6100;

// Where this cartridge keeps its audio section. A real game gets this from a disassembly's symbol file.
constexpr std::uint32_t kAudioSectionBase  = 0x4000;
constexpr std::uint32_t kCartridgeBytes    = 32 * 1024;

// The driver's state, named. Each tick consumes a non-zero music mailbox: it records what was played,
// sets CH3's frequency from it and triggers, then clears the mailbox so an idle tick does not re-trigger.
struct DemoSlots {
    std::optional<std::uint8_t> mailbox;        // $C010 — what play(id) writes
    std::optional<std::uint8_t> musicLastSeen;  // $C020 — what the tick recorded
};

// The tick routine, as it would sit inside a cartridge's audio section.
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

// The one place this demo declares inside the cartridge.
struct Places {
    MemoryRegion audio;
};

// Host a cartridge, read its audio section, register the driver from those bytes beside the project's own
// setup image — then let the cartridge and the bytes go. Everything the registration needs, it copied.
DriverId<DemoSlots> registerDriverFromACartridge() {
    Vm::GBC cartridge;

    // Author the cartridge and host it. A real game hands hostRom the image the player supplied; this one
    // writes the audio section into a blank image so it ships no cartridge content of its own.
    const std::vector<std::uint8_t> tick = cartridge.assemble(kTickSource);
    std::vector<std::uint8_t> image(kCartridgeBytes, 0x00);
    cartridge.hostRom(image);

    const auto places = cartridge.registerRegions(regions(
        region(&Places::audio,
               MemoryRegion{.at = kAudioSectionBase, .size = static_cast<std::uint32_t>(tick.size())},
               "audio section")));
    cartridge.write(places, &Places::audio, tick);

    const std::vector<std::uint8_t> audioSection = cartridge.read(places, &Places::audio);
    std::printf("read %zu bytes out of the hosted cartridge's audio section\n", audioSection.size());

    // One binding, two sources. The bytes read above, and a path the build resolves on its own.
    HostedDriverBinding binding{
        .images    = {DriverImage{.bytes = audioSection, .base = kTickBase},
                      DriverImagePath{.base   = kSetupBase,
                                      .path   = "examples/driver_mixed_images/assets/driver_init.asm",
                                      .policy = AssetPolicy::Embed}},
        .tickEntry = kTickBase,
        .init      = Instruction::call(kSetupBase, gb::A, /*fixedValue=*/0),
        .isa       = Isa::Sm83,
    };
    const DriverVerbs verbs{
        .play = {.music = Instruction::write(Location::memory(0xC010), 1)},
        .stop = Instruction::write(Location::memory(0xC012), 1, /*fixedValue=*/1),
    };

    return AudioLibrary::instance().registerDriver(
        binding, verbs,
        slots(slot(&DemoSlots::mailbox, 0xC010, SlotDirection::Read),
              slot(&DemoSlots::musicLastSeen, 0xC020, SlotDirection::Read)));
}

}  // namespace

int main() {
    // Audio only — no video subsystem, so no window is ever created.
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
        return 1;
    }

    EngineConfig config;
    config.identity = {.organization = "Retro++", .application = "DriverMixedImagesDemo"};
    EngineConfig::setActive(config);

    const DriverId<DemoSlots> id = registerDriverFromACartridge();
    std::printf("registered — the cartridge and the bytes it produced are both gone now\n\n");

    {
        // host() places both images: the baked setup image is read out of the binary, the byte image out
        // of the copy the registration took. Neither the cartridge nor the buffer it filled still exists.
        SdlAudioSink sink;
        AudioSystem::GBC sys{AudioKind::Chiptune, sink};
        HostedDriver<DemoSlots> driver = sys.host(id);
        std::printf("hosted — setup image from the binary, tick image from the copied bytes\n");

        driver.play(0x40);
        std::printf("playing id $40 for three seconds\n");
        std::this_thread::sleep_for(std::chrono::seconds(3));

        const DemoSlots read = driver.slots();
        std::printf("\nthe driver's own state, read back through the handle:\n");
        std::printf("  music last seen: $%02X\n", read.musicLastSeen.value_or(0));
        std::printf("  mailbox:         $%02X (the tick consumed it)\n", read.mailbox.value_or(0));

        driver.stop();
        driver.close();
    }

    SDL_Quit();
    return 0;
}
