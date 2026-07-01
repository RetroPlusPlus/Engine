// Per-row effect data-table demo — an effect reads a per-SCANLINE value from its paramTable, the input a
// scalar cbuffer + a closed-form f(row, time) shader cannot express. The table here is a hand-built,
// digitized HORIZONTAL SCALE ramp: a dome that scales the middle scanlines wider/narrower than the edges,
// its depth drifting slowly. The custom shader (row_warp.frag.hlsl) reads the row under each fragment and
// samples the source at the scaled UV. A single sine could not make this profile — the game writes the
// whole array each frame and the effect reads it row by row.
//
// The scene is a procedural diagonal-band tilemap (no PNG): four solid-colour tiles, a 20×18 map, scrolling
// slowly so the per-line warp is legible against moving structure.
//
// Run on a dev machine:
//   B  — toggle the effect on / off (off = the table is unused → identity, the unwarped scene).
//   Up — toggle WHERE the effect runs: the WHOLE frame (FrameDrawState::postEffects) vs a CIRCLE REGION in
//        the centre (FrameDrawState::regions) — the same table-driven effect, confined for free by a Region.
//   Select — fullscreen; Start — nearest / bilinear.
// The dome drifts SLOWLY off the frame counter — no strobing / high-frequency flicker (photosensitivity).

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main and expect SDL's entry
// shim. We init SDL ourselves inside SdlPlatform.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewportW = 160;
constexpr int kViewportH = 144;  // one paramTable row per scanline
constexpr int kMapW      = 20;   // 20×18 tiles covers the 160×144 viewport
constexpr int kMapH      = 18;

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{
        .window = {.title = "Retro++ — per-row effect data-table demo (per-line scale ramp)"}};

    EngineConfig::setActive(config);  // the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};

    // Register the custom warp shader BY PATH — the build scans this source, sees the .hlsl path, injects
    // the engine preamble (which declares the row-data table + paramRow / paramRowAtUv), compiles + embeds
    // it, and registers it. The shader declares no params of its own; its entire input is the per-row table.
    const PostProcessStageId warp =
        renderer.registerPostProcessStage("examples/row_data_table_demo/shaders/row_warp.frag.hlsl");

    // A procedural indexed atlas: four 8×8 tiles, tile k filled with palette index k (one solid colour
    // each). Laid out 4 tiles wide × 1 tall = 32×8 indices.
    std::array<std::uint8_t, 32 * 8> atlasPixels{};
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 32; ++col) {
            atlasPixels[static_cast<std::size_t>(row) * 32 + col] = static_cast<std::uint8_t>(col / 8);
        }
    }
    const AtlasId atlas = renderer.uploadAtlas(atlasPixels.data(), 32, 8);

    const std::array<Rgba8, 4> colours{{{30, 30, 46}, {235, 110, 75}, {95, 180, 235}, {245, 220, 130}}};
    const PaletteId palette = renderer.uploadPalette(std::span<const Rgba8>(colours));

    // Diagonal bands so horizontal per-line scaling is legible: tile = (x + y) % 4. Each cell names the one
    // sheet + palette directly.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            cells[static_cast<std::size_t>(y) * kMapW + x] =
                TileCell{.tile = static_cast<std::uint16_t>((x + y) % 4), .atlas = atlas, .palette = palette};
        }
    }

    bool effectOn       = true;
    bool regionConfined = false;
    auto targetName     = [](bool region) { return region ? "centre circle region" : "whole frame"; };

    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::Select)) {
            platform.setFullscreen(!platform.isFullscreen());
            std::printf("[dev] fullscreen: %s\n", platform.isFullscreen() ? "on" : "off");
        }
        if (in.justPressed(Button::Start)) {
            const bool toBilinear = renderer.samplingMode() == SamplingMode::Nearest;
            renderer.setSamplingMode(toBilinear ? SamplingMode::Bilinear : SamplingMode::Nearest);
            std::printf("[dev] sampling: %s\n", toBilinear ? "bilinear" : "nearest");
        }
        if (in.justPressed(Button::B)) {
            effectOn = !effectOn;
            std::printf("[dev] effect: %s\n", effectOn ? "on" : "off (identity)");
        }
        if (in.justPressed(Button::Up)) {
            regionConfined = !regionConfined;
            std::printf("[dev] effect target: %s\n", targetName(regionConfined));
        }
    });

    std::vector<Vec4> scaleTable(static_cast<std::size_t>(kViewportH));  // one Vec4 per scanline; refilled each frame
    FrameDrawState    frame;
    int               tick = 0;
    loop.setRender([&]() {
        const float t     = static_cast<float>(tick);
        const int   drift = tick / 6;  // gentle same-direction scroll (~10 px/s); no strobing moiré

        // Refill the per-line scale table: a dome (centre scanlines scaled, edges left at 1.0) whose depth
        // drifts slowly between squeeze and stretch. Only the x lane is used; the rest stay 0.
        const float depth = 0.30f * std::sin(t * 0.02f);  // slow [-0.30, 0.30]
        for (int r = 0; r < kViewportH; ++r) {
            const float v    = kViewportH > 1 ? static_cast<float>(r) / (kViewportH - 1) : 0.0f;  // 0..1
            const float dome = 1.0f - std::fabs(v - 0.5f) * 2.0f;                                 // 1 centre → 0 edges
            scaleTable[static_cast<std::size_t>(r)] = Vec4{1.0f + depth * dome, 0.0f, 0.0f, 0.0f};
        }

        frame.layers.clear();
        frame.postEffects.clear();
        frame.regions.clear();

        DrawLayer bg{};
        bg.label   = "diagonalBands";
        bg.z       = 0;
        bg.size    = PixelSize{kViewportW, kViewportH};
        bg.scroll  = LayerScroll{drift, drift / 2};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(cells)};
        frame.layers.push_back(std::move(bg));

        if (effectOn) {
            // The same table-driven Custom effect, attached whole-frame or confined to a centre circle. The
            // paramTable is the inline per-frame span (valid for this renderFrame call).
            const ScreenSpaceEffect fx{.kind       = ScreenSpaceEffectKind::Custom,
                                       .customShader = warp,
                                       .paramTable = std::span<const Vec4>(scaleTable)};
            if (regionConfined) {
                frame.regions.push_back(
                    Region{.shape   = ShapePoints::circle(Point{kViewportW / 2.0f, kViewportH / 2.0f}, 55.0f),
                           .effects = {fx}});
            } else {
                frame.postEffects.push_back(fx);
            }
        }

        renderer.renderFrame(frame);
        ++tick;
    });

    std::printf("per-row effect data-table demo — a custom effect reads a per-scanline horizontal-scale "
                "ramp from its paramTable (a curve a single sine can't make). Close to quit.\n");
    std::printf("[dev] B = effect on/off, Up = target (whole frame / centre circle region), "
                "Select = fullscreen, Start = nearest/bilinear.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
