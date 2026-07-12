// Sprite-effects demo — a runnable host for `Sprite::effects` and `Sprite::regions`. Two 16×16 heroes sit
// side by side over a scrolling scene — a FLAT solid disc (left) and a TEXTURED shaded ball (right) —
// carrying the SAME effect stack, so each effect shows on flat and textured art at once (the sheen is the
// clearest contrast: it keys off the ball's luminance and rides flat over the disc). Toggle key by key:
//
//   • SHEEN (always on) — a `Gleam` chain effect whose `sweep` animates off the tick, so a luminance-keyed
//     highlight sweeps across the hero's bright body and fades at its rim. A whole-silhouette chain effect.
//   • FLASH (A) — a white `ColorFill` region over the whole silhouette, its `alpha` pulsing: a damage
//     flash. Empty region shape ⇒ the whole sprite.
//   • TINT (B) — a `Multiply` `ColorFill` region confined to a quad-space rectangle over the hero's lower
//     half: a shadow that darkens only part of the silhouette.
//   • HOLE (X) — a `Transparency` region over a quad-space circle at the hero's centre: the scene behind
//     shows through a punched hole.
//
// A third sprite below the heroes shows the DISPLACING kinds — a striped tile whose art is re-read at a
// shifted position:
//   • WAVE (W) — cycles the striped sprite off / RowDisplacement (the bars shear into a wave) / Ripple (they
//     push radially from the centre). The displacement is in the sprite's own art px; a crest pulled past the
//     static quad renders in the inflated footprint, and where the art is pulled aside the exposed strip is
//     transparent so the scrolling scene shows through.
//
// Effects evaluate inline in the sprite fragment — no extra passes. Parameters are per-tick data, so the
// flash pulse, the sheen sweep, and the wave phase are recomputed and resubmitted each tick (the interpolator
// never eases effect params). The pixel-exact math is the ctest suite's job (evalSpriteFxRecords /
// spriteDisplacedRead vs the shader, in sprite_effects_test.cpp); this is the live GPU sanity check + a
// teaching scene.
//
// Motion advances on the sim tick, so it runs the same on any display. A = flash, B = tint, X = hole,
// W = wave, Backspace = fullscreen; close to quit.

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
constexpr int kMapW = 20, kMapH = 18;

enum class Action : std::uint8_t { Flash, Tint, Hole, Wave, Fullscreen };

// A 16×16 solid disc: index 1 inside radius 7, index 0 (a GameBoy hole) outside — a round, FLAT sprite
// (one uniform colour, no internal texture).
[[nodiscard]] std::array<std::uint8_t, 16 * 16> discArt() {
    std::array<std::uint8_t, 16 * 16> a{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const float dx = x - 7.5f, dy = y - 7.5f;
            a[static_cast<std::size_t>(y) * 16 + x] = (dx * dx + dy * dy <= 7.0f * 7.0f) ? 1 : 0;
        }
    return a;
}

// A 16×16 shaded ball: a lit sphere with a highlight up-and-left (index 3 brightest → 1 darkest), index 0
// (a GameBoy hole) outside radius 7. The internal luminance banding is what a luminance-keyed effect like
// Gleam rides — the sheen catches the highlight and skips the shadowed rim, which a flat disc can't show.
[[nodiscard]] std::array<std::uint8_t, 16 * 16> shadedBallArt() {
    std::array<std::uint8_t, 16 * 16> a{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const float dx = x - 7.5f, dy = y - 7.5f;
            if (dx * dx + dy * dy > 7.0f * 7.0f) { a[static_cast<std::size_t>(y) * 16 + x] = 0; continue; }
            const float hd = std::sqrt((x - 5.0f) * (x - 5.0f) + (y - 5.0f) * (y - 5.0f));  // dist to highlight
            const std::uint8_t idx = hd < 3.0f ? 3 : hd < 6.5f ? 2 : 1;
            a[static_cast<std::size_t>(y) * 16 + x] = idx;
        }
    return a;
}

// A 16×16 opaque striped tile — alternating vertical bars (indices 1 and 2, every 2 px), no hole. A horizontal
// RowDisplacement shears the bars into a wave; a Ripple pushes them radially. Where a crest pulls the art
// aside, the exposed strip is transparent (the default Blank edge) so the scene shows through — the
// infinite-transparent-field domain the displacing kinds work in.
[[nodiscard]] std::array<std::uint8_t, 16 * 16> stripeArt() {
    std::array<std::uint8_t, 16 * 16> a{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            a[static_cast<std::size_t>(y) * 16 + x] = static_cast<std::uint8_t>(1 + (x / 2) % 2);
    return a;
}

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Sprite Effects Demo"},
        .window   = {.title = "Retro++ — sprite-effects demo (sheen / flash / tint / hole)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};

    ActionMap map{
        {Action::Flash, {SDL_SCANCODE_A, PadButton::FaceSouth}},
        {Action::Tint, {SDL_SCANCODE_B, PadButton::FaceEast}},
        {Action::Hole, {SDL_SCANCODE_X, PadButton::FaceWest}},
        {Action::Wave, {SDL_SCANCODE_W, PadButton::FaceNorth}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.setActions(map);

    // The opaque scene the sprite's hole reveals: four distinct jewel tones — clearly different hues so the
    // punched-through hole shows obvious colour, but deeper in value so a full screen of them is easy on the
    // eyes.
    std::array<std::uint8_t, 64> sceneArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            sceneArt[static_cast<std::size_t>(y) * 8 + x] = static_cast<std::uint8_t>(((x + y) / 2) % 4);
    const AtlasId sceneAtlas = renderer.uploadAtlas(sceneArt.data(), 8, 8);
    const std::array<Rgba8, 4> scenePal{{{170, 60, 80}, {45, 120, 130}, {180, 140, 55}, {95, 70, 150}}};
    const PaletteId scenePalId = renderer.uploadPalette(std::span<const Rgba8>(scenePal));
    const std::vector<TileCell> sceneCells(static_cast<std::size_t>(kMapW) * kMapH,
                                           TileCell{.atlas = sceneAtlas, .tile = 0, .palette = scenePalId});

    const auto disc = discArt();
    const auto ball = shadedBallArt();
    const AtlasId discAtlas = renderer.uploadAtlas(disc.data(), 16, 16, TransparentIndices::GameBoy);
    const AtlasId ballAtlas = renderer.uploadAtlas(ball.data(), 16, 16, TransparentIndices::GameBoy);
    const std::array<Rgba8, 2> heroPal{{{0, 0, 0}, {255, 150, 30}}};  // flat disc: one bright-orange body
    // Shaded ball: index 0 is the hole; 1→3 are dark rim → mid → bright highlight, a lit sphere's luminance.
    const std::array<Rgba8, 4> ballPal{{{0, 0, 0}, {140, 70, 15}, {215, 120, 35}, {255, 225, 175}}};
    const PaletteId heroPalId = renderer.uploadPalette(std::span<const Rgba8>(heroPal));
    const PaletteId ballPalId = renderer.uploadPalette(std::span<const Rgba8>(ballPal));

    const auto    stripe      = stripeArt();
    const AtlasId stripeAtlas = renderer.uploadAtlas(stripe.data(), 16, 16);  // opaque — no transparent index
    const std::array<Rgba8, 3> stripePal{{{0, 0, 0}, {40, 120, 160}, {170, 210, 230}}};  // idx 0 unused
    const PaletteId stripePalId = renderer.uploadPalette(std::span<const Rgba8>(stripePal));

    bool flash = false, tint = false;
    int  holeMode = 0;  // 0 = off, 1 = TransparentInside (a hole), 2 = TransparentOutside (a porthole)
    int  waveMode = 1;  // 0 = off, 1 = RowDisplacement, 2 = Ripple (on the striped sprite)
    auto holeLabel = [&]() {
        return holeMode == 1 ? "hole (inside)" : holeMode == 2 ? "porthole (outside)" : "off";
    };
    auto waveLabel = [&]() {
        return waveMode == 1 ? "row-displace" : waveMode == 2 ? "ripple" : "off";
    };
    auto announce = [&]() {
        std::printf("sprite-effects demo — sheen always on; flash %s, tint %s, hole %s, wave %s. A = flash, "
                    "B = tint, X = cycle hole (off/inside/outside), W = cycle wave (off/row/ripple), "
                    "Backspace = fullscreen; close to quit.\n",
                    flash ? "ON" : "off", tint ? "ON" : "off", holeLabel(), waveLabel());
    };
    announce();

    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Action::Flash)) { flash = !flash; announce(); }
        if (in.justPressed(Action::Tint))  { tint = !tint;   announce(); }
        if (in.justPressed(Action::Hole))  { holeMode = (holeMode + 1) % 3; announce(); }
        if (in.justPressed(Action::Wave))  { waveMode = (waveMode + 1) % 3; announce(); }
        if (in.justPressed(Action::Fullscreen)) platform.setFullscreen(!platform.isFullscreen());
    });

    FrameDrawState      frame;
    std::vector<Sprite> sprites;
    std::vector<Region> heroRegions;
    loop.setRender([&]() {
        const double t = static_cast<double>(loop.tickCount());
        frame.layers.clear();

        DrawLayer scene{.key = "scene"};
        scene.z       = 0;
        scene.size    = PixelSize{kViewW, kViewH};
        scene.scroll  = LayerScroll{static_cast<int>(t) / 16 % (kMapW * 8), 0};
        scene.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                    .cells = std::span<const TileCell>(sceneCells)};
        frame.layers.push_back(scene);

        const int heroY = 64 + static_cast<int>(4.0 * std::sin(t * 0.024));  // a shared gentle vertical bob

        // The shared effect stack — built once, applied to BOTH sprites so each toggle shows on flat AND
        // textured art at once. A Gleam sheen whose crest sweeps across the sprite (sweep animates off the
        // tick); the scrolling scene behind provides the motion the hole reveals.
        const float sweep = static_cast<float>(0.5 + 0.5 * std::sin(t * 0.03));
        const std::vector<ScreenSpaceEffect> sheen = {
            ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam, .sweep = sweep, .width = 0.5f, .gain = 1.6f}};

        heroRegions.clear();
        if (flash) {  // a white damage flash over the whole silhouette, alpha pulsing
            const float a = static_cast<float>(0.4 + 0.4 * std::sin(t * 0.12));
            heroRegions.push_back(Region{.key = "flash",
                                         .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                                       .fill = Rgba8{255, 255, 255, 255}}},
                                         .alpha = a});
        }
        if (tint) {  // a Multiply shadow on the lower half (quad-space rectangle y ∈ [8,16] of a 16px sprite)
            heroRegions.push_back(Region{.key = "tint",
                                         .shape = ShapePoints::rectangle(Point{0, 8}, 16, 8),
                                         .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                                       .fill = Rgba8{90, 90, 130, 255}}},
                                         .blend = BlendMode::Multiply});
        }
        if (holeMode != 0) {  // a see-through circle at the centre (quad-space); the scene shows through it
            const StencilMode mode = holeMode == 1 ? StencilMode::TransparentInside   // punch the circle out
                                                   : StencilMode::TransparentOutside;  // keep only the circle
            heroRegions.push_back(Region{.key = "hole",
                                         .shape = ShapePoints::circle(Point{8, 8}, 3.5f),
                                         .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Transparency,
                                                                       .stencil = mode}}});
        }

        // FLAT disc (left) and TEXTURED shaded ball (right), same effects on each. Keys are stable + unique.
        auto styled = [&](const char* key, int x, AtlasId atlas, PaletteId pal) {
            Sprite s{.key = key, .x = x, .y = heroY, .size = AssetDimensions::Snes16x16,
                     .atlas = atlas, .tile = 0, .palette = pal};
            s.effects = sheen;
            s.regions = heroRegions;
            return s;
        };
        sprites.clear();
        sprites.push_back(styled("flat", 40, discAtlas, heroPalId));
        sprites.push_back(styled("ball", 96, ballAtlas, ballPalId));

        // The displacing striped sprite below — its art is re-read at a shifted within-sprite position; the
        // phase advances off the tick to animate. RowDisplacement shears the bars into a wave; Ripple pushes
        // them radially from the centre.
        if (waveMode != 0) {
            const float phase = static_cast<float>(t * 0.02);
            const ScreenSpaceEffect disp =
                waveMode == 1
                    ? ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 3.0f,
                                        .frequency = 2.0f, .phase = phase, .axis = Axis::Horizontal}
                    : ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = 3.0f,
                                        .frequency = 3.0f, .phase = phase, .center = Point{8, 8}, .decay = 1.2f};
            Sprite water{.key = "water", .x = 72, .y = 108, .size = AssetDimensions::Snes16x16,
                         .atlas = stripeAtlas, .tile = 0, .palette = stripePalId};
            water.effects = {disp};
            sprites.push_back(water);
        }
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
