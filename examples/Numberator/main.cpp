// Numberator — a working calculator, styled after the classic Mac OS look, built on the engine to show
// it rendering a non-game UI from the same primitives a game uses. The chrome (window, title bar, sunken
// display well) is a TILEMAP assembled from a map PNG + a catalog; the keys are SPRITES that flip X+Y to
// look pressed; the digits and labels are transparent glyph sprites. Every asset is a PNG baked into the
// binary — no pixel bytes in the source.
//
// Click the keys with the mouse: digits and . build a number, the right column operates, = evaluates,
// C clears, ± negates, % takes a percent, ⌫ deletes a digit. A held key shows sunken until released.
// This is one of the runnable example hosts; CI compiles and links it on every platform without opening
// the window.

// Take ownership of main(): SDL's header would otherwise redirect main -> SDL_main.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <exception>

#include "retropp/analog_input.h"   // MouseButton
#include "retropp/clock.h"          // SteadyClock
#include "retropp/draw_state.h"     // FrameDrawState
#include "retropp/engine_config.h"  // EngineConfig
#include "retropp/input.h"          // InputState
#include "retropp/renderer.h"       // Renderer
#include "retropp/run_loop.h"       // RunLoop
#include "retropp/sdl_platform.h"   // SdlPlatform
#include "retropp/viewport.h"       // ViewportResolution
#include "retropp/windowed_host.h"  // WindowedHost

#include "assets.h"
#include "calculator.h"
#include "layout.h"
#include "render.h"

int main() {
    SDL_SetMainReady();
    using namespace retropp;
    using namespace numberator;

    // A custom viewport, sized to the calculator window, presented at a 2x integer zoom.
    const EngineConfig config{.window       = {.title = "Numberator"},
                              .viewport     = ViewportResolution{kViewW, kViewH},
                              .enhancements = {.windowScale = 2}};
    EngineConfig::setActive(config);  // the bare Renderer below inherits this viewport

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};

    Assets assets;
    try {
        assets = loadAssets(renderer);
    } catch (const std::exception& e) {
        std::printf("Numberator: could not load an asset: %s\n", e.what());
        return 1;
    }

    Calculator calc;
    int        pressedKey = -1;  // the key currently under a held mouse button (for the sunken look)

    loop.setTick([&](const InputState& in) {
        if (in.mouseJustPressed(MouseButton::Left)) {
            const int k = keyAt(in.cursor());
            if (k >= 0) {
                const Key& key = kKeys[static_cast<std::size_t>(k)];
                calc.press(key.action, key.data);
            }
        }
        pressedKey = in.mouseHeld(MouseButton::Left) ? keyAt(in.cursor()) : -1;
    });

    View           view;
    FrameDrawState frame;
    loop.setRender([&]() {
        view.build(frame, assets, calc.display(), pressedKey);
        renderer.renderFrame(frame);
    });

    std::printf("Numberator — click the keys to calculate. Close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
