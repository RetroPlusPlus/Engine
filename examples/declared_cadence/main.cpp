// Declared-cadence demo — two movers, identical motion, one of them declaring how often its world
// advances.
//
// WHAT YOU ARE LOOKING AT. Both blocks travel left and right at exactly the same speed, moving several
// pixels every few ticks rather than one pixel every tick — the way a game whose simulation runs on a
// divider moves. The ruled background is there to judge them against.
//
//   TOP (red)    declares nothing. Its move is eased across the single tick the change was submitted
//                on, so each step is split between two frames by whatever the sub-tick fraction happens
//                to be. Watch it for a few seconds: it slides evenly for a while, then visibly steps and
//                twitches, then goes smooth again. The wave is the beat between the simulation's cadence
//                and the display's, and it is roughly 3.7 s long on a 60 Hz screen.
//
//   BOTTOM (blue) declares `.advancesEvery = N` — the same N its world actually uses. Its move is spread
//                across the N ticks it really takes, so it advances by the same amount every frame no
//                matter where the fraction lands. It never develops the wave, and it draws N ticks
//                behind rather than one, which is why it trails the red block slightly.
//
// HOW YOU DRIVE IT. One field, on the layer (or on the frame, for every layer at once):
//
//     DrawLayer mover{.key = "smooth"};
//     mover.advancesEvery = 2;      // this layer's world steps every second tick
//
// Unset takes `FrameDrawState::advancesEvery`, which defaults to 1. A sprite advances with the layer it
// lives in. Both are read fresh every submission, so Z below turns the declaration on and off live.
//
// DEV KEYS: Z = declare / stop declaring on the blue block, X = cycle the divider (2 / 3 / 4 ticks per
// step), Up/Down = step size, Backspace = fullscreen. Close to quit.
//
// A dev drives the window.

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
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;
constexpr int kBlock = 8;
constexpr int kTopY = 44, kBottomY = 92;

enum class Action : std::uint8_t { ToggleDeclare, CycleDivider, StepUp, StepDown, Fullscreen };

}  // namespace

int main() {
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "DeclaredCadence"},
        .window   = {.title = "Polyrhythm — declared cadence (a world that advances every few ticks)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::ToggleDeclare, {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::CycleDivider, {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::StepUp, {SDL_SCANCODE_UP, PadButton::DpadUp}},
        {Action::StepDown, {SDL_SCANCODE_DOWN, PadButton::DpadDown}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // Programmatic art. One solid 8x8 tile for the movers, and one ruled tile — a single column lit —
    // that tiles into a vertical grid the eye can measure motion against.
    std::array<std::uint8_t, 64> solid{};
    solid.fill(1);
    const AtlasId blockAtlas = renderer.uploadAtlas(solid.data(), 8, 8).atlasId;

    std::array<std::uint8_t, 64> ruled{};
    for (int y = 0; y < 8; ++y) ruled[static_cast<std::size_t>(y) * 8] = 1;
    const AtlasId ruleAtlas = renderer.uploadAtlas(ruled.data(), 8, 8).atlasId;

    const auto palette = [&](Rgba8 ink, Rgba8 paper) {
        const std::array<Rgba8, 2> pal{{paper, ink}};
        return renderer.uploadPalette(std::span<const Rgba8>(pal));
    };
    const PaletteId rulePal   = palette(Rgba8{44, 44, 58}, Rgba8{18, 18, 24});
    const PaletteId steppyPal = palette(Rgba8{226, 72, 72}, Rgba8{0, 0, 0, 0});
    const PaletteId smoothPal = palette(Rgba8{88, 156, 236}, Rgba8{0, 0, 0, 0});

    std::vector<TileCell> ruleCells(static_cast<std::size_t>(kMapW) * kMapH);
    for (auto& c : ruleCells) c = TileCell{.atlas = ruleAtlas, .tile = 0, .palette = rulePal};

    // The world: both movers share one position, so any difference on screen is the declaration alone.
    int  x         = 0;
    int  dir       = 1;
    int  step      = 4;    // pixels per advance
    int  divider   = 2;    // ticks per advance — what the world actually does
    bool declaring = true; // whether the blue block says so
    int  tick      = 0;

    loop.simTick([&](const InputState& in) {
        ++tick;
        if (in.justPressed(Action::ToggleDeclare)) {
            declaring = !declaring;
            std::printf("[dev] blue block %s\n",
                        declaring ? "declares its cadence" : "declares nothing (same as red)");
        }
        if (in.justPressed(Action::CycleDivider)) {
            divider = divider >= 4 ? 2 : divider + 1;
            std::printf("[dev] the world advances every %d ticks\n", divider);
        }
        if (in.justPressed(Action::StepUp)) {
            step += 2;
            std::printf("[dev] step = %d px\n", step);
        }
        if (in.justPressed(Action::StepDown)) {
            step = step > 2 ? step - 2 : 2;
            std::printf("[dev] step = %d px\n", step);
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());

        if (tick % divider == 0) {                 // the world advances on its own divider
            x += dir * step;
            if (x >= kViewW - kBlock) { x = kViewW - kBlock; dir = -1; }
            if (x <= 0)               { x = 0;               dir = 1; }
        }
    });

    FrameDrawState      frame;
    std::vector<Sprite> steppy, smooth;
    loop.renderLoop([&]() {
        frame.layers.clear();

        DrawLayer rule{.key = "rule"};
        rule.z       = 0;
        rule.size    = PixelSize{kViewW, kViewH};
        rule.content = TileContent{.widthInTiles  = kMapW,
                                   .heightInTiles = kMapH,
                                   .cells         = std::span<const TileCell>(ruleCells)};
        frame.layers.push_back(rule);

        steppy.assign({Sprite{.key     = "steppy",
                              .x       = x,
                              .y       = kTopY,
                              .atlas   = blockAtlas,
                              .tile    = 0,
                              .palette = steppyPal}});
        DrawLayer top{.key = "undeclared"};
        top.z       = 1;
        top.size    = PixelSize{kViewW, kViewH};
        top.content = SpriteContent{std::span<const Sprite>(steppy)};
        frame.layers.push_back(top);

        smooth.assign({Sprite{.key     = "smooth",
                              .x       = x,
                              .y       = kBottomY,
                              .atlas   = blockAtlas,
                              .tile    = 0,
                              .palette = smoothPal}});
        DrawLayer bottom{.key = "declared"};
        bottom.z       = 2;
        bottom.size    = PixelSize{kViewW, kViewH};
        bottom.content = SpriteContent{std::span<const Sprite>(smooth)};
        // The one line this demo is about. Unset, the layer takes the frame's default of 1 and behaves
        // exactly like the red block above it.
        if (declaring) bottom.advancesEvery = static_cast<std::uint32_t>(divider);
        frame.layers.push_back(bottom);

        renderer.renderFrame(frame);
    });

    std::printf("declared-cadence demo — two blocks, identical motion. The RED one declares nothing, so a "
                "multi-tick move is eased across one tick and the split depends on where the sub-tick "
                "fraction falls: watch it go smooth, then stepped, then smooth again on a ~3.7 s beat. The "
                "BLUE one declares `.advancesEvery`, so the move is spread across the ticks it takes and "
                "the wave never appears — at the cost of drawing that many ticks behind.\n");
    std::printf("[dev] Z = toggle the blue block's declaration, X = cycle the divider, Up/Down = step size, "
                "Backspace = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
