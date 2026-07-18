// ============================================================================================
//  Polterball — a hybrid arcade game on Retro++: Breakout's paddle-and-ball, Pac-Man's pellet
//  maze and pursuit, and a pinch of wall-carving demolition, combined into one loop:
//
//    THE BALL IS THE PELLET-EATER, AND THE GHOSTS HUNT THE BALL — NOT YOU.
//
//  You keep the ball alive from a paddle below the maze and steer it only indirectly, through
//  english. It eats every pellet it ricochets across; three ghosts (a chaser, an ambusher, a
//  wanderer) prowl the corridors after it. A ghost that touches the ball swallows it — the same
//  cost as dropping it past the paddle. Power pellets ignite the ball for a few seconds: the
//  ghosts turn and flee, contact smashes THEM (chain-scored bank shots), and their eyes fly home
//  to respawn. Some walls are soft — the ball carves them permanently, reshaping both its own
//  bounce corridors and the ghosts' routes. Eat every pellet to clear the board; the next one is
//  faster. An original homage to the golden-age arcade — original art, layouts, names, and SFX.
//
//  This is the ENTRY POINT only. The example is MULTI-FILE — one translation unit per concern:
//    • layout.h          — shared constants, the two maze boards, the runtime Board (no engine deps).
//    • assets.{h,cpp}    — slice the three committed indexed PNGs, upload the palettes + clips.
//    • game.{h,cpp}      — the simulation: ball / paddle / pellets / walls, a GameEvent stream.
//    • ghosts.{h,cpp}    — the pursuit AI: junction movement, scatter/chase, frightened, eyes.
//    • feel.{h,cpp}      — tweened popups + paddle squash + gentle shake + the animation cursors.
//    • render.{h,cpp}    — the draw step: the mixed-sheet backdrop layer + the sprite layers.
//  main() below wires them together and runs the loop; it holds no game logic itself.
//
//  ASSET POLICY: EVERY asset/routine is Embed, stated explicitly at its own call site — the built
//  binary is self-contained; build/polterball_demo/ holds the executable alone.
//
//  PHOTOSENSITIVITY: motion is smooth and moderate; nothing flashes or strobes (the shake is
//  gentle and brief, the pellet pulse is slow, the frightened swap is a steady colour change).
//  The demo never auto-launches a window; you run it yourself.
//
//  QUIT: close the window.
//
//  CI: like the other example hosts it instantiates SdlPlatform + Renderer + AudioSystem for
//  real, so the live GPU + image-load + analog-input + audio path keeps compiling/linking on
//  every CI platform — but CI never opens the window.
// ============================================================================================

// Take ownership of main(): SDL's header would otherwise #define main → SDL_main and expect SDL's
// entry shim. We init SDL ourselves (inside SdlPlatform), so we opt out of that redirect.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

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
    SDL_SetMainReady();

    // Startup configuration — a raw 640×480 viewport at 60 Hz. Engine interpolation stays at its
    // default (ON): every sprite carries a stable key, so the engine eases real motion and
    // mount-snaps the deliberate teleports (re-serves, board resets) automatically.
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Polterball"},
        .window   = {.title = "Retro++ — Polterball (640×480, 60 Hz)"},
        .viewport = ViewportResolution{640, 480},
        .timing   = TimingProfile{TickPeriodNs::Hz60}};
    EngineConfig::setActive(config);

    // Core engine objects.
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};

    // The paddle vocabulary, every source per action in one row: arrows AND A/D move, X or the
    // pad's south face serves, Return/Start starts, Backspace/Select toggles fullscreen. The stick
    // and the mouse need no rows — the sim reads them raw.
    const ActionMap actions{
        {polter::Action::MoveLeft,   {SDL_SCANCODE_LEFT, SDL_SCANCODE_A, PadButton::DpadLeft}},
        {polter::Action::MoveRight,  {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, PadButton::DpadRight}},
        {polter::Action::Serve,      {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {polter::Action::Start,      {SDL_SCANCODE_RETURN, PadButton::Start}},
        {polter::Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(actions);

    // Load + slice the committed indexed PNGs (all Embed) and upload the palettes + shared clips.
    polter::PolterAssets assets;
    try {
        assets = polter::loadPolterAssets(renderer);
    } catch (const std::exception& e) {
        std::printf("polterball: could not load assets: %s\n", e.what());
        return 1;
    }

    polter::PolterGame     game;
    polter::PolterRenderer polterRenderer;
    polter::PolterAudio    audio;        // after the platform: the auto-owned sinks need SDL audio
    polter::PolterFeel     feel{assets}; // popups / squash / shake + the pulse + skirt cursors

    // The mouse drives the paddle during play, so the OS arrow would just hover distractingly over
    // the maze — hide it while Playing, show it again on the Title screen. Absolute tracking stays
    // live (the paddle follows the cursor); only the arrow is suppressed, never captured. Tracked
    // so SDL is poked only on a state change, not every tick.
    bool cursorHidden = false;
    loop.simTick([&](const InputState& in) {
        if (in.justPressed(polter::Action::Fullscreen))
            platform.fullscreen(!platform.fullscreen());
        game.tick(in);
        for (const polter::GameEvent& e : game.events()) {
            audio.onEvent(e.kind);  // voice each event
            feel.onEvent(e);        // and react with the game-feel layer
        }
        feel.tick();                // advance every tween + animation cursor, reap finished popups
        // Audio needs no per-tick step — each AudioSystem produces on its own thread; the game
        // just cues via audio.onEvent() above.

        const bool wantHidden = (game.state == polter::GameState::Playing);
        if (wantHidden != cursorHidden) {
            platform.cursorVisible(!wantHidden);
            cursorHidden = wantHidden;
        }
    });
    loop.renderLoop([&] { polterRenderer.render(renderer, game, assets, feel); });

    std::printf("Polterball (640×480, 60 Hz) — ENTER to start; Left/Right or the mouse move the "
                "paddle, A or a click serves; SELECT toggles fullscreen. The ball eats the pellets; "
                "the ghosts hunt the ball; power pellets turn the tables. Close the window to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
