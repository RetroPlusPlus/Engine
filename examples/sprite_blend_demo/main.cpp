// Sprite-blend demo — a runnable host for per-sprite `Sprite::blend`. A hero disc bobs across a scrolling
// textured scene carrying two blended decals, and a probe disc lets you cycle one sprite through every
// blend mode over the same moving background:
//
//   • SHADOW (Multiply, alpha 0.7) — a soft grey decal rides at the hero's feet, one z below it. Multiply
//     darkens the scene under it, most where the decal is darkest (its centre), so it reads as a shadow
//     that tracks the hero over whatever tile it crosses.
//   • FLARE (Add) — a warm radial decal pulses one z above the hero. Add lifts the scene toward light,
//     brightest at the decal's centre — a glow that never darkens.
//   • PROBE — a single disc mid-screen. Press A to cycle its blend (Normal → Multiply → Add → Screen →
//     Subtract → Half) and watch the SAME sprite grade the scene beneath it differently at each mode.
//
// Every sprite here rides one ordinary sprite layer over the scene, so each grades the accumulated scene
// beneath it (the container rule for a direct-to-accumulator layer). The pixel-exact blend math is the
// ctest suite's job (applyBlendMode vs the shaders, and the per-sprite matrix in sprite_blend_test.cpp);
// this is the live GPU sanity check + a teaching scene.
//
// Motion advances on the sim tick (RunLoop::tickCount), so it runs the same on any display. A = cycle the
// probe's blend mode; Backspace = fullscreen; close to quit.

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
#include "retropp/image.h"  // TransparentIndices
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
constexpr int kMapW = 20, kMapH = 18;  // 20×18 tiles cover the 160×144 viewport

struct ModeEntry {
    BlendMode   mode;
    const char* name;
};
constexpr std::array<ModeEntry, 6> kModes{{{BlendMode::Normal, "Normal (replace)"},
                                           {BlendMode::Multiply, "Multiply (shadow)"},
                                           {BlendMode::Add, "Add (glow)"},
                                           {BlendMode::Screen, "Screen (bloom)"},
                                           {BlendMode::Subtract, "Subtract"},
                                           {BlendMode::Half, "Half"}}};

enum class Action : std::uint8_t { CycleMode, Fullscreen };

// A 16×16 solid disc: index 1 inside radius 7, index 0 (a GameBoy hole) outside — a round, opaque sprite.
[[nodiscard]] std::array<std::uint8_t, 16 * 16> discArt() {
    std::array<std::uint8_t, 16 * 16> a{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const float dx = x - 7.5f, dy = y - 7.5f;
            a[static_cast<std::size_t>(y) * 16 + x] = (dx * dx + dy * dy <= 7.0f * 7.0f) ? 1 : 0;
        }
    return a;
}

// A 16×16 radial falloff: index 3 at the centre grading out to 1 at the rim, 0 (hole) beyond — three
// concentric bands. Paired with a dark-centre palette it is a Multiply shadow; with a bright-centre one an
// Add flare (the SAME art, the owning sprite's blend decides which).
[[nodiscard]] std::array<std::uint8_t, 16 * 16> softArt() {
    std::array<std::uint8_t, 16 * 16> a{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const float d = std::sqrt((x - 7.5f) * (x - 7.5f) + (y - 7.5f) * (y - 7.5f));
            std::uint8_t idx = 0;                         // outside → hole
            if (d < 2.5f)      idx = 3;                   // centre
            else if (d < 5.0f) idx = 2;                   // mid
            else if (d < 7.5f) idx = 1;                   // rim
            a[static_cast<std::size_t>(y) * 16 + x] = idx;
        }
    return a;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Sprite Blend Demo"},
        .window   = {.title = "Retro++ — sprite-blend demo (per-sprite shadow / flare / probe)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};

    ActionMap map{
        {Action::CycleMode, {SDL_SCANCODE_A, PadButton::FaceSouth}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // The opaque scene the sprites grade over: a soft four-tone diagonal pattern, muted and close in value so
    // the blends read against a calm backdrop rather than a busy one.
    std::array<std::uint8_t, 64> sceneArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            sceneArt[static_cast<std::size_t>(y) * 8 + x] = static_cast<std::uint8_t>(((x + y) / 2) % 4);
    const AtlasId sceneAtlas = renderer.uploadAtlas(sceneArt.data(), 8, 8);  // opaque (default None)
    const std::array<Rgba8, 4> scenePal{{{150, 158, 150}, {150, 150, 162}, {162, 156, 146}, {144, 156, 156}}};
    const PaletteId scenePalId = renderer.uploadPalette(std::span<const Rgba8>(scenePal));
    // The scene spans two viewports so the scroll wraps cleanly.
    const std::vector<TileCell> sceneCells(static_cast<std::size_t>(kMapW) * kMapH,
                                           TileCell{.atlas = sceneAtlas, .tile = 0, .palette = scenePalId});

    // Disc + soft sheets (index 0 = a GameBoy hole on both, so the round silhouette is the blend's mask).
    const auto disc = discArt();
    const auto soft = softArt();
    const AtlasId discAtlas = renderer.uploadAtlas(disc.data(), 16, 16, TransparentIndices::GameBoy);
    const AtlasId softAtlas = renderer.uploadAtlas(soft.data(), 16, 16, TransparentIndices::GameBoy);

    // Palettes. Disc index 1 is the body colour; soft indices 1..3 are the rim→centre falloff.
    const std::array<Rgba8, 2> heroPal{{{0, 0, 0}, {240, 240, 255}}};    // a bright opaque hero
    const std::array<Rgba8, 2> probePal{{{0, 0, 0}, {130, 170, 225}}};   // a mid blue probe body
    // Shadow (Multiply): darkest at the centre so Multiply bites hardest there, lightest at the rim.
    const std::array<Rgba8, 4> shadowPal{{{0, 0, 0}, {150, 150, 160}, {90, 90, 100}, {45, 45, 55}}};
    // Flare (Add): brightest at the centre so Add lifts hardest there, dim at the rim.
    const std::array<Rgba8, 4> flarePal{{{0, 0, 0}, {24, 18, 6}, {84, 66, 22}, {168, 132, 52}}};
    const PaletteId heroPalId   = renderer.uploadPalette(std::span<const Rgba8>(heroPal));
    const PaletteId probePalId  = renderer.uploadPalette(std::span<const Rgba8>(probePal));
    const PaletteId shadowPalId = renderer.uploadPalette(std::span<const Rgba8>(shadowPal));
    const PaletteId flarePalId  = renderer.uploadPalette(std::span<const Rgba8>(flarePal));

    int  modeIdx = 0;
    auto announce = [&]() {
        std::printf("sprite-blend demo — probe blend: %s. The hero carries a Multiply shadow and an Add "
                    "flare; the probe (right of centre) cycles every mode over the same scene. A = next "
                    "mode; Backspace = fullscreen; close to quit.\n",
                    kModes[static_cast<std::size_t>(modeIdx)].name);
    };
    announce();

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::CycleMode)) {
            modeIdx = (modeIdx + 1) % static_cast<int>(kModes.size());
            announce();
        }
        if (in.justPressed(Action::Fullscreen)) platform.fullscreen(!platform.fullscreen());
    });

    FrameDrawState      frame;
    std::vector<Sprite> sprites;
    loop.renderLoop([&]() {
        const double t = static_cast<double>(loop.tickCount());
        frame.layers.clear();

        // The opaque scene, drifting slowly sideways so the moving decals cross a little variety.
        DrawLayer scene{.key = "scene"};
        scene.z       = 0;
        scene.size    = PixelSize{kViewW, kViewH};
        scene.scroll  = LayerScroll{static_cast<int>(t) / 16 % (kMapW * 8), 0};
        scene.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                    .cells = std::span<const TileCell>(sceneCells)};
        frame.layers.push_back(scene);

        // The hero drifts gently left↔right with a shallow bob; its shadow/flare track it. Integer placement,
        // motion off the tick.
        const int heroX = 24 + static_cast<int>(44.0 + 44.0 * std::sin(t * 0.010));
        const int heroY = 64 + static_cast<int>(3.0 * std::sin(t * 0.024));
        const BlendMode probeMode = kModes[static_cast<std::size_t>(modeIdx)].mode;
        const float     flareA    = 0.5f;  // a steady glow

        sprites.clear();
        // Shadow decal — Multiply, below the hero (z 0), offset to its feet, translucent.
        sprites.push_back(Sprite{.key = "shadow", .x = heroX, .y = heroY + 10, .z = 0,
                                 .size = AssetDimensions::Snes16x16, .atlas = softAtlas, .tile = 0,
                                 .palette = shadowPalId, .alpha = 0.7f, .blend = BlendMode::Multiply});
        // Hero — opaque, Normal, over the shadow (z 1).
        sprites.push_back(Sprite{.key = "hero", .x = heroX, .y = heroY, .z = 1,
                                 .size = AssetDimensions::Snes16x16, .atlas = discAtlas, .tile = 0,
                                 .palette = heroPalId});
        // Flare — Add, above the hero (z 2), alpha pulsing.
        sprites.push_back(Sprite{.key = "flare", .x = heroX, .y = heroY - 8, .z = 2,
                                 .size = AssetDimensions::Snes16x16, .atlas = softAtlas, .tile = 0,
                                 .palette = flarePalId, .alpha = flareA, .blend = BlendMode::Add});
        // Probe — one disc at a fixed spot, cycling its blend so you can compare modes over the moving scene.
        sprites.push_back(Sprite{.key = "probe", .x = 112, .y = heroY, .z = 1,
                                 .size = AssetDimensions::Snes16x16, .atlas = discAtlas, .tile = 0,
                                 .palette = probePalId, .blend = probeMode});

        DrawLayer actors{.key = "actors"};
        actors.z       = 10;
        actors.size    = PixelSize{kViewW, kViewH};
        actors.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(actors);

        renderer.renderFrame(frame);
    });

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
