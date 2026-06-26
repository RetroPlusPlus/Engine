// Curve-mask region demo — what this feature is, and how to SEE it.
//
// THE PROBLEM. An effect can be confined to a shape (a "region"): a ripple, a colour fill, or a see-through
// hole that only happens inside the shape. For a circle or a polygon that confinement is easy. For a smooth,
// wavy, hand-curved outline (a "cubic" / Catmull-Rom curve) it is NOT: the GPU has no quick formula for "how
// far is this pixel from that curve?", so without help the engine falls back to tracing the curve with a few
// straight lines — a faceted, low-poly approximation.
//
// THE FEATURE. bakeCurveMask / bakeCurveRegion solves it by precomputing, ONE TIME, a tiny picture of the
// shape called a signed-distance field (an "SDF mask"): for every spot it stores how far that spot is from
// the curve's edge, and whether it is inside (negative) or outside (positive). The GPU then just looks up
// that picture per pixel — fast — and the confinement edge follows the TRUE curve exactly, no facets, at any
// size. The bake is done once at setup and reused every frame; moving or rotating the region just looks up a
// different spot in the same picture (no re-bake).
//
// WHAT YOU SEE. Two copies of the SAME wavy blob, side by side:
//   • LEFT  — confined by the baked MASK. The filled edge is SMOOTH — it hugs the curve.
//   • RIGHT — confined by the old straight-line approximation. The filled edge is JAGGED / faceted.
// The TRUE curve is drawn as a thin MAGENTA outline over BOTH. Read the demo by comparing each filled edge to
// its magenta line: on the left the fill sits right on the magenta; on the right the fill cuts across it in
// flat segments. Same shape, same fill colour — only how the boundary is computed differs. That difference IS
// the feature.
//
//   B      — right side: toggle between coarse (obvious facets) and fine (almost smooth) approximation, so
//            you can watch the right edge get blockier or rounder while the left stays perfectly smooth.
//   A      — slowly SPIN both regions. The LEFT (masked) blob rotates yet its edge stays perfectly smooth —
//            the one baked mask is reused under the rotation with NO re-bake (the right polygon re-facets).
//   Start  — turn the LEFT fill into a see-through HOLE (a teal layer shows through), proving the same baked
//            mask also drives the curved "cut a hole in this shape" path, not just colour fills.
//   Select — fullscreen. Close to quit.
//
// The maths (does the baked field match the true curve?) is checked by the device-free ctest suite
// (bakeCurveMaskField / sampleCurveMaskField vs Curve::signedDistance); this window is the live on-GPU
// sanity check. Photosensitivity: the scene is STATIC (a fill animates nothing) and never strobes; the
// window never auto-launches — a developer drives it.

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
#include "retropp/transform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;  // 20×18 tiles cover the 160×144 viewport

// A closed wavy blob through `n` points on a circle about (cx,cy) — a Catmull-Rom (cubic) loop. Cubic has no
// closed-form GPU distance, so this is exactly the boundary the baked mask exists to evaluate.
[[nodiscard]] Curve blob(float cx, float cy, float r, int n) {
    std::vector<Vec2> pts;
    pts.reserve(static_cast<std::size_t>(n));
    constexpr float kTwoPi = 6.283185307179586f;
    for (int i = 0; i < n; ++i) {
        const float a   = kTwoPi * static_cast<float>(i) / static_cast<float>(n);
        // A gentle in/out wobble so the shape is obviously curvy (not a plain circle), highlighting the win.
        const float rad = r * (i % 2 == 0 ? 1.0f : 0.78f);
        pts.push_back(Vec2{cx + rad * std::cos(a), cy + rad * std::sin(a)});
    }
    return Curve::throughPoints(std::span<const Vec2>(pts), /*closed=*/true);
}

// One centred 8×8 marker at (x, y), drawing from `atlas` through `palette`.
[[nodiscard]] Sprite marker(float x, float y, AtlasId atlas, PaletteId palette) {
    Sprite s{};
    s.x       = static_cast<int>(std::lround(x)) - 4;
    s.y       = static_cast<int>(std::lround(y)) - 4;
    s.size    = AssetDimensions::GameBoy8x8;
    s.tile    = 0;
    s.atlas   = atlas;
    s.palette = palette;
    return s;
}

enum Pal : std::uint8_t { kOutline = 0, kVertex = 1 };

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{.window = {.title = "Retro++ — curve-mask region demo (exact cubic vs faceted)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    renderer.setSamplingMode(config.enhancements.sampling);

    // A tiny 8×8 marker atlas (every texel is palette-index 1, a solid square) for the boundary outlines.
    std::array<std::uint8_t, 64> markerArt{};
    markerArt.fill(1);
    const AtlasId markerAtlas = renderer.uploadAtlas(markerArt.data(), 8, 8);

    // A dim grid tile so the filled regions read over something; low-contrast so the magenta outline pops.
    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8);

    // A bright solid tile for the layer the stencil hole reveals (Start mode).
    std::array<std::uint8_t, 64> brightArt{};
    brightArt.fill(1);
    const AtlasId brightAtlas = renderer.uploadAtlas(brightArt.data(), 8, 8);

    const std::array<Rgba8, 2> outlinePal{{{0, 0, 0}, {235, 120, 255}}};  // magenta — the true boundary
    const std::array<Rgba8, 2> vertexPal{{{0, 0, 0}, {255, 210, 90}}};    // gold — the coarse sample vertices
    const std::array<PaletteId, 2> markerPalettes{renderer.uploadPalette(std::span<const Rgba8>(outlinePal)),
                                                  renderer.uploadPalette(std::span<const Rgba8>(vertexPal))};
    // Resolve a marker ROLE (kOutline / kVertex) to its uploaded palette and build the marker that draws
    // from the marker atlas through it — each sprite names its own sheet + palette directly.
    const auto mark = [&](float x, float y, Pal role) {
        return marker(x, y, markerAtlas, markerPalettes[role]);
    };

    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {20, 26, 40}, {40, 52, 78}}};  // dim navy grid
    const PaletteId            gridPalId = renderer.uploadPalette(std::span<const Rgba8>(gridPal));
    const std::vector<TileCell>    gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                             TileCell{.tile = 0, .atlas = gridAtlas, .palette = gridPalId});

    const std::array<Rgba8, 2> brightPal{{{0, 0, 0}, {60, 200, 140}}};  // teal-green reveal
    const PaletteId            brightPalId = renderer.uploadPalette(std::span<const Rgba8>(brightPal));
    const std::vector<TileCell>    brightCells(static_cast<std::size_t>(kMapW) * kMapH,
                                               TileCell{.tile = 0, .atlas = brightAtlas, .palette = brightPalId});

    // The same wavy cubic blob on each side. LEFT: bake it into a mask once → an exact curved boundary. The
    // bake is reused every frame and reused under the rotation toggle with NO re-bake.
    constexpr float    kRadius = 34.0f;
    const Vec2         leftC{44, 80}, rightC{116, 80};
    constexpr int      kBlobPoints = 7;
    const Curve        leftCurve   = blob(leftC.x, leftC.y, kRadius, kBlobPoints);
    const Curve        rightCurve  = blob(rightC.x, rightC.y, kRadius, kBlobPoints);
    // Bake the left blob's distance field ONCE here, at setup. bakeCurveRegion hands back a ready region shape
    // whose boundary is this exact curve (its .curveMask is the baked handle). The render loop reuses it every
    // frame — the bake never repeats, even when the region is rotated below.
    const ShapePoints  leftRegionMasked = renderer.bakeCurveRegion(leftCurve);

    // Sample the right blob to a straight-edged polygon — the faceted approximation. B switches coarse/fine.
    const auto sampleRegion = [&](const Curve& c, int n) {
        ShapePoints r;
        r.points.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const Vec2 p = c.at(static_cast<float>(i) / static_cast<float>(n));
            r.points.push_back(Point{p.x, p.y});
        }
        return r;
    };
    constexpr int kCoarse = 10, kFine = 48;
    bool          fine     = false;  // B toggles the right side coarse/fine
    bool          spinning = false;  // A toggles a slow continuous spin (the mask is reused under it, no re-bake)
    bool          seeThrough = false;  // Start toggles the left region: colour fill <-> see-through hole
    float         angle    = 0.0f;   // accumulated spin angle (radians); frozen when the spin is off

    loop.setTick([&](const InputState& in) {
        if (in.justPressed(Button::B)) {
            fine = !fine;
            std::printf("[dev] right boundary samples: %d\n", fine ? kFine : kCoarse);
        }
        if (in.justPressed(Button::A)) {
            spinning = !spinning;
            std::printf("[dev] spin: %s — the masked (left) blob rotates yet stays exactly smooth (no re-bake)\n",
                        spinning ? "on" : "off");
        }
        if (in.justPressed(Button::Start)) {
            seeThrough = !seeThrough;
            std::printf("[dev] left region: %s\n", seeThrough ? "see-through stencil hole" : "colour fill");
        }
        if (in.justPressed(Button::Select)) platform.setFullscreen(!platform.isFullscreen());
        if (spinning) angle += 0.01f;  // ~0.6 rad/s at 60 Hz — slow, same-direction; photosensitivity-safe
    });

    std::vector<Sprite> sprites;
    FrameDrawState      frame;
    loop.setRender([&](float alpha) {
        // The optional rotation rides the region transform — the SAME baked mask, warped through the inverse
        // homography, with no re-bake. The right polygon takes the same transform and simply re-facets.
        const Transform leftXform  = Transform::rotation(angle, leftC.x, leftC.y);   // angle 0 ⇒ identity
        const Transform rightXform = Transform::rotation(angle, rightC.x, rightC.y);

        ShapePoints leftRegion = leftRegionMasked;  // carries the baked curveMask
        leftRegion.transform   = leftXform;
        ShapePoints rightRegion = sampleRegion(rightCurve, fine ? kFine : kCoarse);
        rightRegion.transform   = rightXform;

        // Trace the TRUE curve over BOTH sides (magenta), and the coarse vertices on the right (gold), so the
        // faceted fill visibly departs from the true curve while the masked fill follows it.
        sprites.clear();
        constexpr int kOutlineSamples = 72;
        for (int i = 0; i < kOutlineSamples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kOutlineSamples);
            const Vec2  lp = leftCurve.at(t);
            const Vec2  rp = rightCurve.at(t);
            sprites.push_back(mark(leftXform.applyX(lp.x, lp.y), leftXform.applyY(lp.x, lp.y), kOutline));
            sprites.push_back(mark(rightXform.applyX(rp.x, rp.y), rightXform.applyY(rp.x, rp.y), kOutline));
        }
        for (const Point& p : rightRegion.points)
            sprites.push_back(mark(rightXform.applyX(p.x, p.y), rightXform.applyY(p.x, p.y), kVertex));

        frame.layers.clear();
        // The reveal layer (only visible through the stencil hole), beneath the grid.
        DrawLayer reveal{};
        reveal.id      = "reveal";
        reveal.z       = -20;
        reveal.size    = PixelSize{kViewW, kViewH};
        reveal.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                     .cells = std::span<const TileCell>(brightCells)};
        frame.layers.push_back(reveal);

        DrawLayer bg{};
        bg.id      = "backgroundGrid";
        bg.z       = -10;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(gridCells)};
        // Start mode: punch a curved SEE-THROUGH hole in the grid along the masked boundary, via the stencil()
        // helper. Layer scope makes only THIS grid layer transparent inside the blob, so the teal layer beneath
        // (z = -20) shows through. The shape carries the baked curveMask, so this runs the curved-mask stencil.
        if (seeThrough)
            bg.regions = stencil(leftRegion, StencilMode::TransparentInside, /*feather=*/0.0f,
                                 ScreenSpaceEffectScope::Layer);
        frame.layers.push_back(bg);

        DrawLayer outlines{};
        outlines.id      = "boundaryOutlines";
        outlines.z       = 0;
        outlines.size    = PixelSize{kViewW, kViewH};
        outlines.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(std::move(outlines));

        // LEFT: the exact masked boundary, a translucent colour fill (in Start mode it is the see-through hole
        // above instead). RIGHT: the faceted polygon, always a colour fill.
        const ScreenSpaceEffect fill{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{31, 219, 255}};
        frame.regions.clear();
        if (!seeThrough)
            frame.regions.push_back(Region{.shape = leftRegion, .effects = {fill}, .alpha = 0.6f});
        frame.regions.push_back(Region{.shape = rightRegion, .effects = {fill}, .alpha = 0.6f});

        renderer.renderFrame(frame, alpha);
    });

    std::printf(
        "curve-mask region demo\n"
        "  Two copies of the SAME wavy blob. The thin MAGENTA line is the true curve on both.\n"
        "  LEFT  : edge computed EXACTLY from a baked distance-field mask  -> the fill hugs the magenta (smooth).\n"
        "  RIGHT : edge approximated by a few straight lines               -> the fill cuts across it (faceted).\n"
        "  Same shape, same colour; only how the boundary is computed differs -- that is the feature.\n"
        "  B = right coarse/fine facets   A = slow spin (masked edge stays smooth, no re-bake)\n"
        "  Start = left fill <-> see-through hole   Select = fullscreen   Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
