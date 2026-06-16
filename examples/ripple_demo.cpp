// Ripple demo — a runnable host showcasing the engine's BUILT-IN radial ripple effect (ENG-2.I.a).
// Based on the layer-transparency demo (two scrolling indexed-PNG tile layers in real colour); the
// addition is the built-in ripple, named and parameterized like any other screen-space effect.
//
// The ripple (ScreenSpaceEffectKind::Ripple) is a RADIAL water-droplet RIPPLE emanating from the centre
// of the screen — concentric rings expanding slowly outward. It does something the axis-aligned built-in
// RowDisplacement cannot, and it is now a first-class member of the engine's effect library: the game
// names the kind (.kind = ScreenSpaceEffectKind::Ripple) and sets its fields — no shader, no registration.
// The demo also stacks the ripple WITH RowDisplacement to confirm two built-ins compose in one chain.
//
// (Until ENG-2.I.a the ripple lived as a consumer-authored custom shader here, demonstrating the Issue-5
// custom-shader hook; it was promoted to a built-in, so this demo no longer registers a fragment. The
// custom-shader registration path + its device-free tests remain; ENG-2.I.b's bespoke swirl restores the
// consumer-fragment-generates-on-all-backends CI signal.)
//
// Run it on a dev machine and confirm: the two diamond fields scroll as before; pressing B drops a
// ripple in the centre and rings expand outward across the whole frame; pressing Up adds the built-in
// horizontal wave on top, the two composing; both off restores the faithful frame. SLOW expansion
// only — no strobing / high-frequency flicker (photosensitivity).

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main and
// expect SDL's entry shim. We init SDL ourselves inside SdlPlatform.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/image.h"
#include "retropp/input.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;  // the faithful GBC viewport
constexpr int kMapW = 20;  // tilemap dimensions in tiles (covers the 160×144 viewport: 20×18)
constexpr int kMapH = 18;

// Locate a committed asset next to the executable (CMake copies examples/assets there post-build).
std::string assetPath(const char* name) {
    const char* base = SDL_GetBasePath();  // SDL-owned, do not free (SDL3)
    return (base ? std::string{base} : std::string{}) + "assets/" + name;
}

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{
        .window = {.title = "Retro++ — custom shader demo (radial ripple)"}};

    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    renderer.setSamplingMode(config.enhancements.sampling);

    bool rippleOn = false;  // B — the built-in radial ripple
    bool waveOn   = false;  // Up — the built-in RowDisplacement (to show the two compose)

    // Load the committed indexed PNG (a 2×2-tile diamond atlas centred on index 0).
    LoadedImage tiles;
    try {
        tiles = loadPng(assetPath("demo_tiles.png"));
    } catch (const std::exception& e) {
        std::printf("demo: could not load demo_tiles.png: %s\n", e.what());
        return 1;
    }

    const AtlasId opaqueAtlas =
        renderer.uploadAtlas(tiles.indices.data(), tiles.width, tiles.height);
    const AtlasId holeAtlas =
        renderer.uploadAtlas(tiles.indices.data(), tiles.width, tiles.height, /*transparentIndex=*/0);

    const std::array<Rgba8, 4> warm{{ {40, 18, 18}, {180, 70, 60}, {225, 130, 95}, {255, 220, 180} }};
    const std::array<Rgba8, 4> cool{{ {16, 22, 40}, {60, 110, 200}, {110, 175, 240}, {205, 235, 255} }};
    const PaletteId warmPal = renderer.uploadPalette(std::span<const Rgba8>(warm));
    const PaletteId coolPal = renderer.uploadPalette(std::span<const Rgba8>(cool));
    const std::array<PaletteId, 1> warmSet{warmPal};
    const std::array<PaletteId, 1> coolSet{coolPal};

    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            TileCell& c = cells[static_cast<std::size_t>(y) * kMapW + x];
            c.tile    = static_cast<std::uint16_t>((x % 2) + 2 * (y % 2));
            c.palette = 0;
        }
    }

    loop.setTick([&](const InputState& in) {
        // Live verification — overload buttons as DEV toggles (demo only):
        if (in.justPressed(Button::Select)) {
            platform.setFullscreen(!platform.isFullscreen());
            std::printf("[dev] fullscreen: %s\n", platform.isFullscreen() ? "on" : "off");
        }
        if (in.justPressed(Button::Start)) {
            const bool bilinear = renderer.samplingMode() == SamplingMode::Nearest;
            renderer.setSamplingMode(bilinear ? SamplingMode::Bilinear : SamplingMode::Nearest);
            std::printf("[dev] sampling: %s\n", bilinear ? "bilinear" : "nearest");
        }
        if (in.justPressed(Button::B)) {
            rippleOn = !rippleOn;
            std::printf("[dev] custom ripple: %s\n", rippleOn ? "on" : "off");
        }
        if (in.justPressed(Button::Up)) {
            waveOn = !waveOn;
            std::printf("[dev] built-in row-displacement: %s\n", waveOn ? "on" : "off");
        }
    });

    FrameDrawState frame;
    int            tick = 0;
    loop.setRender([&](float alpha) {
        frame.layers.clear();
        const int drift = tick / 6;  // gentle same-direction parallax (~10 px/s); no strobing moiré

        DrawLayer lower{};
        lower.id      = "opaqueLowerField";
        lower.z       = 0;
        lower.size    = PixelSize{160, 144};
        lower.scroll  = LayerScroll{drift / 2, 0};
        lower.content = TileContent{opaqueAtlas, std::span<const PaletteId>(warmSet),
                                    kMapW, kMapH, std::span<const TileCell>(cells)};
        frame.layers.push_back(std::move(lower));

        DrawLayer upper{};
        upper.id      = "holedUpperField";
        upper.z       = 10;
        upper.size    = PixelSize{160, 144};
        upper.scroll  = LayerScroll{drift, drift / 4};
        upper.content = TileContent{holeAtlas, std::span<const PaletteId>(coolSet),
                                    kMapW, kMapH, std::span<const TileCell>(cells)};
        frame.layers.push_back(std::move(upper));

        // The post-process chain composes two BUILT-IN effects — the radial ripple and the axis-aligned
        // wave — in submission order: the headline is that the ripple is a first-class effect kind, used
        // exactly where RowDisplacement is. `phase` advances slowly off the frame counter (SLOW expansion
        // — photosensitivity). Empty chain (both off) is the faithful baseline.
        frame.postEffects.clear();
        if (rippleOn) {
            // center in VIEWPORT PIXELS — screen centre (the droplet impact point); the engine normalizes
            // it to UV. ±6 px amplitude, ~6 rings, slow outward phase, radial decay (droplet falloff).
            frame.postEffects.push_back(ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::Ripple, .amplitude = 6.0f, .frequency = 6.0f,
                .phase = static_cast<float>(tick) * 0.012f,
                .center = {kViewW / 2.0f, kViewH / 2.0f}, .decay = 2.5f});
        }
        if (waveOn) {
            frame.postEffects.push_back(ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 3.0f, .frequency = 3.0f,
                .phase = static_cast<float>(tick) * 0.01f, .axis = Axis::Horizontal});
        }

        renderer.renderFrame(frame, alpha);
        ++tick;
    });

    std::printf("ripple demo — the engine's BUILT-IN radial ripple; rings expand from the centre like a "
                "dropped water droplet. Close to quit.\n");
    std::printf("[dev] B = built-in ripple, Up = built-in row-displacement (stack them — they compose), "
                "Select = fullscreen, Start = nearest/bilinear.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
