// Blend-mode demo — a runnable host that VISUALLY proves the container blend modes: each compositing
// container (a DrawLayer, a Region, the FrameDrawState) carries a BlendMode beside its alpha that governs
// HOW its pixels combine with what they sit on. It opens a window over an opaque slate-grid scene and lays
// out one clearly distinct element per kind:
//
//   • LEFT HALF — a translucent HALF LAYER (DrawLayer::blend = Half): a warm colour layer covering only the
//     left half (a finite tilemap with a hard right edge), averaged with the scene, (dst + src) / 2. The
//     visible boundary down the middle shows the whole layer is translucent — no per-pixel alpha.
//   • RIGHT HALF — FOUR mode circles, each a Region with the SAME grey ColorFill but a different
//     Region::blend, so the only difference you see is the blend math:
//        top-left  Add        dst + src              (brightens)
//        top-right Subtract   dst − src              (darkens hard)
//        bot-left  Multiply   dst · src              (darkens)
//        bot-right Screen     1 − (1 − dst)(1 − src) (lifts toward light)
//   • WHOLE FRAME — a gentle cool SCREEN overlay (FrameDrawState::blendMode = Screen): a whole-frame
//     ColorFill combined over the composited image with the frame's blend mode, lifting the scene slightly.
//
// A Normal container (the default) is the plain alpha-over the compositor always ran — that is the
// untouched slate grid. The pixel-exact blend math is the device-free ctest suite's job (applyBlendMode vs
// the shaders); this is the live GPU sanity check.
//
// Photosensitivity: the scene is STATIC — nothing moves, flashes, or strobes. The window never auto-launches
// (a dev drives it). Select = fullscreen; close to quit.

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;  // 20×18 tiles cover the 160×144 viewport
constexpr int kHalfW = 10;             // the Half layer's tilemap is 10 tiles wide → the left half only

// A solid colour fill (paint the region's shape this colour). The owning Region's blend mode grades how it
// combines over the scene — Add brightens, Subtract/Multiply darken, Screen lifts.
[[nodiscard]] ScreenSpaceEffect solidFill(Rgba8 colour) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = colour};
}

// A Region that fills `shape` with `colour` and composites it over the scene with `mode`.
[[nodiscard]] Region blendedFill(ShapePoints shape, Rgba8 colour, BlendMode mode) {
    return Region{.shape = std::move(shape), .effects = {solidFill(colour)}, .blend = mode};
}

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{.window = {.title = "Retro++ — blend-mode demo (Half layer / Add / Subtract / Multiply / Screen)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    renderer.setSamplingMode(config.enhancements.sampling);

    // The opaque scene the containers blend over: a two-tone slate grid.
    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8);
    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {52, 58, 82}, {72, 80, 110}}};  // opaque slate grid
    const std::array<PaletteId, 1> gridSet{renderer.uploadPalette(std::span<const Rgba8>(gridPal))};
    const std::vector<TileCell>    gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                             TileCell{.tile = 0, .palette = 0});

    // The Half layer's art: solid warm tiles, so the Half blend is a clean (grid + warm) / 2 average.
    std::array<std::uint8_t, 64> warmArt{};
    warmArt.fill(1);
    const AtlasId warmAtlas = renderer.uploadAtlas(warmArt.data(), 8, 8);
    const std::array<Rgba8, 2> warmPal{{{0, 0, 0}, {235, 120, 40}}};  // warm orange
    const std::array<PaletteId, 1> warmSet{renderer.uploadPalette(std::span<const Rgba8>(warmPal))};
    const std::vector<TileCell>    warmCells(static_cast<std::size_t>(kHalfW) * kMapH,
                                             TileCell{.tile = 0, .palette = 0});

    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::Select)) platform.setFullscreen(!platform.isFullscreen());
    });

    FrameDrawState frame;
    loop.setRender([&](float alpha) {
        frame.layers.clear();

        // The opaque scene (whole screen).
        DrawLayer bg{};
        bg.id      = "backgroundGrid";
        bg.z       = -10;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{gridAtlas, std::span<const PaletteId>(gridSet),
                                 kMapW, kMapH, std::span<const TileCell>(gridCells)};
        frame.layers.push_back(bg);

        // The translucent HALF LAYER: a finite 10×18 warm map (wrap = Blank), so it covers ONLY the left
        // half and is transparent past its right edge. blend = Half averages it with the scene, (dst+src)/2,
        // so the whole left panel is translucent with no per-pixel alpha — the visible mid-screen boundary.
        DrawLayer half{};
        half.id      = "halfLayer";
        half.z       = 0;
        half.size    = PixelSize{kViewW, kViewH};
        half.blend   = BlendMode::Half;
        half.content = TileContent{warmAtlas, std::span<const PaletteId>(warmSet),
                                   kHalfW, kMapH, std::span<const TileCell>(warmCells), TileWrap::Blank};
        frame.layers.push_back(half);

        // FOUR mode circles in the right half, same grey fill so only the blend differs.
        constexpr Rgba8 grey{120, 120, 120};
        const float r = 18.0f;
        frame.regions.clear();
        frame.regions.push_back(blendedFill(ShapePoints::circle(Point{104, 44}, r),  grey, BlendMode::Add));
        frame.regions.push_back(blendedFill(ShapePoints::circle(Point{140, 44}, r),  grey, BlendMode::Subtract));
        frame.regions.push_back(blendedFill(ShapePoints::circle(Point{104, 100}, r), grey, BlendMode::Multiply));
        frame.regions.push_back(blendedFill(ShapePoints::circle(Point{140, 100}, r), grey, BlendMode::Screen));

        // The frame-level SCREEN overlay: a cool whole-frame ColorFill, combined over the composited image
        // with the frame's blend mode — Screen lifts the scene gently toward light.
        frame.postEffects.clear();
        frame.postEffects.push_back(solidFill(Rgba8{34, 44, 78}));
        frame.blendMode = BlendMode::Screen;

        renderer.renderFrame(frame, alpha);
    });

    std::printf("blend-mode demo — LEFT half: a translucent HALF layer (DrawLayer::blend = Half), with a hard "
                "boundary down the middle. RIGHT half, four circles (same grey fill, different Region::blend): "
                "top-left Add, top-right Subtract, bottom-left Multiply, bottom-right Screen. The whole scene is "
                "gently lifted by a frame-level Screen overlay (FrameDrawState::blendMode). Static scene; "
                "Select = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
