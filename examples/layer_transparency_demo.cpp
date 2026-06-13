// Layer transparency demo — a runnable host showcasing the INDEXED-tile/palette compositor + opt-in
// per-source index-hole transparency (ENG-2.B.3.a). Open a window, LOAD A REAL COMMITTED PNG
// (examples/assets/demo_tiles.png) via loadPng, upload its index plane TWICE through the existing
// uploadAtlas — once OPAQUE (no transparent index) and once with transparent index 0 — colour each
// through a hand-built palette set, and composite two role-free tile layers: a fully-opaque lower
// background (z=0) and, above it (z=10), the SAME art with index 0 declared transparent so its
// diamond-shaped index-0 regions become HOLES that reveal the opaque layer beneath. Then blit it
// integer-scaled + letterboxed onto the swapchain at display refresh, routing keyboard + gamepad
// input to the tick callback.
//
// Run it on a dev machine and confirm: the window shows two scrolling diamond fields in REAL
// COLOUR; the LOWER field is fully opaque (its central diamonds are a solid palette colour — the
// "no transparent index → faithful opaque background" control); the UPPER field's central diamonds
// are PUNCHED THROUGH (holes), revealing the lower field sliding past underneath as the two layers
// scroll at different rates — the same PNG, one upload opaque, one with an opt-in transparent index.
// Resizing re-letterboxes it, the close button quits, and pressing a mapped button prints a line.
//
// This is one of the runnable example hosts that instantiates SdlPlatform + Renderer in a real run,
// so it keeps the live SDL_GPU pipeline/upload/present path + the image-load front end compiling and
// linking on every CI platform even though CI never opens the window. (The beach demo —
// examples/beach_demo.cpp — is the companion that exercises the per-layer screen-space-effect path.)

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main and
// expect SDL's entry shim. We init SDL ourselves inside SdlPlatform.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "gbcpp/clock.h"
#include "gbcpp/draw_state.h"
#include "gbcpp/engine_config.h"
#include "gbcpp/geometry.h"
#include "gbcpp/image.h"
#include "gbcpp/input.h"
#include "gbcpp/palette.h"
#include "gbcpp/renderer.h"
#include "gbcpp/run_loop.h"
#include "gbcpp/sdl_platform.h"
#include "gbcpp/windowed_host.h"

namespace {

using namespace gbcpp;

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

    // One startup config bundles window + viewport + timing + controller profile; defaults are the
    // faithful Game Boy Color baseline — only the window title is overridden here.
    const EngineConfig config{
        .window = {.title = "GBCPP — layer transparency demo (index-hole)"}};

    SteadyClock clock;
    RunLoop     loop{clock, config.timing};
    SdlPlatform platform{config};
    Renderer    renderer{platform.device(), platform.window(), config.viewport};

    // Apply the startup presentation enhancements (ENG-2.C.1). The window already opened at
    // config.enhancements.windowScale (4×, clamped to the display) in the platform ctor; here we set
    // the blit sampler. windowScale is toggled live below; the renderer always auto-fills the window.
    renderer.setSamplingMode(config.enhancements.sampling);
    int windowScale = config.enhancements.windowScale;  // live-toggled target (clamped on apply)
    int waveMode = 0;  // frame-level row-displacement: 0 off, 1 blank edge, 2 stretch edge

    // Load the real committed PNG (an engine-authored, license-clean indexed tileset: a 2×2-tile
    // atlas whose four tiles assemble a diamond centred on index 0 — the hole). loadPng extracts its
    // index plane; the embedded palette is ignored here (the demo hand-builds colour).
    LoadedImage tiles;
    try {
        tiles = loadPng(assetPath("demo_tiles.png"));
    } catch (const std::exception& e) {
        std::printf("demo: could not load demo_tiles.png: %s\n", e.what());
        return 1;
    }

    // Upload the SAME index plane twice: once fully opaque (transparent index −1, the default), once
    // with index 0 declared transparent so it renders as a hole. This is the headline: identical art,
    // one solid, one punched through — the per-source indexed transparency policy.
    const AtlasId opaqueAtlas =
        renderer.uploadAtlas(tiles.indices.data(), tiles.width, tiles.height);          // −1 = opaque
    const AtlasId holeAtlas =
        renderer.uploadAtlas(tiles.indices.data(), tiles.width, tiles.height, /*transparentIndex=*/0);

    // Two hand-built 4-entry palettes (index 0 = the would-be hole colour on the opaque layer). Warm
    // for the opaque lower field, cool for the holed upper field — so revealing one through the other
    // is unmistakable.
    const std::array<Rgba8, 4> warm{{ {40, 18, 18}, {180, 70, 60}, {225, 130, 95}, {255, 220, 180} }};
    const std::array<Rgba8, 4> cool{{ {16, 22, 40}, {60, 110, 200}, {110, 175, 240}, {205, 235, 255} }};
    const PaletteId warmPal = renderer.uploadPalette(std::span<const Rgba8>(warm));
    const PaletteId coolPal = renderer.uploadPalette(std::span<const Rgba8>(cool));
    const std::array<PaletteId, 1> warmSet{warmPal};
    const std::array<PaletteId, 1> coolSet{coolPal};

    // A tilemap that lays the 2×2-tile atlas in repeating 2×2 super-blocks, so the diamond
    // reconstructs and repeats across the viewport. Kept alive for the program's duration.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            TileCell& c = cells[static_cast<std::size_t>(y) * kMapW + x];
            c.tile    = static_cast<std::uint16_t>((x % 2) + 2 * (y % 2));  // 0,1 / 2,3 block
            c.palette = 0;
        }
    }

    // The labelled buttons the demo prints — the Game Boy set (the demo's active profile).
    constexpr auto kLabels = std::to_array<std::pair<Button, const char*>>({
        {Button::Up, "Up"}, {Button::Down, "Down"}, {Button::Left, "Left"},
        {Button::Right, "Right"}, {Button::A, "A"}, {Button::B, "B"},
        {Button::Start, "Start"}, {Button::Select, "Select"},
    });

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

        // Live verification — overload three gameplay buttons as DEV toggles (demo only):
        //   Select → toggle native fullscreen (a real macOS Space) and back
        //   Start  → toggle blit sampling nearest ↔ bilinear (crisp ↔ smoothed)
        //   A      → cycle the window scale 1×…8× — resize the window to that multiple of the
        //            viewport (clamped to the display), the content auto-fills it crisply
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
            waveMode = (waveMode + 1) % 3;  // off → blank edge → stretch edge → off
            const char* names[] = {"off", "on (blank edge)", "on (stretch edge)"};
            std::printf("[dev] row-displacement: %s\n", names[waveMode]);
        }
        if (in.justPressed(Button::A)) {
            windowScale = (windowScale >= 8) ? 1 : windowScale + 1;  // 1→2→…→8→1
            const PixelSize vp{config.viewport.width, config.viewport.height};
            const int eff = fitWindowScale(vp, platform.usableDisplaySize(), windowScale);
            if (!platform.isFullscreen()) {
                platform.setWindowSize(PixelSize{vp.width * eff, vp.height * eff});
            }
            if (eff != windowScale) {
                std::printf("[dev] window scale: %d× requested, clamped to %d× (display limit)\n",
                            windowScale, eff);
            } else {
                std::printf("[dev] window scale: %d×\n", eff);
            }
        }
    });

    // The game owns the draw state; the render callback rebuilds + scrolls it each advance(). It
    // stacks TWO role-free tile layers from the SAME PNG: the opaque lower field, and above it the
    // holed upper field whose index-0 diamonds reveal the lower field through the holes.
    FrameDrawState frame;
    int tick = 0;
    loop.setRender([&](float alpha) {
        frame.layers.clear();

        // Gentle, SAME-DIRECTION parallax drift: advance a pixel only every few frames so the two
        // dense fields don't counter-scroll into a strobing moiré (a fast opposing scroll over an
        // 8px-repeating pattern flickers in the photosensitive band — keep it a calm drift).
        const int drift = tick / 6;  // ~10 px/s

        // z=0: the fully-opaque lower field (no transparent index → faithful opaque), drifting slowly.
        DrawLayer lower{};
        lower.id      = "opaqueLowerField";
        lower.z       = 0;
        lower.size    = PixelSize{160, 144};
        lower.scroll  = LayerScroll{drift / 2, 0};
        lower.alpha   = 1.0f;
        lower.content = TileContent{opaqueAtlas, std::span<const PaletteId>(warmSet),
                                    kMapW, kMapH, std::span<const TileCell>(cells)};
        frame.layers.push_back(std::move(lower));

        // z=10: the same art with index 0 transparent — its diamonds are HOLES revealing the lower
        // field beneath. Drifts the SAME direction a touch faster, so the reveal slides calmly.
        DrawLayer upper{};
        upper.id      = "holedUpperField";
        upper.z       = 10;
        upper.size    = PixelSize{160, 144};
        upper.scroll  = LayerScroll{drift, drift / 4};
        upper.alpha   = 1.0f;
        upper.content = TileContent{holeAtlas, std::span<const PaletteId>(coolSet),
                                    kMapW, kMapH, std::span<const TileCell>(cells)};
        frame.layers.push_back(std::move(upper));

        // A frame-level row-displacement post-process, cycled by B (off → blank edge → stretch edge).
        // A gentle, slow horizontal wave (small amplitude, phase advanced slowly off the frame
        // counter) so the whole composited frame wobbles — NO strobing / high-frequency flicker
        // (photosensitivity). Empty postEffects (waveMode == 0) is the faithful baseline.
        frame.postEffects.clear();
        if (waveMode != 0) {
            frame.postEffects.push_back(ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::RowDisplacement,
                .amplitude = 4.0f,                            // ±4 viewport px
                .frequency = 3.0f,                            // 3 wave crests down the screen
                .phase     = static_cast<float>(tick) * 0.01f,  // ~0.6 cycles/s — calm drift
                .axis      = Axis::Horizontal,
                .edge      = (waveMode == 2) ? DisplacementEdge::Stretch : DisplacementEdge::Blank});
        }

        // No frame-level modifier/blend (identity) — the faithful baseline blit is unchanged.
        renderer.renderFrame(frame, alpha);

        ++tick;
    });

    std::printf("layer transparency demo — a real indexed PNG uploaded twice (opaque lower field + a "
                "holed upper field whose index-0 diamonds reveal the lower field through the holes); "
                "close to quit.\n");
    std::printf("[dev] Select = fullscreen, Start = nearest/bilinear, A = cycle window scale "
                "(1×–8×, clamped to display), B = frame-level row-displacement wave.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
