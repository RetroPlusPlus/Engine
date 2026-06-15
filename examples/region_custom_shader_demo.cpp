// ENG-2.F focused example #6 — a CUSTOM shader confined to a region.
//
// One idea: the region gate is engine-side, so it works on a game-registered CUSTOM shader EXACTLY as
// on a built-in — with no change to the custom-shader contract. This reuses the consumer-authored radial
// ripple from the custom-shader demo (examples/shaders/ripple.frag.hlsl), registered as a custom stage,
// and confines it to a circle: the droplet rings expand only inside the porthole, the rest of the grid
// is still. B toggles the region on/off so you can see the same custom shader run whole-frame vs gated.
//
// Opens a real window so the live registration + gate path keep compiling on every CI platform. SLOW
// expansion only — no strobing (photosensitivity).

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
#include "retropp/shader_format.h"
#include "retropp/windowed_host.h"

#include "shaders/generated/ripple_frag.h"  // build-time-generated from examples/shaders/ripple.frag.hlsl

namespace {
using namespace retropp;
constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;

// The ripple shader's own uniform — byte-for-byte ripple.frag.hlsl's RippleUniforms (two registers).
struct RippleUniforms {
    float centerX, centerY, amplitude, frequency;
    float phase, invViewportW, invViewportH, decay;
};
static_assert(sizeof(RippleUniforms) == 32, "RippleUniforms must match ripple.frag's cbuffer");
}  // namespace

int main() {
    SDL_SetMainReady();
    const EngineConfig config{.window = {.title = "Retro++ — ENG-2.F: custom shader in a region"}};
    SteadyClock clock;
    RunLoop     loop{clock, config.timing};
    SdlPlatform platform{config};
    Renderer    renderer{platform.device(), platform.window(), config.viewport};
    renderer.setSamplingMode(config.enhancements.sampling);

    // Register the consumer ripple as a custom stage (the engine builds its ShaderVariants from the same
    // generated symbol set as its own shaders).
    using namespace retropp::shaders::ripple_frag;
    const ShaderVariants rippleFrag{{kSpirv, sizeof(kSpirv), kSpirvEntrypoint},
                                    {kDxil, sizeof(kDxil), kDxilEntrypoint},
                                    {kMsl, sizeof(kMsl), kMslEntrypoint}};
    const PostProcessStageId rippleStage = renderer.registerPostProcessStage(rippleFrag, sizeof(RippleUniforms));

    std::array<std::uint8_t, 64> grid{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            grid[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId atlas = renderer.uploadAtlas(grid.data(), 8, 8);
    const std::array<Rgba8, 3> pal{{{0, 0, 0}, {44, 70, 120}, {180, 210, 255}}};
    const PaletteId p = renderer.uploadPalette(std::span<const Rgba8>(pal));
    const std::array<PaletteId, 1> palSet{p};
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH, TileCell{.tile = 0, .palette = 0});

    bool gated = true;  // B: confine the ripple to a circle vs run it whole-frame
    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::B)) { gated = !gated; std::printf("[dev] ripple region: %s\n", gated ? "circle" : "whole frame"); }
        if (in.justPressed(Button::Select)) platform.setFullscreen(!platform.isFullscreen());
    });

    FrameDrawState frame;
    int            tick = 0;
    RippleUniforms ripple{};  // lives across renderFrame — the effect's uniform span points at it
    loop.setRender([&](float alpha) {
        frame.layers.clear();
        DrawLayer bg{};
        bg.id      = "grid";
        bg.z       = 0;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{atlas, std::span<const PaletteId>(palSet),
                                 kMapW, kMapH, std::span<const TileCell>(cells)};
        frame.layers.push_back(bg);

        ripple = RippleUniforms{.centerX = 0.5f, .centerY = 0.5f, .amplitude = 6.0f, .frequency = 6.0f,
                                .phase = static_cast<float>(tick) * 0.012f,
                                .invViewportW = 1.0f / kViewW, .invViewportH = 1.0f / kViewH, .decay = 2.5f};
        ScreenSpaceEffect rip{
            .kind = ScreenSpaceEffectKind::Custom, .customShader = rippleStage,
            .uniform = std::as_bytes(std::span<const RippleUniforms>(&ripple, 1))};
        if (gated) rip.region = ShapePoints::circle({80, 72}, 40);  // SAME custom shader, now confined
        frame.postEffects.clear();
        frame.postEffects.push_back(rip);

        renderer.renderFrame(frame, alpha);
        ++tick;
    });

    std::printf("ENG-2.F custom shader in a region — the consumer ripple confined to a circle (engine-side "
                "gate, custom-shader contract untouched). B toggles circle vs whole frame. Select = fullscreen.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
