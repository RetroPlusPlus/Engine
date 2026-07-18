// ============================================================================================
//  Bongusoid — an Arkanoid-style brick-breaker on Retro++, and the engine's broad reference example.
//
//  This is the ENTRY POINT only. As of S2 the example is MULTI-FILE — one translation unit per concern,
//  so each stays small and the audio/feel work lands on clean files:
//    • layout.h          — shared constants + the small value types (no engine deps).
//    • assets.{h,cpp}    — the loader: slice the two committed indexed PNGs, upload the palettes.
//    • game.{h,cpp}      — the simulation: board / ball / paddle, emitting a GameEvent stream each tick.
//    • audio.{h,cpp}     — (S2) chiptune SFX registered on the AudioLibrary, cued per GameEvent.
//    • feel.{h,cpp}      — (S2) tweened score popups + paddle squash, ball spin, gentle screen shake.
//    • render.{h,cpp}    — the draw step: text/HUD tile layer + the sprite layer.
//  main() below wires them together and runs the loop; it holds no game logic itself.
//
//  "Bongusoid" is an ORIGINAL homage — original art, layouts, and SFX; no Taito assets or names.
//
//  ASSET POLICY (S2): EVERY asset/routine is Embed, stated explicitly at its own call site — Bongusoid
//  sets and relies on no global default. So build/bongusoid_demo/ is the binary alone; nothing rides along.
//
//  PHOTOSENSITIVITY: motion is smooth and moderate; nothing flashes or strobes (S2's screen shake is gentle
//  and brief). The demo never auto-launches a window; you run it yourself.
//
//  QUIT: close the window. (A game-facing quit API — RunLoop::requestStop() — is deferred ENG work; see
//  ENGINE_DISCUSSION_ISSUES.md §K. Bongusoid-S4's pause-menu "Quit" is its first real consumer.)
//
//  CI: like the other example hosts it instantiates SdlPlatform + Renderer + AudioSystem for real, so the
//  live GPU + image-load + analog-input + audio path keeps compiling/linking on every CI platform.
// ============================================================================================

// Take ownership of main(): SDL's header would otherwise #define main → SDL_main and expect SDL's
// entry shim. We init SDL ourselves (inside SdlPlatform), so we opt out of that redirect.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <exception>

#include "retropp/clock.h"           // SteadyClock
#include "retropp/engine_config.h"   // EngineConfig
#include "retropp/input.h"           // InputState
#include "retropp/input_actions.h"   // ActionMap, PadButton — the binding surface
#include "retropp/renderer.h"        // Renderer
#include "retropp/run_loop.h"        // RunLoop
#include "retropp/sdl_platform.h"    // SdlPlatform
#include "retropp/timing.h"          // TimingProfile, TickPeriodNs (Hz60)
#include "retropp/viewport.h"        // ViewportResolution
#include "retropp/windowed_host.h"   // WindowedHost

#include "assets.h"
#include "audio.h"
#include "feel.h"
#include "game.h"
#include "render.h"

int main() {
    using namespace retropp;
    SDL_SetMainReady();

    // Startup configuration — a raw 640×480 viewport at 60 Hz.
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Bongusoid"},
        .window   = {.title = "Retro++ — Bongusoid (640×480, 60 Hz)"},
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
        {bong::Action::MoveLeft,   {SDL_SCANCODE_LEFT, SDL_SCANCODE_A, PadButton::DpadLeft}},
        {bong::Action::MoveRight,  {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, PadButton::DpadRight}},
        {bong::Action::Serve,      {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {bong::Action::Start,      {SDL_SCANCODE_RETURN, PadButton::Start}},
        {bong::Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(actions);

    // Load + slice the committed indexed PNGs (both Embed) and upload the palettes.
    bong::BongAssets assets;
    try {
        assets = bong::loadBongAssets(renderer);
    } catch (const std::exception& e) {
        std::printf("bongusoid: could not load assets: %s\n", e.what());
        return 1;
    }

    bong::BongGame     game;
    bong::BongRenderer bongRenderer;
    bong::BongAudio    audio;  // registers the SFX + owns the AudioSystems (after the platform: needs SDL audio)
    bong::BongFeel     feel;   // tweened popups / paddle squash / ball spin / screen shake

    // The mouse drives the paddle during play, so the OS arrow would just hover distractingly over the
    // field — hide it while Playing, show it again on the Title screen. Uses the new
    // Platform::cursorVisible, which is independent of pointer capture: Bongusoid wants the ABSOLUTE
    // cursor (the paddle tracks it), just no visible arrow — so it suppresses the cursor without ever
    // entering relative/capture mode. Tracked so SDL is poked only on a state change, not every tick.
    bool cursorHidden = false;
    loop.simTick([&](const InputState& in) {
        if (in.justPressed(bong::Action::Fullscreen))
            platform.fullscreen(!platform.fullscreen());
        game.tick(in);
        for (const bong::GameEvent& e : game.events()) {
            audio.onEvent(e.kind);  // voice each event
            feel.onEvent(e);        // and react with the game-feel layer
        }
        feel.update(game);          // accumulate ball spin from the current english
        feel.tick();                // advance tween cursors, reap finished popups
        // Audio needs no per-tick step — each AudioSystem produces on its own thread (ENG-4.D.1); the
        // game just cues via audio.onEvent() above.

        const bool wantHidden = (game.state == bong::GameState::Playing);
        if (wantHidden != cursorHidden) {
            platform.cursorVisible(!wantHidden);
            cursorHidden = wantHidden;
        }
    });
    loop.renderLoop([&] { bongRenderer.render(renderer, game, assets, feel); });

    std::printf("Bongusoid (640×480, 60 Hz) — ENTER to start; Left/Right or the mouse move the paddle, "
                "A serves; SELECT toggles fullscreen. Silver takes two hits, gold never breaks. "
                "Close the window to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
