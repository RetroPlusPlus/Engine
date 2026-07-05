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

// Take ownership of main(): SDL's header would otherwise #define main → SDL_main and expect
// SDL's entry shim. We init SDL ourselves (inside SdlPlatform), so we opt out of that redirect.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_keyboard.h>  // SDL_GetKeyboardState — the raw S-key sail poll (see below)
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <exception>

#include "retropp/clock.h"          // SteadyClock
#include "retropp/engine_config.h"  // EngineConfig
#include "retropp/input.h"          // InputState
#include "retropp/input_map.h"      // ControlBindings, InputProfile — the WASD sail aliases
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
    SDL_SetMainReady();

    // Startup configuration — a raw 640×480 viewport at 60 Hz. Engine interpolation stays at
    // its default (ON): every sprite carries a stable key, and teleports (respawns, abductor
    // visits, enemy re-entries) re-key, so the engine eases real motion and mount-snaps jumps.
    const EngineConfig config{
        .window   = {.title = "Retro++ — Ferryman (640×480, 60 Hz)"},
        .viewport = ViewportResolution{640, 480},
        .timing   = TimingProfile{TickPeriodNs::Hz60},
        // The SNES profile exposes the X/Y/L/R logical buttons the WASD keys ride (see the
        // bindings note below) — the Game Boy default would mask them off before the sim saw
        // them.
        .inputProfile = InputProfile::Snes,
    };
    EngineConfig::setActive(config);

    // Core engine objects.
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};

    // WASD sailing, ALONGSIDE the arrows. ControlBindings is one key per logical button, so
    // WASD can't be a SECOND binding for the d-pad — instead the sim reads the logical buttons
    // the default keyboard map already lands those keys on (W→R, A→Y) as aliases (see
    // game.cpp's moveInput). D is unbound by default, so one rebind puts it on the
    // otherwise-unused L slot; S's default slot (X) is rebound to numpad Enter below, so the
    // host polls S raw each tick instead. (setBindings marks the bindings customized, which
    // suppresses the per-family pad face-button auto-swap — fine for a game whose pad surface
    // is d-pad + stick + B.)
    ControlBindings bindings = ControlBindings::defaults();
    bindings.bindKey(Button::L, SDL_SCANCODE_D);
    // Numpad Enter rides the X slot: it starts the game at the title and drops cargo in play
    // (game.cpp reads X beside B). This rebind deliberately lifts X off its default S key, so
    // sailing with S never drops a passenger.
    bindings.bindKey(Button::X, SDL_SCANCODE_KP_ENTER);
    platform.setBindings(bindings);

    // Load + slice the committed indexed PNGs and the 32 palette images (all Embed), and build
    // the shared clips.
    ferryman::FerrymanAssets assets;
    try {
        assets = ferryman::loadFerrymanAssets(renderer);
    } catch (const std::exception& e) {
        std::printf("ferryman: could not load assets: %s\n", e.what());
        return 1;
    }

    ferryman::FerrymanGame     game;
    ferryman::FerrymanRenderer ferryRenderer;
    ferryman::FerrymanAudio    audio;        // after the platform: the sinks need SDL audio
    ferryman::FerrymanFeel     feel{assets}; // popups / shake / card + the animation cursors

    // S sails down. All 12 logical button slots are assigned (and a binding is one key per
    // button), so this one extra key is polled RAW from SDL each tick and fed to the sim as a
    // held flag — the established demo-side extension beside the logical surface.
    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::Select)) platform.setFullscreen(!platform.isFullscreen());
        const bool* keys = SDL_GetKeyboardState(nullptr);
        game.rawDownHeld = keys != nullptr && keys[SDL_SCANCODE_S];
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
    loop.setRender([&] { ferryRenderer.render(renderer, game, assets, feel); });

    std::printf("Ferryman (640×480, 60 Hz) — ENTER to set sail; arrows / WASD / the stick sail. "
                "DOCK against an islet to take its souls aboard (no button), carry them to the "
                "sanctuary coast to bank; every soul aboard slows you, pays more, and fires back "
                "at the fleet. START pauses, SELECT toggles fullscreen. Close the window to "
                "quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
