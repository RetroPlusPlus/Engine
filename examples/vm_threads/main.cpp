// Machines and threads — what a growing set of resident machines costs the audio that carries them.
//
// Every hosted sound driver is a whole machine, stepped once per frame to produce that frame's samples.
// This demo hosts as many of them as you ask for — RIGHT adds one, LEFT removes one — each on its own
// pitch, and reads the output ring's health while they run.
//
// The readout is the point:
//
//   MACHINES        how many are hosted right now
//   RING FRAMES     what is buffered ahead of the device (the latency buffer, ~50 ms when healthy)
//   UNDERFLOW       frames the device asked for and the ring could not supply, since start
//   PER SECOND      the same count over the last second — the live signal
//   DROPPED         mixed frames the ring had no room for
//   BUS             the VMDriver mixer level, scaled by the machine count so the sum stays in range
//   LANE STARVED    frames of silence the mix substituted for the machines over the last second
//   WORST MACHINE   the largest share of that any one machine accounted for
//   MACHINES BEHIND how many machines were short at all
//
// The last three read every machine's own count through its handle, and they are what separates a mix
// arriving late from the machines inside it: UNDERFLOW is the device going hungry, LANE STARVED is the
// machines that could not fill their share. Add machines until PER SECOND starts counting and the tone
// breaks up: that is the point where producing every machine's frame costs more than the time that
// frame is worth. Where that point sits is the measurement this demo exists to take.
//
// The driver is hand-assembled in synthetic_drivers.h, so the demo needs no ROM. Each machine plays a
// continuous tone, so the mix is steady and a break in it is audible rather than a matter of taste.
//
// RIGHT and LEFT add and remove one machine; UP and DOWN move in coarse steps, for crossing the range
// quickly. SPACE re-triggers every machine's tone, F toggles fullscreen. Close the window to quit.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "retropp/audio_library.h"
#include "retropp/audio_mixer.h"
#include "retropp/audio_system.h"
#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

#include "synthetic_drivers.h"

using namespace retropp;
using namespace demo;

namespace {

constexpr int kTilePx = 8;
constexpr int kGlyphPx = 16, kGlyphStride = 128 / kTilePx;  // font.png is 128 wide
constexpr int kCols = 44, kRows = 18;
constexpr int kViewW = kCols * kGlyphPx, kViewH = kRows * kGlyphPx;

// How many machines the demo can reach. Each hosted machine is a whole console — its own memory, its
// own sound hardware — so the count is also what the demo costs in memory. Raise it if the machine
// running this gets to the ceiling without the tone ever breaking up.
constexpr int kMaxMachines = 512;

// How many machines the coarse keys add or remove at once, for crossing the interesting range quickly.
constexpr int kCoarseStep = 16;

// Sim ticks between two samples of the underflow counter — the window the per-second figure covers.
constexpr int kSampleTicks = 60;

enum class Action : std::uint8_t { Add, Remove, AddMany, RemoveMany, Retrigger, Fullscreen };

// The font sheet carries digits, then letters, then a blank. Anything else lands on the blank.
[[nodiscard]] std::size_t glyphCell(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::size_t>(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return static_cast<std::size_t>(10 + (ch - 'A'));
    if (ch >= 'a' && ch <= 'z') return static_cast<std::size_t>(10 + (ch - 'a'));
    return 36;
}

// `n` right-aligned in `width` columns, so a rolling digit never shifts its row.
[[nodiscard]] std::string pad(std::uint64_t n, int width) {
    std::string s = std::to_string(n);
    while (static_cast<int>(s.size()) < width) s.insert(s.begin(), ' ');
    return s;
}

// The pitch machine `i` plays. Every machine gets its own, so the ear can tell how many are sounding.
[[nodiscard]] std::uint32_t pitchFor(std::size_t i) {
    return static_cast<std::uint32_t>(0x20 + (i % kMaxMachines) * 9);
}

}  // namespace

int main() {
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "VmThreads"},
        .window   = {.title = "Retro++ — machines and threads"},
        .viewport = ViewportResolution{kViewW, kViewH},
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Add, {SDL_SCANCODE_RIGHT, PadButton::DpadRight}},
        {Action::Remove, {SDL_SCANCODE_LEFT, PadButton::DpadLeft}},
        {Action::AddMany, {SDL_SCANCODE_UP, PadButton::DpadUp}},
        {Action::RemoveMany, {SDL_SCANCODE_DOWN, PadButton::DpadDown}},
        {Action::Retrigger, {SDL_SCANCODE_SPACE, PadButton::FaceSouth}},
        {Action::Fullscreen, {SDL_SCANCODE_F, PadButton::Select}},
    };
    platform.actions(map);

    // ── The readout's font and its three palettes ────────────────────────────────────────────────
    const AtlasManifest font =
        renderer.loadAtlas("examples/vm_threads/assets/art/font.png", AssetDimensions{kGlyphPx, kGlyphPx},
                           ContentKind::Tileset, ReadOrder::LeftRightThenDown, 64,
                           TransparentIndices::of({0}), 0, AssetPolicy::Embed);
    const PaletteId palText = renderer.loadPaletteImage("examples/vm_threads/assets/palettes/font.png",
                                                        ReadOrder::LeftRightThenDown, 0,
                                                        AssetPolicy::Embed);
    const PaletteId palLive = renderer.loadPaletteImage("examples/vm_threads/assets/palettes/font_pick.png",
                                                        ReadOrder::LeftRightThenDown, 0,
                                                        AssetPolicy::Embed);
    const PaletteId palDim  = renderer.loadPaletteImage("examples/vm_threads/assets/palettes/mono.png",
                                                        ReadOrder::LeftRightThenDown, 0,
                                                        AssetPolicy::Embed);

    // A glyph is 16px and the grid is 8px tiles, so each character stamps a 2×2 block.
    constexpr int        kMonW = kCols * 2, kMonH = kRows * 2;
    std::vector<TileCell> mon(static_cast<std::size_t>(kMonW) * kMonH,
                              TileCell{.atlas = font.atlasId, .tile = 0, .palette = palDim});
    const auto clearMon = [&] {
        for (TileCell& c : mon) {
            c.tile    = static_cast<std::uint16_t>(font[36].tile);
            c.palette = palDim;
        }
    };
    const auto write = [&](int col, int row, std::string_view text, PaletteId pal) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            const int gc = col + static_cast<int>(i);
            if (gc < 0 || gc >= kCols || row < 0 || row >= kRows) continue;
            const auto base = static_cast<std::uint16_t>(font[glyphCell(text[i])].tile);
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    TileCell& c = mon[static_cast<std::size_t>(row * 2 + dy) * kMonW + (gc * 2 + dx)];
                    c.tile    = static_cast<std::uint16_t>(base + dx + dy * kGlyphStride);
                    c.palette = pal;
                }
        }
    };

    // ── The machines ─────────────────────────────────────────────────────────────────────────────
    // One system carries them all. Registration mints an id per machine up front; host() places the
    // images and runs the driver's .init, and the handle it returns is how the machine is driven.
    AudioSystem::GBC                     sys{AudioKind::Chiptune};
    std::vector<DriverId<ToneSlots>>     ids;   // one registration per machine, minted on first use
    std::vector<HostedDriver<ToneSlots>> live;

    const auto addMachine = [&] {
        const std::size_t which = live.size();
        if (which >= static_cast<std::size_t>(kMaxMachines)) return;
        if (which >= ids.size()) ids.push_back(registerToneDriver());
        live.push_back(sys.host(ids[which]));
        live.back().play(pitchFor(which));
    };
    const auto removeMachine = [&] {
        if (live.empty()) return;
        live.back().close();
        live.pop_back();
    };

    // Keep the summed mix inside range. Every machine drives its wave channel at full scale and the
    // mixer sums voices saturating, so a few dozen machines at unity would clip — and clipping sounds
    // like breakup without being the breakup this demo measures. Scaling the VMDriver bus by the machine
    // count keeps the sum in range, which leaves the ring going empty as the one thing left to hear.
    //
    // Two things decide the level, and getting either wrong fades the demo out as machines are added,
    // which is the opposite of what it exists to show. A level is a slider POSITION, not a divisor —
    // the bus applies it through a perceptual curve, so the level to pick is the one whose GAIN is the
    // one wanted. And the gain wanted is 1/sqrt(n), not 1/n: the machines play different pitches, so
    // they are independent sources and their sum grows with the square root of their number, not with
    // their number. Measured at unity on the shipped mix: one machine peaks around 2600 of full scale
    // and six peak around 6900, which is the square-root law and leaves ample headroom.
    std::uint8_t busLevel = 255;
    const auto   balanceBus = [&] {
        const std::size_t n = live.empty() ? 1 : live.size();
        const auto        wanted =
            static_cast<std::uint32_t>(65536.0 / std::sqrt(static_cast<double>(n)));
        busLevel                   = 1;
        for (int level = 255; level > 1; --level) {
            if (perceptualGain(static_cast<std::uint8_t>(level)) <= wanted) {
                busLevel = static_cast<std::uint8_t>(level);
                break;
            }
        }
        AudioMixer::instance().levels(AudioLevels{.vmDriver = busLevel});
    };

    addMachine();  // open with one machine sounding, so the baseline is audible immediately
    balanceBus();

    std::uint64_t underflowAtSample = 0, underflowPerSecond = 0;
    int           sampleCountdown   = kSampleTicks;

    // Each machine's own count at the last sample, so the panel can show what the machines did over the
    // window rather than since they were hosted — a machine that stumbled once an hour ago reads the
    // same either way, and only one of those two answers says whether it is coping now.
    std::vector<std::size_t> laneAtSample;
    std::size_t laneStarvedPerSecond = 0, worstMachinePerSecond = 0, machinesBehind = 0;

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Fullscreen))
            platform.window().fullscreen(!platform.window().fullscreen());
        const std::size_t before = live.size();
        if (in.justPressed(Action::Add))    addMachine();
        if (in.justPressed(Action::Remove)) removeMachine();
        if (in.justPressed(Action::AddMany))
            for (int i = 0; i < kCoarseStep; ++i) addMachine();
        if (in.justPressed(Action::RemoveMany))
            for (int i = 0; i < kCoarseStep; ++i) removeMachine();
        if (live.size() != before) balanceBus();
        if (in.justPressed(Action::Retrigger))
            for (std::size_t i = 0; i < live.size(); ++i) live[i].play(pitchFor(i));

        if (--sampleCountdown <= 0) {
            const std::uint64_t now = sys.underflowFrames();
            underflowPerSecond = now - underflowAtSample;
            underflowAtSample  = now;

            // A machine keeps its index while it lives and the count only grows, so what a machine did
            // this window is the growth in its own figure. A machine hosted since the last sample, and
            // one whose index a fresh machine has taken over, both read as having done nothing yet.
            laneStarvedPerSecond = worstMachinePerSecond = machinesBehind = 0;
            std::vector<std::size_t> lanes(live.size());
            for (std::size_t i = 0; i < live.size(); ++i) {
                lanes[i]                 = live[i].underflowFrames();
                const std::size_t before = i < laneAtSample.size() ? laneAtSample[i] : lanes[i];
                const std::size_t grew   = lanes[i] > before ? lanes[i] - before : 0;
                laneStarvedPerSecond += grew;
                worstMachinePerSecond = std::max(worstMachinePerSecond, grew);
                if (grew > 0) ++machinesBehind;
            }
            laneAtSample    = std::move(lanes);
            sampleCountdown = kSampleTicks;
        }
    });

    FrameDrawState frame;
    const auto buildFrame = [&]() {
        clearMon();
        write(2, 1, "MACHINES ON THREADS", palText);

        write(2, 3, "MACHINES", palDim);
        write(18, 3, pad(live.size(), 6), palLive);
        write(2, 4, "RING FRAMES", palDim);
        write(18, 4, pad(sys.framesBuffered(), 6), palText);
        write(2, 5, "UNDERFLOW", palDim);
        write(18, 5, pad(sys.underflowFrames(), 6), palText);
        write(2, 6, "PER SECOND", palDim);
        write(18, 6, pad(underflowPerSecond, 6),
              underflowPerSecond > 0 ? palLive : palText);
        write(2, 7, "DROPPED", palDim);
        write(18, 7, pad(sys.framesDropped(), 6), palText);
        write(2, 8, "BUS", palDim);
        write(18, 8, pad(busLevel, 6), palText);
        write(2, 9, "LANE STARVED", palDim);
        write(18, 9, pad(laneStarvedPerSecond, 6),
              laneStarvedPerSecond > 0 ? palLive : palText);
        write(2, 10, "WORST MACHINE", palDim);
        write(18, 10, pad(worstMachinePerSecond, 6), palText);
        write(2, 11, "MACHINES BEHIND", palDim);
        write(18, 11, pad(machinesBehind, 6), machinesBehind > 0 ? palLive : palText);

        const std::string coarse = std::to_string(kCoarseStep);
        write(2, 13, "EACH MACHINE RUNS ITS OWN DRIVER AND", palDim);
        write(2, 14, "PRODUCES ITS OWN FRAME EVERY TICK", palDim);
        write(2, 15, "RIGHT ADD 1        UP ADD " + coarse, palText);
        write(2, 16, "LEFT REMOVE 1      DOWN REMOVE " + coarse, palText);
        write(2, 17, "SPACE RETRIGGER    F FULLSCREEN", palText);

        frame.layers.clear();
        DrawLayer panel{.key = "panel"};
        panel.z       = 0;
        panel.size    = PixelSize{kViewW, kViewH};
        panel.content = TileContent{.widthInTiles  = kMonW,
                                    .heightInTiles = kMonH,
                                    .cells         = std::span<const TileCell>(mon),
                                    .wrap          = TileWrap::Blank};
        frame.layers.push_back(panel);
    };

    loop.renderLoop([&]() {
        buildFrame();
        renderer.renderFrame(frame);
    });

    std::printf(
        "machines and threads — every hosted sound driver is a whole machine, stepped once per frame to\n"
        "produce that frame's samples. RIGHT and LEFT add and remove one machine, UP and DOWN move in\n"
        "coarse steps, SPACE re-triggers every tone, F fullscreen.\n\n"
        "Watch PER SECOND: the frames the device asked for that the ring could not supply over the last\n"
        "second. Add machines until it starts counting and the tone breaks up — that is where producing\n"
        "every machine's frame costs more than the frame is worth.\n\n");

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
