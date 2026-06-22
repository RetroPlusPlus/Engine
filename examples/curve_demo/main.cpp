// Curve demo — a runnable host that VISUALLY proves the Curve primitive's queries. It opens a window and
// plots, with small palette-coloured sprite markers (a tiny in-memory atlas — no committed asset), three
// authored curves over a dim background grid in the 160×144 viewport:
//
//   • a CATMULL-ROM curve through gold waypoints (the curve passes THROUGH every waypoint), with cyan
//     dots sampled by Curve::at(t) tracing the smooth shape, a PINK walker advanced by
//     Curve::atDistance(s) at CONSTANT SPEED (even spacing — it does not lurch through the control
//     clusters a raw-parameter walk would), and GREEN tangent ticks from Curve::tangentAtDistance — the
//     facing at the walker's own arc-length point.
//   • a HERMITE curve that LEAVES and ARRIVES along its supplied tangents.
//   • a funky CLOSED curve (magenta) whose interior is a curve-DEFINED REGION: its boundary is sampled
//     into the polygon the engine's region gate consumes, and a gentle radial ripple is confined to it,
//     so the background shimmers only inside the curve's outline. (The ripple effect itself is untouched;
//     the engine confines it to the shape — the curve just supplies the shape.) B toggles the ripple
//     between the curve region and the whole frame so the confinement is unmistakable.
//
// This is the visual sanity check for a pure-CPU primitive — the device-free ctest suite is the real
// gate. Photosensitivity: the walker drifts slowly and wraps, the ripple swells gently; nothing strobes
// or flashes; the window never auto-launches (a dev drives it). A restarts the walker; close to quit.

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main.
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
#include "retropp/curve.h"
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
constexpr int kMapW = 20, kMapH = 18;   // 20×18 tiles cover the 160×144 viewport

// Palette-select indices into the marker layer's palette set (built below in the same order).
enum Pal : std::uint8_t { kSample = 0, kWaypoint = 1, kWalker = 2, kTangent = 3, kBlob = 4 };

// One centred 8×8 marker at (x, y), selecting palette `pal`.
Sprite marker(float x, float y, std::uint8_t pal) {
    Sprite s{};
    s.x       = static_cast<int>(std::lround(x)) - 4;  // centre the 8×8 cell on the point
    s.y       = static_cast<int>(std::lround(y)) - 4;
    s.size    = AssetDimensions::GameBoy8x8;
    s.tile    = 0;
    s.palette = pal;
    return s;
}

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{.window = {.title = "Retro++ — curve demo (curves + curve-defined region)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    renderer.setSamplingMode(config.enhancements.sampling);

    // A tiny 8×8 marker atlas: every texel is palette-index 1 (a solid square); the colour comes from the
    // selected palette's entry [1]. (Atlas index 0 would be the OBJ-transparent hole; we use index 1.)
    std::array<std::uint8_t, 64> markerArt{};
    markerArt.fill(1);
    const AtlasId markerAtlas = renderer.uploadAtlas(markerArt.data(), 8, 8);

    // A dim 8×8 background grid tile (index 2 on the cell border, index 1 inside) so the ripple has
    // something to displace; kept low-contrast so the bright curve markers still read on top.
    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8);

    // One palette per marker role; entry [1] is the visible colour (entry [0] is unused — never sampled).
    const std::array<Rgba8, 2> samplePal{{{0, 0, 0}, {90, 200, 255}}};   // cyan — Curve::at samples
    const std::array<Rgba8, 2> wayPal{{{0, 0, 0}, {255, 210, 90}}};      // gold — waypoints
    const std::array<Rgba8, 2> walkPal{{{0, 0, 0}, {255, 90, 120}}};     // pink — constant-speed walker
    const std::array<Rgba8, 2> tanPal{{{0, 0, 0}, {150, 255, 150}}};     // green — tangent ticks
    const std::array<Rgba8, 2> blobPal{{{0, 0, 0}, {235, 120, 255}}};    // magenta — funky region outline
    const std::array<PaletteId, 5> markerSet{
        renderer.uploadPalette(std::span<const Rgba8>(samplePal)),
        renderer.uploadPalette(std::span<const Rgba8>(wayPal)),
        renderer.uploadPalette(std::span<const Rgba8>(walkPal)),
        renderer.uploadPalette(std::span<const Rgba8>(tanPal)),
        renderer.uploadPalette(std::span<const Rgba8>(blobPal))};

    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {20, 26, 40}, {40, 52, 78}}};  // dim navy grid
    const PaletteId            gridPalId = renderer.uploadPalette(std::span<const Rgba8>(gridPal));
    const std::array<PaletteId, 1> gridSet{gridPalId};
    const std::vector<TileCell>    gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                             TileCell{.tile = 0, .palette = 0});

    // ── The three authored curves ────────────────────────────────────────────────────────────────
    const std::array<Vec2, 5> waypoints{{{18, 34}, {54, 20}, {92, 44}, {128, 22}, {150, 40}}};
    const Curve catmull = Curve::throughPoints(std::span<const Vec2>(waypoints), false);
    const Curve hermiteC =
        Curve::hermite(Vec2{14, 122}, Vec2{40, -54}, Vec2{74, 122}, Vec2{40, 54});

    // A funky CLOSED curve — a lumpy blob. Catmull-Rom through wobbly points, wrapped into a loop, so the
    // boundary is genuinely curved (not a straight-edged polygon).
    const std::array<Vec2, 6> blobPts{{{116, 70}, {136, 86}, {128, 110}, {106, 118}, {90, 100}, {100, 78}}};
    const Curve blob = Curve::throughPoints(std::span<const Vec2>(blobPts), true);

    // Sample the closed curve's boundary into the polygon the engine's region gate consumes (≤ 64
    // vertices). The region is therefore CURVE-DEFINED: its outline is the curve, sampled. Built once —
    // the blob does not change.
    constexpr int      kRegionSamples = 48;
    std::vector<Point> blobRegionPts;
    blobRegionPts.reserve(kRegionSamples);
    for (int i = 0; i < kRegionSamples; ++i) {
        const Vec2 p = blob.at(static_cast<float>(i) / static_cast<float>(kRegionSamples));
        blobRegionPts.push_back(Point{p.x, p.y});
    }
    const ShapePoints blobRegion{.points = blobRegionPts};

    // Bake the catmull curve's arc-length table ONCE — the walker queries it every frame with no
    // per-call resample (the reuse path; the Curve's own atDistance would rebuild the table each call).
    const ArcLengthTable catmullArc   = catmull.arcTable();
    const float          walkerLength = catmullArc.length();
    float                walkerDist   = 0.0f;  // arc-length cursor; advances at constant speed, wraps

    // B cycles the ripple: 0 = confined to the curve region, 1 = whole frame, 2 = OFF (no effect at all —
    // the raw scene, so the magenta outline reads as a hollow ring over plain grid, nothing filled inside).
    int        rippleMode = 0;
    const auto rippleLabel = [](int m) {
        return m == 0 ? "confined to the curve region" : (m == 1 ? "whole frame" : "OFF (raw scene)");
    };

    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::A)) {
            walkerDist = 0.0f;
            std::printf("[dev] walker restarted\n");
        }
        if (in.justPressed(Button::B)) {
            rippleMode = (rippleMode + 1) % 3;
            std::printf("[dev] ripple: %s\n", rippleLabel(rippleMode));
        }
        if (in.justPressed(Button::Select)) platform.setFullscreen(!platform.isFullscreen());
        // ~14 px/s along the arc (slow, monotonic) at 59.7275 Hz — photosensitivity-safe.
        walkerDist += 0.24f;
        if (walkerLength > 0.0f && walkerDist > walkerLength) walkerDist -= walkerLength;
    });

    std::vector<Sprite> sprites;
    FrameDrawState      frame;
    int                 tick = 0;
    loop.setRender([&](float alpha) {
        sprites.clear();

        // Catmull-Rom: Curve::at samples (cyan) + waypoints (gold).
        constexpr int kSamples = 48;
        for (int i = 0; i <= kSamples; ++i) {
            const Vec2 p = catmull.at(static_cast<float>(i) / static_cast<float>(kSamples));
            sprites.push_back(marker(p.x, p.y, kSample));
        }
        for (const Vec2& w : waypoints) sprites.push_back(marker(w.x, w.y, kWaypoint));

        // Hermite curve sampled (cyan) — its ends leave/arrive along the supplied tangents.
        for (int i = 0; i <= kSamples; ++i) {
            const Vec2 p = hermiteC.at(static_cast<float>(i) / static_cast<float>(kSamples));
            sprites.push_back(marker(p.x, p.y, kSample));
        }

        // The funky closed curve's outline (magenta) — the boundary of the curve-defined region below.
        for (int i = 0; i < kRegionSamples; ++i) {
            const Vec2 p = blob.at(static_cast<float>(i) / static_cast<float>(kRegionSamples));
            sprites.push_back(marker(p.x, p.y, kBlob));
        }

        // The constant-speed walker (pink) + a few tangent ticks (green) ahead of it along travel. The
        // facing comes from tangentAtDistance — the heading at the SAME arc-length point as the walker,
        // not tangent(s / length), which would read a different point on this non-uniform curve.
        const Vec2 here = catmullArc.atDistance(walkerDist);
        const Vec2 dir  = catmullArc.tangentAtDistance(walkerDist);
        sprites.push_back(marker(here.x, here.y, kWalker));
        for (int k = 1; k <= 3; ++k) {
            sprites.push_back(marker(here.x + dir.x * (4.0f * k), here.y + dir.y * (4.0f * k), kTangent));
        }

        frame.layers.clear();
        DrawLayer bg{};
        bg.id      = "backgroundGrid";
        bg.z       = -10;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{gridAtlas, std::span<const PaletteId>(gridSet),
                                 kMapW, kMapH, std::span<const TileCell>(gridCells)};
        frame.layers.push_back(bg);

        DrawLayer markers{};
        markers.id      = "curveMarkers";
        markers.z       = 0;
        markers.size    = PixelSize{kViewW, kViewH};
        markers.content = SpriteContent{markerAtlas, std::span<const PaletteId>(markerSet),
                                        std::span<const Sprite>(sprites)};
        frame.layers.push_back(std::move(markers));

        // A gentle radial ripple. Mode 0 confines it to the CURVE-DEFINED region (the sampled blob
        // boundary) — the engine masks the effect to the shape, the curve supplies the shape. Mode 1 drops
        // the region so the SAME effect runs whole-frame. Mode 2 emits no effect at all: the raw scene, so
        // the magenta outline is plainly a hollow ring over flat grid — nothing is filled inside it.
        frame.postEffects.clear();
        if (rippleMode != 2) {
            ScreenSpaceEffect ripple{
                .kind      = ScreenSpaceEffectKind::Ripple,
                .amplitude = 3.5f,
                .frequency = 5.0f,
                .phase     = static_cast<float>(tick) * 0.01f,  // ~0.6 cycles/s — rings drift out slowly
                .center    = Point{113, 92},                     // roughly the blob's centre
                .decay     = 2.0f};
            if (rippleMode == 0) ripple.region = blobRegion;  // SAME effect, now bounded by the curve
            frame.postEffects.push_back(ripple);
        }

        renderer.renderFrame(frame, alpha);
        ++tick;
    });

    std::printf("curve demo — gold = waypoints, cyan = Curve::at samples, pink = the constant-speed "
                "atDistance walker, green = tangentAtDistance ticks, magenta = a closed curve whose "
                "interior ripples (a curve-defined region). A restarts the walker, B cycles the ripple "
                "(curve region → whole frame → OFF), Select = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
