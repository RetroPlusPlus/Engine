# Getting started

This page takes you from a fresh clone to a window with a scrolling, full-colour tile background you
can steer with the d-pad — the smallest complete program that uses the engine for real. It is about
60 lines, uses no asset files, and every line is explained below. The full source is
[`examples/hello_world.cpp`](../../examples/hello_world.cpp); it builds as the `gbcpp-hello-world`
target, so you can run exactly what you read here.

New to how the pieces fit together? Skim [concepts.md](concepts.md) first for the mental model — but
you can also just follow along here and pick it up as you go.

## 1. Build the engine

```sh
git clone --recurse-submodules <repo-url>
cd GBCPP-Engine
cmake -S . -B build
cmake --build build
```

`--recurse-submodules` matters — SDL3 and the other vendored dependencies come in as submodules. You
also need CMake 3.28+, a C++20 compiler (GCC 13+, Clang 16+, or MSVC 19.38+), and a build-time shader
toolchain. The full requirements list and what each dependency is for are in
[build-and-consume.md](build-and-consume.md). When the build finishes you have the engine library,
its tests, and two runnable examples — `gbcpp-hello-world` (this page) and `gbcpp-window-demo` (a
richer demo).

## 2. The whole program

This is the complete thing — copy it, or just read it and run the committed
[`examples/hello_world.cpp`](../../examples/hello_world.cpp).

```cpp
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "gbcpp/clock.h"
#include "gbcpp/draw_state.h"
#include "gbcpp/engine_config.h"
#include "gbcpp/input.h"
#include "gbcpp/palette.h"
#include "gbcpp/renderer.h"
#include "gbcpp/run_loop.h"
#include "gbcpp/sdl_platform.h"
#include "gbcpp/windowed_host.h"

using namespace gbcpp;

int main() {
    SDL_SetMainReady();

    // 1. Configure. A default EngineConfig is the faithful Game Boy Color baseline.
    const EngineConfig config{.window = {.title = "Hello, GBCPP"}};

    // 2. The four core objects.
    SteadyClock clock;
    SdlPlatform platform{config};
    Renderer    renderer{platform.device(), platform.window(), config.viewport};
    RunLoop     loop{clock, config.timing};

    // 3. Upload indexed art + a palette.
    constexpr int kTile = 8, kCols = 2;
    std::array<std::uint8_t, kTile * kTile * kCols> atlas{};
    for (int y = 0; y < kTile; ++y) {
        for (int x = 0; x < kTile; ++x) {
            const bool edge = (x == 0 || y == 0 || x == kTile - 1 || y == kTile - 1);
            atlas[y * (kTile * kCols) + x]           = 1;
            atlas[y * (kTile * kCols) + (kTile + x)] = edge ? 3 : 1;
        }
    }
    const AtlasId atlasId = renderer.uploadAtlas(atlas.data(), kTile * kCols, kTile);

    const std::array<Rgba8, 4> colours{{{20, 20, 30}, {70, 110, 180}, {0, 0, 0}, {200, 230, 255}}};
    const PaletteId pal = renderer.uploadPalette(std::span<const Rgba8>(colours));
    const std::array<PaletteId, 1> paletteSet{pal};

    // 4. A tilemap.
    constexpr int kMapW = 32, kMapH = 32;
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y)
        for (int x = 0; x < kMapW; ++x)
            cells[static_cast<std::size_t>(y) * kMapW + x].tile =
                static_cast<std::uint16_t>((x + y) % 2);

    // 5. One layer, built once.
    FrameDrawState frame;
    frame.layers.resize(1);
    DrawLayer& bg = frame.layers[0];
    bg.id      = "background";
    bg.z       = 0;
    bg.size    = PixelSize{config.viewport.width, config.viewport.height};
    bg.content = TileContent{atlasId, std::span<const PaletteId>(paletteSet),
                             kMapW, kMapH, std::span<const TileCell>(cells)};

    // 6. Wire the loop.
    int camX = 0, camY = 0;
    loop.setTick([&](const InputState& in) {
        if (in.isHeld(Button::Right)) ++camX;
        if (in.isHeld(Button::Left))  --camX;
        if (in.isHeld(Button::Down))  ++camY;
        if (in.isHeld(Button::Up))    --camY;
    });
    loop.setRender([&](float /*alpha*/) {
        frame.layers[0].scroll = LayerScroll{camX, camY};
        renderer.renderFrame(frame, /*alpha=*/0.0f);
    });

    // 7. Run.
    WindowedHost{loop, platform}.run();
    return 0;
}
```

## 3. What each part does

**`#define SDL_MAIN_HANDLED` + `SDL_SetMainReady()`.** SDL normally redirects your `main` to its own
entry shim. The engine initialises SDL itself (inside `SdlPlatform`), so you take ownership of `main`
with this define and the matching call. Boilerplate — every host does it once.

**Step 1 — configure.** [`EngineConfig`](platform-and-windowing.md) is one value bundle for startup:
window, internal viewport, timing, and input profile. Every field defaults to the faithful Game Boy
Color baseline, so `EngineConfig{}` reproduces the original behaviour and you override only what you
mean to — here just the window title with C++20 designated-initializer syntax.

**Step 2 — the four core objects.** This is the whole architecture in four lines (see
[concepts.md](concepts.md)):

- `SteadyClock` — the monotonic time source the loop reads. (Tests swap in a fake clock; you use the
  real one.)
- `SdlPlatform` — owns the OS window, the GPU device, and input. Constructed from the config.
- `Renderer` — draws. It takes the platform's live `device()` and `window()` and the configured
  viewport. Drawing is the renderer's job; the platform owns the window/device.
- `RunLoop` — the fixed-step scheduler. It takes the clock and the timing profile.

**Step 3 — upload art.** The engine's colour model is **indexed**: an atlas holds one palette *index*
per pixel, and colour comes from a palette chosen at render time — never baked into the art (full
detail in [tiles-and-colour.md](tiles-and-colour.md)). Here we build a tiny 16×8 atlas of two 8×8
tiles by hand: tile 0 is solid index 1, tile 1 has a bright (index 3) border. `uploadPalette` gives
those indices colours; `uploadAtlas` uploads the index plane. Both uploads are **amortized** — you do
them once, and the per-frame draw state just references the returned `AtlasId` / `PaletteId` handles.
A real game loads art from PNG instead of building it in code (see
[images-and-transparency.md](images-and-transparency.md)); hand-building keeps this example
dependency-free.

**Step 4 — a tilemap.** A 32×32 grid of `TileCell`s, each naming which atlas tile it shows. We
checkerboard tiles 0 and 1. The `cells` vector must stay alive as long as the layer references it —
it's declared in `main`, so it lives for the whole program.

**Step 5 — one layer, built once.** A [`FrameDrawState`](draw-state.md) is the whole frame you hand
the renderer: a stack of layers sorted by `z`. We make one tile layer pointing at our atlas, palette
set, and tilemap. Note we build it **once**, before the loop — the only thing that changes each frame
is its scroll, so there's no reason to rebuild it. (You *can* rebuild the frame every frame instead;
both styles are fine — see [the retained-vs-rebuilt recipe](how-to.md#retained-vs-rebuilt-frame).)

**Step 6 — wire the loop.** You give the loop two callbacks:

- **Tick** is one logical step of your game. It receives an [`InputState`](input.md) (held buttons +
  press/release edges) and updates game state — here, a camera moved by the held d-pad. Ticks run at
  the fixed timing-profile rate, so your logic is deterministic and frame-rate-independent.
- **Render** draws the current state. It runs once per displayed frame and calls
  `renderer.renderFrame(frame, alpha)`. We set the layer's scroll from the camera and submit. (`alpha`
  is for smoothing motion *between* ticks — see [run-loop-and-timing.md](run-loop-and-timing.md); this
  example ignores it.)

**Step 7 — run.** [`WindowedHost`](platform-and-windowing.md) is the driver: each iteration it pumps
OS events, pushes the current held buttons into the loop, and advances the simulation (which calls
your render). It returns when the window's close button is pressed.

## 4. Build and run it

The example builds with the engine (top-level builds turn examples on by default):

```sh
cmake --build build --target gbcpp-hello-world
./build/gbcpp-hello-world        # path varies by generator/platform
```

A window opens showing a blue-and-white checkerboard. Hold the arrow keys (the default keyboard
mapping) or a gamepad d-pad and the field scrolls. Close the window to quit.

## 5. Where to go next

- **Understand the architecture** you just used — [concepts.md](concepts.md).
- **Do specific things** — scroll, animate a sprite, make a character walk behind scenery, add a HUD,
  fade the screen, load a PNG: [how-to.md](how-to.md).
- **The frame you submit, in depth** — layers, sprites, z-ordering, modifiers: [draw-state.md](draw-state.md).
- **Colour** — indexed atlases, palettes, recolouring without new art: [tiles-and-colour.md](tiles-and-colour.md).
- **A richer running example** — the `gbcpp-window-demo` target
  ([`examples/window_demo.cpp`](../../examples/window_demo.cpp)) loads a real PNG, stacks two layers,
  and shows per-source transparency, rebuilding its frame each tick (the immediate-mode style).
