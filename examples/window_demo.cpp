// ENG-2.A manual runtime demo — the smallest real host that exercises the live
// platform path: open a window, present a cleared frame at display refresh, and route
// keyboard + gamepad input through to the tick callback. Run it on a dev machine and
// confirm the window opens, stays responsive, closes on the window-close button, and
// prints a line when a mapped button is pressed/released.
//
// This is also the only target that instantiates SdlPlatform in a real run, so it
// keeps the live SDL_GPU present path compiling and linking on every CI platform even
// though CI never opens the window.

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main and
// expect SDL's entry shim. We init SDL ourselves inside SdlPlatform.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdio>
#include <utility>

#include "gbcpp/clock.h"
#include "gbcpp/input.h"
#include "gbcpp/run_loop.h"
#include "gbcpp/sdl_platform.h"
#include "gbcpp/windowed_host.h"

int main() {
    SDL_SetMainReady();

    using namespace gbcpp;

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform{{.title = "GBCPP — ENG-2.A window demo"}};

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
    // Decision #6: the platform owns the GPU present; the render callback drives it.
    loop.setRender([&](float /*alpha*/) { platform.presentClearFrame(); });

    std::printf("ENG-2.A window demo — close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
