// ENG-2.B.1 manual runtime demo — the smallest real host that exercises the live
// platform + renderer path: open a window, render the internal viewport (a bring-up
// colour) blitted integer-scaled and letterboxed onto the swapchain at display refresh,
// and route keyboard + gamepad input through to the tick callback. Run it on a dev
// machine and confirm: the window shows a centred colour rect on black bars, resizing
// re-letterboxes it, the close button quits, and pressing a mapped button prints a line.
//
// This is also the only target that instantiates SdlPlatform + Renderer in a real run,
// so it keeps the live SDL_GPU pipeline/present path compiling and linking on every CI
// platform even though CI never opens the window.

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main and
// expect SDL's entry shim. We init SDL ourselves inside SdlPlatform.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdio>
#include <utility>

#include "gbcpp/clock.h"
#include "gbcpp/input.h"
#include "gbcpp/renderer.h"
#include "gbcpp/run_loop.h"
#include "gbcpp/sdl_platform.h"
#include "gbcpp/windowed_host.h"

int main() {
    SDL_SetMainReady();

    using namespace gbcpp;

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform{{.title = "GBCPP — ENG-2.B.1 viewport blit demo"}};
    Renderer    renderer{platform.device(), platform.window()};

    constexpr std::array<std::pair<Button, const char*>, kButtonCount> kLabels{{
        {Button::Up, "Up"}, {Button::Down, "Down"}, {Button::Left, "Left"},
        {Button::Right, "Right"}, {Button::A, "A"}, {Button::B, "B"},
        {Button::Start, "Start"}, {Button::Select, "Select"},
    }};

    auto familyName = [](ControllerType t) {
        switch (t) {
            case ControllerType::Xbox:        return "Xbox";
            case ControllerType::PlayStation: return "PlayStation";
            case ControllerType::Nintendo:    return "Nintendo";
            case ControllerType::Standard:    return "Standard";
            default:                          return "none";
        }
    };

    ControllerType lastType = ControllerType::Unknown;
    loop.setTick([&](const InputState& in) {
        if (platform.controllerType() != lastType) {  // auto-detected on connect
            lastType = platform.controllerType();
            std::printf("controller: %s\n", familyName(lastType));
        }
        for (const auto& [button, name] : kLabels) {
            if (in.justPressed(button))  std::printf("press   %s\n", name);
            if (in.justReleased(button)) std::printf("release %s\n", name);
        }
    });
    // The render callback drives the renderer once per advance() (ENG-1 contract).
    loop.setRender([&](float alpha) { renderer.renderFrame(alpha); });

    std::printf("ENG-2.B.1 viewport blit demo — close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
