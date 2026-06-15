// Hello, world! — the bare minimum: open a window and show "Hello, world!" on screen. It loads one
// committed indexed image (examples/assets/hello_world.png — index 0 = background, 1 = ink) as a
// single-asset atlas, draws it as one sprite centred in the viewport, and runs until you close the
// window. Nothing else.
//
// (For a first *interactive* program — input + a scrolling world — see examples/controller_scrolling.cpp
// and docs/guide/getting-started.md.) Built on every CI platform so it keeps compiling against the live
// engine; run it on a dev machine (CI has no display).

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <span>
#include <string>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/image.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

using namespace retropp;

int main() {
    SDL_SetMainReady();

    // Set the active config ONCE; the bare engine ctors below then inherit it (window + input from
    // EngineConfig::active, viewport + timing from the per-type defaults setActive fans out) — no
    // per-ctor threading. Explicit threading (e.g. RunLoop{clock, config.timing}) still works as an
    // override; this is the recommended minimal startup. A default config is the faithful GBC baseline.
    const EngineConfig config{.window = {.title = "Hello, world!"}};
    EngineConfig::setActive(config);

    SteadyClock clock;
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    RunLoop     loop{clock};

    // Load the committed text image. loadAtlas decodes the PNG, uploads it once, and slices it — here
    // as ONE whole-image asset (ContentKind::Single) — returning a manifest whose single slot gives us
    // the image's atlas cell and pixel size. The assets dir is copied next to the executable after the
    // build; SDL_GetBasePath finds it.
    const char* base = SDL_GetBasePath();  // SDL-owned, do not free
    const AtlasManifest text = renderer.loadAtlas(
        (base ? std::string{base} : std::string{}) + "assets/hello_world.png",
        AssetDimensions::GameBoy8x8, ContentKind::Single);

    // A two-colour palette: index 0 is transparent on the sprite path (the background shows through),
    // index 1 is the ink. Colour is applied at render time — never baked into the art.
    const std::array<Rgba8, 2> palette{{{0, 0, 0}, {235, 235, 245}}};
    const PaletteId pal = renderer.uploadPalette(std::span<const Rgba8>(palette));
    const std::array<PaletteId, 1> paletteSet{pal};

    // One sprite, the whole image (the manifest's single slot), centred in the 160×144 viewport.
    const std::array<Sprite, 1> sprites{Sprite{
        .x    = (config.viewport.width  - text[0].dimensions.width)  / 2,
        .y    = (config.viewport.height - text[0].dimensions.height) / 2,
        .size = text[0].dimensions,
        .tile = text[0].tile}};

    FrameDrawState frame;
    loop.setRender([&](float alpha) {
        frame.layers.clear();
        DrawLayer layer{};
        layer.id      = "text";
        layer.z       = 0;
        layer.size    = PixelSize{config.viewport.width, config.viewport.height};
        layer.content = SpriteContent{text.atlas, std::span<const PaletteId>(paletteSet),
                                      std::span<const Sprite>(sprites)};
        frame.layers.push_back(std::move(layer));
        renderer.renderFrame(frame, alpha);
    });

    WindowedHost{loop, platform}.run();
    return 0;
}
