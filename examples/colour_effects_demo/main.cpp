// Colour-effects demo — a runnable host that VISUALLY proves the whole-frame colour looks a retro game
// reaches for (day/night, a cutscene flash, a fade, a glow, a cast shadow, a sunlit highlight) are ordinary
// screen-space effects: a ColorFill paired with a blend mode and an alpha / fillIntensity, composited at
// whatever scope the look wants. There is no bespoke "colour modifier" or "flash" member on the frame —
// colour is the effect system, same as a ripple or a row-displacement.
//
// It draws a small blocky landscape — a vertical sky→ground gradient with a sun and two trees, all tiles —
// and grades it with effects that each read as a real lighting phenomenon:
//
//   • DAY / NIGHT (whole-frame) — a Multiply-blended ColorFill region covering the viewport, drifting very
//     slowly between a bright, slightly overexposed midday and a dim cool night. At midday its white fill
//     runs fillIntensity > 1, so Multiply LIFTS the whole scene above its base — a multiplicative exposure
//     (at 8-bit a Multiply day could only reach "no change"); at night it cools and dims. The always-on grade.
//   • SUN GLOW (per-region, Multiply at fillIntensity > 1) — a warm ColorFill disc over the sun: a
//     multiplicative bloom that lifts the sun above white while keeping its core's shape, brightest at midday.
//   • TREE SHADOW (background-LAYER region, Multiply) — a dark ColorFill oval pooled at the left tree's
//     trunk base, on the BACKGROUND layer so the tree (a layer above) draws over it: a real cast shadow,
//     below the tree and above the grass.
//   • SUNBEAM (per-region, Add) — a warm ColorFill wedge (a triangle) from the sun down to the ground: Add
//     lifts whatever it crosses, so it reads as a shaft of light.
//   • FLASH (whole-frame, press A) — a Normal-blended white ColorFill region whose alpha ramps gently up and
//     back down: lerp(scene, white, strength), exactly the cutscene flash, as an effect.
//   • FADE (whole-frame, press B) — a Normal-blended black ColorFill region whose alpha dips to black and
//     eases back: a fade out-and-in, as an effect. (Both flash and fade self-return; neither sticks.)
//
// Photosensitivity: the day/night drift — and the midday brightening and sun bloom that ride it — is
// extremely slow (~40 s per cycle); the exposure eases up and back over many seconds, never a flash. The
// cutscene flash is a gentle, capped (0.45), ~1.5 s key-triggered ramp — never full white, never fast, never
// automatic; the fade eases over ~3 s; the scene, shadow, and sunbeam are static. Nothing strobes. The window
// never auto-launches (a dev drives it). A = flash, B = fade, Select = fullscreen; close to quit.

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <utility>
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
constexpr int kMapW = 20, kMapH = 18;     // 20×18 tiles cover the 160×144 viewport
constexpr int kGroundRow = 12;            // rows [kGroundRow, kMapH) are ground; above is sky

constexpr float kPi = 3.14159265358979323846f;

// The atlas is one solid tile per palette colour: tile t is 8×8 of palette index t, so a cell picks its
// colour by selecting a TILE (the proven scene pattern). Tiles / palette indices:
//   1..8 sky (deep top → light horizon) · 9 ground near · 10 ground far · 11 sun · 12 sun core ·
//   13 tree canopy · 14 trunk   (0 is unused/black)
constexpr std::uint16_t kSky0 = 1, kGroundNear = 9, kGroundFar = 10, kSun = 11, kSunCore = 12,
                        kCanopy = 13, kTrunk = 14;
constexpr int kColours = 15;

[[nodiscard]] std::uint8_t u8(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<std::uint8_t>(c * 255.0f + 0.5f);
}

// A whole-viewport region (no shape) carrying one ColorFill, composited over the scene with `mode` at
// `alpha`. The building block for every whole-frame colour look: day/night (Multiply), flash/fade (Normal).
// `fillIntensity` scales the fill past 1 (default 1) — a Multiply fill above 1 brightens the scene.
[[nodiscard]] Region wholeFrameFill(Rgba8 colour, BlendMode mode, float alpha = 1.0f,
                                    float fillIntensity = 1.0f) {
    return Region{.effects = {ScreenSpaceEffect{.kind          = ScreenSpaceEffectKind::ColorFill,
                                                .fill          = colour,
                                                .fillIntensity = fillIntensity}},
                  .alpha = alpha,
                  .blend = mode};
}

// A region carrying one ColorFill over `shape`, composited with `mode`. The in-scene lighting: a glow disc
// (Multiply at fillIntensity > 1 → a multiplicative bloom), a sunbeam wedge (Add → brighten), a shadow oval
// (Multiply → darken). `fillIntensity` scales the fill past 1 (default 1).
[[nodiscard]] Region shapedFill(ShapePoints shape, Rgba8 colour, BlendMode mode,
                                float fillIntensity = 1.0f) {
    return Region{.shape   = std::move(shape),
                  .effects = {ScreenSpaceEffect{.kind          = ScreenSpaceEffectKind::ColorFill,
                                                .fill          = colour,
                                                .fillIntensity = fillIntensity}},
                  .blend = mode};
}

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{.window = {.title = "Retro++ — colour-effects demo (day/night · flash · fade · glow · shadow · highlight, all effects)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    renderer.setSamplingMode(config.enhancements.sampling);

    // Atlas: kColours tiles, tile t = 8×8 of index t. One multi-colour palette holds the scene colours; a
    // cell selects its colour by tile (palette select stays 0). This is the standard scene pattern.
    std::array<std::uint8_t, static_cast<std::size_t>(kColours) * 8 * 8> atlasPx{};
    for (int t = 0; t < kColours; ++t)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                atlasPx[(static_cast<std::size_t>(y) * kColours + t) * 8 + x] = static_cast<std::uint8_t>(t);
    // Two uploads of the same art: opaque (the background) and with index 0 declared transparent (the tree
    // layer's non-tree cells, tile 0, become holes so the background shows through).
    const AtlasId atlas     = renderer.uploadAtlas(atlasPx.data(), kColours * 8, 8);
    const AtlasId holeAtlas = renderer.uploadAtlas(atlasPx.data(), kColours * 8, 8, /*transparentIndex=*/0);

    const std::array<Rgba8, kColours> palette{{
        {0, 0, 0},                                                        // 0 transparent marker (never shown)
        {28, 40, 92}, {40, 58, 120}, {54, 80, 150}, {70, 104, 176},       // 1..4 sky deep → mid
        {92, 132, 196}, {120, 160, 212}, {150, 186, 226}, {184, 208, 236},// 5..8 sky mid → light horizon
        {74, 150, 82}, {52, 112, 64},                                     // 9 ground near, 10 ground far
        {255, 214, 96}, {255, 240, 170},                                  // 11 sun, 12 sun core
        {44, 104, 56}, {120, 80, 46},                                     // 13 canopy, 14 trunk
    }};
    const PaletteId pal = renderer.uploadPalette(std::span<const Rgba8>(palette));
    const std::array<PaletteId, 1> palSet{pal};

    // Two tilemaps so the tree shadow sits BELOW the trees and ABOVE the grass: the BACKGROUND (sky, ground,
    // sun) on one layer; the TREES on a layer above it (transparent elsewhere). The shadow is a per-layer
    // region on the background, so the tree layer draws over it.
    std::vector<TileCell> bgCells(static_cast<std::size_t>(kMapW) * kMapH);
    std::vector<TileCell> treeCells(static_cast<std::size_t>(kMapW) * kMapH);  // default tile 0 = transparent
    const auto bgPut = [&](int x, int y, std::uint16_t tile) {
        if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return;
        bgCells[static_cast<std::size_t>(y) * kMapW + x] = TileCell{.tile = tile, .palette = 0};
    };
    const auto treePut = [&](int x, int y, std::uint16_t tile) {
        if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return;
        treeCells[static_cast<std::size_t>(y) * kMapW + x] = TileCell{.tile = tile, .palette = 0};
    };
    // Background: sky gradient (deep top → light horizon), ground, and the sun (a filled disc + a 2×2 core).
    for (int y = 0; y < kMapH; ++y)
        for (int x = 0; x < kMapW; ++x)
            bgPut(x, y, y < kGroundRow ? static_cast<std::uint16_t>(kSky0 + (y * 7) / (kGroundRow - 1))
                                       : (y < kGroundRow + 2 ? kGroundFar : kGroundNear));
    constexpr int sunCx = 16, sunCy = 4;  // centre cell → px (132, 36)
    for (int y = sunCy - 2; y <= sunCy + 2; ++y)
        for (int x = sunCx - 2; x <= sunCx + 2; ++x)
            if ((x - sunCx) * (x - sunCx) + (y - sunCy) * (y - sunCy) <= 5) bgPut(x, y, kSun);
    for (int y = sunCy - 1; y <= sunCy; ++y)
        for (int x = sunCx - 1; x <= sunCx; ++x) bgPut(x, y, kSunCore);
    // Trees on their own layer: a trunk cell + a small canopy blob above it.
    for (const int bx : {4, 11}) {
        treePut(bx, kGroundRow, kTrunk);
        treePut(bx, kGroundRow - 1, kCanopy);
        treePut(bx - 1, kGroundRow - 1, kCanopy); treePut(bx + 1, kGroundRow - 1, kCanopy);
        treePut(bx, kGroundRow - 2, kCanopy);     treePut(bx, kGroundRow - 3, kCanopy);
    }

    // Flash and fade are both TRANSIENT one-shots (self-returning, never stuck): a press starts an age that
    // runs an envelope to 0 and stops.
    int flashAge = -1;  constexpr int kFlashTicks = 90;   // ~1.5 s: white up-and-back
    int fadeAge  = -1;  constexpr int kFadeTicks  = 180;  // ~3 s: black down-and-back

    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::Select)) platform.setFullscreen(!platform.isFullscreen());
        if (in.justPressed(Button::A)) flashAge = 0;  // (re)trigger the flash
        if (in.justPressed(Button::B)) fadeAge  = 0;  // (re)trigger the fade
        if (flashAge >= 0 && ++flashAge > kFlashTicks) flashAge = -1;
        if (fadeAge  >= 0 && ++fadeAge  > kFadeTicks)  fadeAge  = -1;
    });

    FrameDrawState frame;
    int            tick = 0;
    loop.setRender([&](float alpha) {
        frame.layers.clear();
        frame.regions.clear();

        // BACKGROUND layer (sky + ground + sun). Its per-layer region is the TREE SHADOW — a Multiply oval
        // pooled at the left tree's trunk base. Being on this layer (below the trees, on the grass), the tree
        // layer draws OVER it, so it reads as a shadow the tree casts, not an overlay on top of the tree.
        DrawLayer bg{};
        bg.id      = "background";
        bg.z       = 0;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{atlas, std::span<const PaletteId>(palSet),
                                 kMapW, kMapH, std::span<const TileCell>(bgCells)};
        bg.regions.push_back(shapedFill(ShapePoints::capsule(Point{30, 106}, Point{44, 106}, 5.0f),
                                        Rgba8{110, 115, 140}, BlendMode::Multiply));
        frame.layers.push_back(bg);

        // TREES layer (transparent elsewhere) — drawn above the background + its shadow.
        DrawLayer trees{};
        trees.id      = "trees";
        trees.z       = 10;
        trees.size    = PixelSize{kViewW, kViewH};
        trees.content = TileContent{holeAtlas, std::span<const PaletteId>(palSet),
                                    kMapW, kMapH, std::span<const TileCell>(treeCells)};
        frame.layers.push_back(trees);

        // DAY / NIGHT (whole-frame, always on): a Multiply ColorFill region over the composited scene,
        // drifting very slowly (~40 s) between a bright midday and a dim cool night. At midday the white fill
        // runs fillIntensity > 1, so Multiply LIFTS the whole scene above its base — a multiplicative exposure
        // (at 8-bit the fill clamps to 1 and the best a Multiply day could reach is "no change"); at night the
        // fill cools and dims toward a Multiply tint, exposure back at 1.
        const float night    = 0.5f + 0.5f * std::sin(static_cast<float>(tick) * 0.0026f);  // 0..1, very slow
        const float day      = 1.0f - night;                                                // 1 midday, 0 midnight
        const Rgba8 dayNight{u8(1.0f - 0.55f * night), u8(1.0f - 0.45f * night), u8(1.0f - 0.18f * night), 255};
        const float exposure = 1.0f + 0.35f * day;  // 1.0 (night) → 1.35 (midday), a slow brighten/ease
        frame.regions.push_back(wholeFrameFill(dayNight, BlendMode::Multiply, 1.0f, exposure));

        // FRAME-LEVEL LIGHTING (over everything), each a natural phenomenon:
        // Sun glow: a Multiply disc over the sun at fillIntensity > 1 — a multiplicative bloom that lifts the
        // sun above white while keeping its core's shape (an Add disc washes flat; this exposure preserves
        // contrast). Brightest at midday, easing back with the day factor.
        frame.regions.push_back(shapedFill(ShapePoints::circle(Point{132, 36}, 22.0f),
                                           Rgba8{255, 225, 180}, BlendMode::Multiply, 1.0f + 0.8f * day));
        // Sunbeam: an Add wedge (a triangle widening from just under the sun down to the ground) → a warm
        // shaft of light lifting whatever it crosses.
        frame.regions.push_back(shapedFill(ShapePoints::triangle(Point{126, 50}, Point{86, 134}, Point{112, 134}),
                                           Rgba8{120, 95, 45}, BlendMode::Add));

        // FLASH (whole-frame, A): a Normal white ColorFill whose alpha rises then falls — lerp(scene, white,
        // strength), the cutscene flash as an effect. Gentle and capped (photosensitivity).
        if (flashAge >= 0) {
            const float s = 0.45f * std::sin(kPi * static_cast<float>(flashAge) / static_cast<float>(kFlashTicks));
            frame.regions.push_back(wholeFrameFill(Rgba8{255, 255, 255}, BlendMode::Normal, s));
        }

        // FADE (whole-frame, B): a Normal black ColorFill whose alpha dips to black and eases back — a fade
        // out-and-in, as an effect.
        if (fadeAge >= 0) {
            const float s = 0.85f * std::sin(kPi * static_cast<float>(fadeAge) / static_cast<float>(kFadeTicks));
            frame.regions.push_back(wholeFrameFill(Rgba8{0, 0, 0}, BlendMode::Normal, s));
        }

        renderer.renderFrame(frame, alpha);
        ++tick;
    });

    std::printf("colour-effects demo — a sky→ground landscape graded by effects: a slow day/night Multiply "
                "that brightens past base at midday (fillIntensity > 1), a sun glow (Multiply exposure bloom), "
                "a tree shadow (Multiply), a sunbeam (Add), and whole-frame flash + fade — all ColorFill + a "
                "blend mode + alpha / fillIntensity.\n");
    std::printf("[dev] A = flash (white, ~1.5 s), B = fade (black, ~3 s), Select = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
