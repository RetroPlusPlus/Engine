// ============================================================================================
//  Ferryman — a hybrid arcade game on Retro++: an open-sea rescue-and-carry (collect stranded
//  souls from the islets, sail them home to the sanctuary) under a TONED-DOWN BULLET HELL
//  (three enemy craft classes firing straight, readable bullets), with one twist:
//
//    THE CARGO IS THE DIFFICULTY — AND THE ARSENAL. EVERY SOUL ABOARD SLOWS YOU, PAYS MORE,
//    AND OCCASIONALLY FIRES BACK. THERE IS NO FIRE BUTTON: THE CREW FIGHTS, YOU SAIL.
//
//  Delivering n souls at once pays escalating (slot i pays i × 50), so the greed dial is how
//  heavy — how slow, how armed — you dare get. An abductor saucer steals whoever waits (body
//  block it or let your crew shoot it — both foil the theft), and a soul carried off the top
//  comes back as a mutant that hunts you. Everything the enemies fire flies a predictable
//  line: dodge by reading, not by luck. An original homage to the golden-age arcade — original
//  art, names, and SFX.
//
//  This is the ENTRY POINT only. The example is MULTI-FILE — one translation unit per concern:
//    • layout.h          — shared constants, tuning, the slot/palette enums (no engine deps).
//    • assets.{h,cpp}    — slice the three committed indexed PNGs; EVERY palette from a palette
//                          IMAGE (16×1 RGBA PNGs via loadPaletteImage); the shared clips.
//    • game.{h,cpp}      — the simulation: ferry / colonists / enemies / bolts, a GameEvent stream.
//    • abductor.{h,cpp}  — the rival thief: curve-swoop entries, hover → descend → carry.
//    • feel.{h,cpp}      — tweened popups + gentle shake + the round card + the animation cursors.
//    • render.{h,cpp}    — the draw step: two parallax sea planes, terrain, sprites, regions.
//  main() below wires them together and runs the loop; it holds no game logic itself.
//
//  ASSET POLICY: EVERY asset/routine is Embed, stated explicitly at its own call site — the
//  built binary is self-contained; build/ferryman_demo/ holds the executable alone.
//
//  The demo never auto-launches a window; you run it yourself.
//
//  QUIT: close the window.
//
//  CI: like the other example hosts it instantiates SdlPlatform + Renderer + AudioSystem for
//  real, so the live GPU + image-load + palette-image + audio path keeps compiling/linking on
//  every CI platform — but CI never opens the window.
// ============================================================================================

#include <cstdio>
#include <exception>

#include "retropp/clock.h"          // SteadyClock
#include "retropp/engine_config.h"  // EngineConfig
#include "retropp/input.h"          // InputState
#include "retropp/input_actions.h"  // ActionMap, PadButton — the binding surface
#include "retropp/renderer.h"       // Renderer
#include "retropp/run_loop.h"       // RunLoop
#include "retropp/sdl_platform.h"   // SdlPlatform
#include "retropp/timing.h"         // TimingProfile, TickPeriodNs (Hz60)
#include "retropp/viewport.h"       // ViewportResolution
#include "retropp/windowed_host.h"  // WindowedHost

#include "assets.h"
#include "audio.h"
#include "feel.h"
#include "game.h"
#include "render.h"

int main() {
    using namespace retropp;

    // Startup configuration — a raw 640×480 viewport at 60 Hz. Engine interpolation stays at
    // its default (ON): every sprite carries a stable key, and teleports (respawns, abductor
    // visits, enemy re-entries) re-key, so the engine eases real motion and mount-snaps jumps.
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Ferryman"},
        .window   = {.title = "Retro++ — Ferryman (1280×720, 60 Hz)"},
        .viewport = ViewportResolution{1280, 720},
        .timing   = TimingProfile{TickPeriodNs::Hz60}};
    EngineConfig::setActive(config);

    // Core engine objects.
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // The sail vocabulary, every source per action in one row: arrows AND WASD sail together,
    // the pad contributes the d-pad plus its north face button and both shoulders as extra sail
    // aliases, and the menu action rides Return / numpad Enter / Start / the west face button.
    // The left stick needs no row — the sim reads it raw (game.cpp's moveInput).
    const ActionMap actions{
        {ferryman::Action::SailUp,     {SDL_SCANCODE_UP, SDL_SCANCODE_W,
                                        PadButton::DpadUp, PadButton::ShoulderR}},
        {ferryman::Action::SailDown,   {SDL_SCANCODE_DOWN, SDL_SCANCODE_S, PadButton::DpadDown}},
        {ferryman::Action::SailLeft,   {SDL_SCANCODE_LEFT, SDL_SCANCODE_A,
                                        PadButton::DpadLeft, PadButton::FaceNorth}},
        {ferryman::Action::SailRight,  {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D,
                                        PadButton::DpadRight, PadButton::ShoulderL}},
        {ferryman::Action::Menu,       {SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER,
                                        PadButton::Start, PadButton::FaceWest}},
        {ferryman::Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(actions);

    // Load + slice the committed indexed PNGs and the 32 palette images (all Embed), and build
    // the shared clips.
    ferryman::FerrymanAssets assets;  // filled in place below — never moved/copied (clips hold sheet
                                      // pointers into its own manifests; a move would dangle them)
    try {
        ferryman::loadFerrymanAssets(renderer, assets);
    } catch (const std::exception& e) {
        std::printf("ferryman: could not load assets: %s\n", e.what());
        return 1;
    }

    ferryman::FerrymanGame     game;
    ferryman::FerrymanRenderer ferryRenderer;
    ferryman::FerrymanAudio    audio;        // after the platform: the sinks need SDL audio
    ferryman::FerrymanFeel     feel{assets}; // popups / shake / card + the animation cursors

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(ferryman::Action::Fullscreen))
            platform.window().fullscreen(!platform.window().fullscreen());
        game.tick(in);
        for (const ferryman::GameEvent& e : game.events()) {
            audio.onEvent(e.kind);  // voice each event
            feel.onEvent(e);        // and react with the game-feel layer
        }
        if (!game.paused) {         // the pause menu freezes EVERYTHING — sim, feel, and parallax
            feel.tick(game);        // advance every tween + animation cursor, watch the wave
            ferryRenderer.tickScroll(); // the two sea planes drift on the SIM tick (parallax)
        }
        // Audio needs no per-tick step — each AudioSystem produces on its own thread.
    });
    loop.renderLoop([&] { ferryRenderer.render(renderer, game, assets, feel); });

    std::printf("Ferryman (1280×720, 60 Hz) — ENTER to set sail; arrows / WASD / the stick sail. "
                "DOCK against an islet to take its souls aboard (no button), carry them to the "
                "sanctuary coast to bank; every soul aboard slows you, pays more, and fires back "
                "at the fleet. START pauses, SELECT toggles fullscreen. Close the window to "
                "quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
