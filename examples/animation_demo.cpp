// Animation demo (ENG-2.H) — one DISTINCT animation per playback type, each driven by its own button,
// so you can watch each policy behave and read exactly how it is wired.
//
// Four animations, four actions. Each press play/pauses/restarts the animation it owns:
//   • X (pad A) → a LOOPING walk cycle   (PlaybackMode::loopIndefinitely) — runs forever; toggles play/pause.
//   • Z (pad B) → a ONE-OFF cycle        (PlaybackMode::single)           — plays once and holds the last frame.
//   • Up        → an N-LOOP cycle        (PlaybackMode::loopNTimes(3))    — three passes, then holds the last frame.
//   • Down      → a PALETTE-CYCLE for 2s (PlaybackMode::playForDuration)  — colour pulses for two seconds, then stops.
//
// One press = one state step on that animation: a fresh or finished one (re)STARTS from frame 0,
// a playing one PAUSES, a paused one RESUMES. So you press X and the loop starts; press it again to
// pause; press Z and the one-off plays once; press Up for the three-pass run; press Down for the timed
// colour pulse. The console logs each transition so you can correlate the press with the policy.
//
// HOW TO IMPLEMENT (the whole pattern, top to bottom):
//   1. loadAtlas → an AtlasManifest of carved frame slots.
//   2. Build an Animation: each frame = { label, atlas, slot, palette, duration }. The duration is a
//      std::chrono value at the call site (250ms); the engine resolves it to ticks against the profile.
//   3. Hold a game-owned AnimationPlayer per animation. Each game tick call player.advance(mode); the
//      engine keeps NO playback state — the player (and its elapsed-tick clock) lives here in the game.
//   4. Each frame, thread player.current() into draw state: the frame's atlas + its palette into the
//      layer, the frame's slot into the sprite. Palette-cycling is the SAME loop with the slot held
//      constant and only the palette varying — palette is just a per-frame field.
//
// Photosensitivity (locked): slow cycles (≥200ms/frame), gentle colour steps, no flashing, manual
// stepping only — the demo never auto-launches and never strobes. Dev-run only (CI has no display);
// the resolver math is proven headlessly in tests/animation_test.cpp.

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include "retropp/animation.h"
#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/image.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

using namespace retropp;
using namespace std::chrono_literals;

namespace {

// The demo's vocabulary: one action per animation — each press steps the animation it owns.
enum class Action : std::uint8_t { StepLoop, StepOnce, StepTriple, StepCycle };

// One on-screen animation: its player, the policy it plays under, a label, and where it sits.
struct Slot {
    AnimationPlayer player;
    PlaybackMode    mode;
    const char*     name;
    int             x;
    int             y;
    bool            wasFinished = false;  // to log the finish transition once
};

// A button press steps this animation: fresh/finished → restart; playing → pause; paused → resume.
void onPress(Slot& s) {
    if (s.player.finished()) {
        s.player.restart();
        std::printf("[%s] restart\n", s.name);
    } else if (s.player.playing) {
        s.player.pause();
        std::printf("[%s] pause\n", s.name);
    } else {
        s.player.play();
        std::printf("[%s] play\n", s.name);
    }
}

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Animation Demo"},
        .window = {.title = "Retro++ — animations"}};
    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};

    // One action per animation: X / pad A steps the loop, Z / pad B the one-off, Up (or W) the
    // three-pass run, Down (or S) the palette cycle.
    ActionMap map{
        {Action::StepLoop,   {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::StepOnce,   {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::StepTriple, {SDL_SCANCODE_UP, SDL_SCANCODE_W, PadButton::DpadUp}},
        {Action::StepCycle,  {SDL_SCANCODE_DOWN, SDL_SCANCODE_S, PadButton::DpadDown}},
    };
    platform.actions(map);

    // Animation cadence needs no separate line: EngineConfig::setActive(config) above already fanned
    // config.timing into AnimationPlayer::defaultTiming, so every bare AnimationPlayer below inherits
    // the engine cadence with no per-player profile to pass. (Default config = GBC cadence here.)

    // ── Load the numbered sheet (cells 0..5; every cell uses the SAME two indices — index 0 bg, index
    // 1 the digit — so they differ only by SHAPE) and carve it into addressable frame slots ───────────
    AtlasManifest sheet;
    try {
        sheet = renderer.loadAtlas("assets/anim_numbers.png",
                                   AssetDimensions::GameBoy8x8, ContentKind::SpriteSeries,
                                   ReadOrder::LeftRightThenDown, /*count=*/0, TransparentIndices::GameBoy);
    } catch (const std::exception& e) {
        std::printf("animation demo: could not load the sheet: %s\n", e.what());
        return 1;
    }

    // Two-entry palettes (index 0 background, index 1 digit). The fixed palette colours every numbered
    // cell identically — so the frame animations below change only the NUMBER, never the colour. The
    // three hue variants recolour only the DIGIT (background held), so the palette-cycle animation
    // changes only the COLOUR of a constant number. That contrast is the whole point of the demo.
    auto upload = [&](std::array<Rgba8, 2> c) { return renderer.uploadPalette(std::span<const Rgba8>(c)); };
    const PaletteId palFixed = upload({{{28, 28, 40}, {238, 238, 242}}});  // dark bg, near-white digit
    const PaletteId palWarm  = upload({{{28, 28, 40}, {240, 150, 90}}});   // digit → warm orange
    const PaletteId palCool  = upload({{{28, 28, 40}, {110, 180, 235}}});  // digit → cool blue
    const PaletteId palDusk  = upload({{{28, 28, 40}, {205, 120, 205}}});  // digit → violet

    // ── Build the four animations from the carved slots ───────────────────────────────────────────
    // Each frame is a plain { label, atlas, slot, palette, duration } literal. The art reference is
    // .atlas = sheet.atlas + .slot = sheet[tile] (the AtlasManifest's carved cell); a frame could just
    // as well point at a different atlas or a one-off image. The three frame animations (A/B/Up) all
    // use palFixed, so their numbers change but their colour does NOT; only the palette-cycle (Down)
    // varies .palette — proving frame animation and palette animation are the same unit, different field.

    // StepLoop: a looping cycle over the first three cells.
    const Animation loopAnim{{
        {.label = "l0", .atlas = sheet.atlas, .slot = sheet[0], .palette = palFixed, .duration = 250ms},
        {.label = "l1", .atlas = sheet.atlas, .slot = sheet[1], .palette = palFixed, .duration = 250ms},
        {.label = "l2", .atlas = sheet.atlas, .slot = sheet[2], .palette = palFixed, .duration = 250ms},
    }};
    // StepOnce: a one-off march through all six cells (holds cell 5 when done).
    const Animation onceAnim{{
        {.label = "o0", .atlas = sheet.atlas, .slot = sheet[0], .palette = palFixed, .duration = 200ms},
        {.label = "o1", .atlas = sheet.atlas, .slot = sheet[1], .palette = palFixed, .duration = 200ms},
        {.label = "o2", .atlas = sheet.atlas, .slot = sheet[2], .palette = palFixed, .duration = 200ms},
        {.label = "o3", .atlas = sheet.atlas, .slot = sheet[3], .palette = palFixed, .duration = 200ms},
        {.label = "o4", .atlas = sheet.atlas, .slot = sheet[4], .palette = palFixed, .duration = 200ms},
        {.label = "o5", .atlas = sheet.atlas, .slot = sheet[5], .palette = palFixed, .duration = 200ms},
    }};
    // StepTriple: three cells, looped three times then held.
    const Animation triAnim{{
        {.label = "t3", .atlas = sheet.atlas, .slot = sheet[3], .palette = palFixed, .duration = 250ms},
        {.label = "t4", .atlas = sheet.atlas, .slot = sheet[4], .palette = palFixed, .duration = 250ms},
        {.label = "t5", .atlas = sheet.atlas, .slot = sheet[5], .palette = palFixed, .duration = 250ms},
    }};
    // StepCycle: palette-cycling animation — ONE cell held (.slot is the same every frame), only .palette
    // varies, so colour animation reuses the exact same mechanism. Played for 2 seconds, then stops.
    const Animation cycleAnim{{
        {.label = "c0", .atlas = sheet.atlas, .slot = sheet[0], .palette = palFixed, .duration = 300ms},
        {.label = "c1", .atlas = sheet.atlas, .slot = sheet[0], .palette = palWarm, .duration = 300ms},
        {.label = "c2", .atlas = sheet.atlas, .slot = sheet[0], .palette = palCool, .duration = 300ms},
        {.label = "c3", .atlas = sheet.atlas, .slot = sheet[0], .palette = palDusk, .duration = 300ms},
    }};

    // The four game-owned players, each starting PAUSED on frame 0 (nothing moves until you press its
    // button). The TimingProfile defaults to GameBoyColor, matching the run loop's cadence.
    std::array<Slot, 4> slots{{
        {AnimationPlayer{.animation = &loopAnim, .playing = false},  PlaybackMode::loopIndefinitely(),
         "loop",     40,  34},
        {AnimationPlayer{.animation = &onceAnim, .playing = false},  PlaybackMode::single(),
         "once",     112, 34},
        {AnimationPlayer{.animation = &triAnim,  .playing = false},  PlaybackMode::loopNTimes(3),
         "3-loops",  40,  100},
        {AnimationPlayer{.animation = &cycleAnim, .playing = false}, PlaybackMode::playForDuration(2s),
         "2s cycle", 112, 100},
    }};

    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Action::StepLoop))   onPress(slots[0]);
        if (in.justPressed(Action::StepOnce))   onPress(slots[1]);
        if (in.justPressed(Action::StepTriple)) onPress(slots[2]);
        if (in.justPressed(Action::StepCycle))  onPress(slots[3]);

        // Advance every player one tick. advance() accrues elapsed ONLY while playing, so paused
        // animations stay put — the game owns the clock; the engine tracks nothing.
        for (Slot& s : slots) {
            s.player.advance(s.mode);
            if (s.player.finished() && !s.wasFinished) {
                std::printf("[%s] finished (holding the last frame)\n", s.name);
            }
            s.wasFinished = s.player.finished();
        }
    });

    // Each animation draws on its own layer: the current frame's sprite names the frame's atlas and
    // palette directly (palette-cycling is just the palette field varying per frame). Scaled 3× via a
    // per-sprite transform so the 8×8 cells are easy to see; the transform is incidental to the
    // animation (identity would work too).
    std::array<Sprite, 4> sprites{{{.key = "a0"}, {.key = "a1"}, {.key = "a2"}, {.key = "a3"}}};
    FrameDrawState fds;

    loop.setRender([&]() {
        fds.layers.clear();
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const AnimationFrame& f = slots[i].player.current();

            Sprite& sp = sprites[i];
            sp.x         = slots[i].x;
            sp.y         = slots[i].y;
            sp.size      = f.slot.dimensions;
            sp.tile      = f.slot.tile;
            sp.atlas     = f.atlas;     // the frame's sheet
            sp.palette   = f.palette;   // the frame's palette (palette-cycling lives here)
            sp.transform = Transform::scale(3.0F, 3.0F);  // enlarge the 8×8 cell for visibility

            DrawLayer layer{.key = slots[i].name};
            layer.z       = static_cast<int>(10 + i);
            layer.size    = PixelSize{160, 144};
            layer.content = SpriteContent{.sprites = std::span<const Sprite>(&sprites[i], 1)};
            fds.layers.push_back(std::move(layer));
        }
        renderer.renderFrame(fds);
    });

    std::printf(
        "animations — four playback types, one key each:\n"
        "  X (pad A) = looping (forever; toggles play/pause)\n"
        "  Z (pad B) = one-off (plays once, holds the last frame)\n"
        "  Up        = 3 loops then holds\n"
        "  Down      = palette-cycle for 2 seconds then stops\n"
        "Each press: fresh/finished -> restart, playing -> pause, paused -> resume. Close to quit.\n");

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
