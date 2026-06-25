#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "retropp/curve.h"      // CurveSegment — an optional curved region boundary
#include "retropp/geometry.h"   // PixelSize
#include "retropp/image.h"      // AtlasId (relocated here beside the atlas-ingestion surface)
#include "retropp/palette.h"    // PaletteId
#include "retropp/transform.h"  // Transform

namespace retropp {

// The draw-state submission envelope: the C++ shape a game hands the renderer each frame.
//
// The frame's draw state is computed WHOLE every frame from the game's logical inputs —
// there is no mid-frame state-change API, no reconstructed scanline ISR, no register poke.
// A game recomputes and resubmits a fresh FrameDrawState per frame; layer existence, z,
// scroll, size, alpha, and effect/modifier parameters are all fresh each frame (the one
// amortized exception is the atlas texels, uploaded when they change). Effects that a GB
// expressed through hardware tricks are expressed here as layers, per-tile/per-region
// colour attributes, frame-level modifiers, and screen-space-effect declarations — never
// as a hardware-register idiom.
//
// Identity is a typed, first-class field throughout (LayerId, AtlasId, the *Kind enums) —
// never an array position, never demoted to a comment.

// ── Identity handles ────────────────────────────────────────────────────────────────

// Stable, game-assigned layer identity — a human-readable LABEL, not a number. A game names its
// layers ("BottomSpriteLayer", "ScrollingParallaxClouds", …) and references the same logical layer
// across frames by that name. The engine imposes no roles and never derives meaning from the name;
// it only uses it to tell layers apart and to name them in diagnostics. Identity is THIS field —
// never the submission position — and it is fully INDEPENDENT of z: z alone controls depth, the id
// plays no part in ordering. Compared by name. Construct implicitly from a string literal:
//   layer.id = "BottomSpriteLayer";
// The name must outlive the renderFrame() call that consumes it — string literals always do (static
// storage); a dynamically-built name follows the same game-owned-data lifetime as the content spans.
struct LayerId {
    std::string_view name;
    constexpr LayerId() noexcept = default;
    constexpr LayerId(const char* n) noexcept : name(n) {}        // implicit: id = "Foo"
    constexpr LayerId(std::string_view n) noexcept : name(n) {}
    [[nodiscard]] constexpr bool operator==(const LayerId&) const noexcept = default;
};

// AtlasId (a handle to uploaded atlas pixel data) lives in image.h beside the atlas-ingestion
// surface — included above. The fully-qualified retropp::AtlasId name is unchanged; TileContent /
// SpriteContent below carry it exactly as before.

// ── Tile content ────────────────────────────────────────────────────────────────────

// One cell of a tilemap: which atlas tile + which palette in the layer's set + flip. Named
// fields per the no-positional-opacity discipline — identity is a field, never a packed byte
// behind a comment. `palette` selects which palette WITHIN the layer's set (TileContent::palettes)
// this cell draws from — the mechanism that lets one palette render a full-colour map.
struct TileCell {
    std::uint16_t tile        = 0;  // index into the SELECTED atlas (the cell's sheet)
    std::uint8_t  palette     = 0;  // which palette in the layer's set
    bool          flipX       = false;
    bool          flipY       = false;
    std::uint8_t  atlasSelect = 0;  // which ATLAS in the layer's set (TileContent::atlases). 0 = the
                                    // first sheet (and the single-atlas default), so a cell that
                                    // leaves it unset draws from the single `atlas`.
};

// How a tile layer's tilemap is sampled outside its [0, mapPx) bounds. One mode governs both axes;
// the field lives on TileContent (it governs *tilemap* sampling; a sprite has no tilemap).
//   Repeat — toroidal: the map tiles infinitely on both axes (floorMod wrap).
//   Clamp  — clamp the world coord to the map's edge row/column (smear the border tile outward).
//   Blank  — FINITE map: a world coord outside [0, mapPx) on EITHER axis is a hole (transparent;
//            the layers below show through), so the map renders exactly once and can never show a
//            wrap seam — the mode a finite overworld map wants.
// Same blank-edge vocabulary as the transform footprint's DisplacementEdge::Blank — Blank discards
// to reveal the layers below.
enum class TileWrap : std::uint8_t { Repeat, Clamp, Blank };

// A tile layer's content: an INDEXED tile atlas (one palette index per pixel), the layer's
// palette set (the bank a cell's `palette` selects within), and a row-major tilemap
// (widthInTiles × heightInTiles). The map is sampled per-pixel in the tile shader against the
// layer's scroll, so arbitrary layer sizes and wrapping are handled on the GPU; `wrap` chooses how
// the tilemap is sampled beyond its bounds (Repeat/Clamp/Blank). `atlas`, `palettes`, and `cells`
// are game-owned; valid for the duration of the renderFrame() call that consumes them. A palette
// set of one is the single-palette case.
struct TileContent {
    AtlasId                    atlas{};         // indexed tile atlas (palette indices, not colour); the
                                                // layer's single sheet when `atlases` is empty (default).
    std::span<const PaletteId> palettes;        // the layer's palette set; TileCell::palette selects within
    int                        widthInTiles  = 0;
    int                        heightInTiles = 0;
    std::span<const TileCell>  cells;           // row-major, widthInTiles * heightInTiles
    TileWrap                   wrap = TileWrap::Repeat;  // out-of-bounds sampling; Repeat = toroidal
    // The layer's ATLAS SET — TileCell::atlasSelect selects within it, so ONE map mixes tiles from
    // several sheets. EMPTY (default) ⇒ the single `atlas` above is the only sheet and atlasSelect is
    // ignored. Non-empty ⇒ `atlas` is ignored and this set is the sheets (slot i = atlases[i]).
    // Mirrors `palettes`.
    std::span<const AtlasId>   atlases = {};
};

// The R32_UINT tilemap cell layout the tile fragment shader unpacks:
//   [tile:16][palette:8][flipX:1][flipY:1][atlasSelect:6]
// atlasSelect (bits 26..31, 0..kAtlasSetSlots-1) chooses which atlas in the layer's set the cell
// draws from; 0 selects the single-atlas default. This constexpr pair is the unit-tested mirror of
// the GPU packing — the shader unpacks the identical layout, so packTileCell(unpackTileCell(w)) == w
// for every valid cell.
[[nodiscard]] constexpr std::uint32_t packTileCell(const TileCell& c) noexcept {
    return static_cast<std::uint32_t>(c.tile)
         | (static_cast<std::uint32_t>(c.palette) << 16)
         | (static_cast<std::uint32_t>(c.flipX ? 1u : 0u) << 24)
         | (static_cast<std::uint32_t>(c.flipY ? 1u : 0u) << 25)
         | ((static_cast<std::uint32_t>(c.atlasSelect) & 0x3Fu) << 26);
}

[[nodiscard]] constexpr TileCell unpackTileCell(std::uint32_t packed) noexcept {
    TileCell c;
    c.tile        = static_cast<std::uint16_t>(packed & 0xFFFFu);
    c.palette     = static_cast<std::uint8_t>((packed >> 16) & 0xFFu);
    c.flipX       = ((packed >> 24) & 1u) != 0u;
    c.flipY       = ((packed >> 25) & 1u) != 0u;
    c.atlasSelect = static_cast<std::uint8_t>((packed >> 26) & 0x3Fu);
    return c;
}

// ── Sprite content ────────────────────────────────────────────────────────────────────

// A sprite's pixel dimensions are an AssetDimensions (geometry.h) — the same type the atlas slicer
// carves an image into, with the console-named presets (AssetDimensions::GameBoy8x8, …).

// One placed sprite. `x`/`y` are the top-left in the LAYER's coordinate space (before scroll —
// the vertex shader subtracts the layer scroll, so a sprite on a world-scroll layer tracks the
// background, and a HUD layer at scroll {0,0} stays fixed). `tile` is the top-left atlas cell
// (8px grid); the sprite reads a size.width × size.height pixel rectangle from the atlas at that
// cell's pixel origin (so a 16×16 sprite spans a 2×2 cell block laid out contiguously). `palette`
// selects which palette WITHIN the layer's set this sprite colours through. Identity is the named
// fields — no packed attribute byte.
//
// `transform` is the sprite's own geometric transform, applied in SPRITE-LOCAL pixel space — the
// [0, size.width] × [0, size.height] rectangle of the sprite's own art — about whatever pivot the
// caller encoded (the engine imposes no default; rotate an 8×8 about its centre with
// Transform::rotation(deg, 4, 4)). It composes with the layer's own DrawLayer::transform: a sprite
// quad goes sprite.transform first, then the layer transform, exactly as a tile layer's content does.
// The identity default is a no-op (a plain axis-aligned quad). Flips stay a fragment UV op,
// independent of the geometry — a flipped+rotated sprite mirrors its texture and rotates its quad.
struct Sprite {
    int             x       = 0;
    int             y       = 0;
    AssetDimensions size    = AssetDimensions::GameBoy8x8;
    std::uint16_t   tile    = 0;       // top-left atlas cell
    std::uint8_t  palette   = 0;       // palette-select within the layer's set
    bool          flipX     = false;
    bool          flipY     = false;
    Transform     transform{};         // per-sprite geometric transform, sprite-local space; identity default
};

// A sprite layer's content: an INDEXED atlas (shared with the tile path's atlas registry), the
// layer's palette set (the bank a sprite's `palette` selects within), and the layer's sprites.
// `atlas`, `palettes`, and `sprites` are game-owned; valid for the duration of the renderFrame()
// call. Mirrors TileContent. An empty `sprites` span is a valid (degenerate) submission.
struct SpriteContent {
    AtlasId                    atlas{};         // indexed sprite atlas (palette indices, not colour)
    std::span<const PaletteId> palettes;        // the layer's palette set; Sprite::palette selects within
    std::span<const Sprite>    sprites;         // the layer's placed sprites
};

// The sprite storage-buffer record the sprite VERTEX shader reads (one per sprite). std430-style
// 16-byte alignment → 64 bytes, laid out as the shader's { float4 row0; float4 row1; float4 row2;
// uint4 attr; }:
//   row0/row1/row2 = the nine coefficients (row-major, the 4th lane padding) of the COMPOSED
//          clip-space homography H that maps a UNIT-quad corner (cx, cy) ∈ {0,1}² directly to clip-
//          space homogeneous coordinates: clip = H · (cx, cy, 1). H bakes the whole chain CPU-side —
//          unit→sprite-pixel scale, the per-sprite Transform, the scrolled top-left translation, the
//          per-layer Transform, and screen→clip (viewport scale + top-left-origin V-flip) — so the
//          vertex stage stays a pure storage-buffer read with NO uniform. That single-buffer
//          constraint is load-bearing: a vertex stage carrying both a storage buffer AND a uniform
//          buffer collides in Metal's [[buffer]] namespace under the single-pass HLSL→SPIR-V→MSL
//          toolchain (SDL_GPU offsets storage buffers past the uniform buffers, which the toolchain
//          can't express alongside Vulkan's descriptor layout).
//          The bottom row (m20, m21) carries the perspective terms — non-zero ⇒ the per-vertex w
//          varies ⇒ the GPU perspective-divides and interpolates the within-sprite UV perspective-
//          correct for free; zero ⇒ the affine case (w ≡ 1), a plain axis-aligned quad.
//   attr = (tile, paletteOffset, flags, size): `paletteOffset` is the RESOLVED palette flat offset
//          into the palette store (resolved CPU-side from the layer's set + the sprite's select);
//          `flags` is packSpriteFlags; `size` is the pixel size packed (width<<16)|height for the
//          fragment's within-sprite addressing. The unit-tested CPU↔GPU mirror, same discipline as packTileCell.
struct GpuSprite {
    float         row0[4];        // H row 0: m00 m01 m02 _   (unit-quad corner → clip homography)
    float         row1[4];        // H row 1: m10 m11 m12 _
    float         row2[4];        // H row 2: m20 m21 m22 _   (m20,m21 = perspective; w = m20·x + m21·y + m22)
    std::uint32_t tile;           // top-left atlas cell
    std::uint32_t paletteOffset;  // resolved palette flat offset into the palette store
    std::uint32_t flags;          // bit0 flipX | bit1 flipY
    std::uint32_t size;           // pixel size packed (width<<16)|height
};
static_assert(sizeof(GpuSprite) == 64);

[[nodiscard]] constexpr std::uint32_t packSpriteFlags(bool flipX, bool flipY) noexcept {
    return (flipX ? 1u : 0u) | (flipY ? 2u : 0u);
}

// Pack an asset's pixel dimensions into one uint (width in the high 16 bits). The fragment shader
// unpacks this to map the interpolated within-sprite UV back to an atlas pixel.
[[nodiscard]] constexpr std::uint32_t packAssetSize(const AssetDimensions& sz) noexcept {
    return (static_cast<std::uint32_t>(sz.width) << 16) | static_cast<std::uint32_t>(sz.height & 0xFFFF);
}

// Resolve a sprite's palette-select to a palette flat offset via the layer's set (mirrors the tile
// path's paletteSetOffsets mapping; a PaletteId's underlying value IS its flat offset into the
// palette store). An out-of-range select or an empty set resolves to offset 0 (degenerate but valid).
[[nodiscard]] constexpr std::uint32_t spritePaletteOffset(std::span<const PaletteId> set,
                                                          std::uint8_t select) noexcept {
    return select < set.size() ? static_cast<std::uint32_t>(set[select]) : 0u;
}

// Build the GPU record for one sprite. `viewportW`/`viewportH` are the offscreen viewport pixel
// size; `scrollX`/`scrollY` the layer scroll; `layerTransform` the per-layer DrawLayer::transform
// (D.1). The composed clip-space homography is baked here so the vertex shader is a pure storage-
// buffer read (no uniform). Pure + constexpr — the unit-tested CPU↔GPU mirror.
//
// The chain a unit-quad corner (cx, cy) travels, via the constexpr Transform::then():
//   H = scale(w, h)                    // unit corner → sprite-local content pixel
//         .then(s.transform)           // per-sprite transform, sprite-local space (about its own pivot)
//         .then(translation(sox, soy)) // scrolled screen top-left  (sox = x − scrollX, soy = y − scrollY)
//         .then(layerTransform)        // per-layer transform, viewport-pixel space (D.1)
//         .then(screenToClip)          // viewport scale + top-left-origin V-flip
// Scroll is subtracted BEFORE the layer transform — matching the tile path, where the layer
// transform maps (world − scroll) to the destination — so a tile layer and a sprite layer carrying
// the same Transform line up and share the same pivot space. With identity sprite + layer transforms
// H reduces to a plain axis-aligned quad (w ≡ 1).
[[nodiscard]] constexpr GpuSprite makeGpuSprite(const Sprite& s, std::uint32_t paletteOffset,
                                                int viewportW, int viewportH,
                                                int scrollX, int scrollY,
                                                const Transform& layerTransform = Transform{}) noexcept {
    const float vw  = static_cast<float>(viewportW);
    const float vh  = static_cast<float>(viewportH);
    const float sox = static_cast<float>(s.x - scrollX);  // screen-space top-left
    const float soy = static_cast<float>(s.y - scrollY);

    // screen→clip: x' = sox·(2/vw) − 1,  y' = 1 − soy·(2/vh)  (top-left-origin V-flip).
    const Transform screenToClip{2.0f / vw, 0.0f,       -1.0f,
                                 0.0f,      -2.0f / vh,  1.0f,
                                 0.0f,      0.0f,        1.0f};

    const Transform H =
        Transform::scale(static_cast<float>(s.size.width), static_cast<float>(s.size.height))
            .then(s.transform)
            .then(Transform::translation(sox, soy))
            .then(layerTransform)
            .then(screenToClip);

    GpuSprite g{};
    g.row0[0] = H.m00; g.row0[1] = H.m01; g.row0[2] = H.m02; g.row0[3] = 0.0f;
    g.row1[0] = H.m10; g.row1[1] = H.m11; g.row1[2] = H.m12; g.row1[3] = 0.0f;
    g.row2[0] = H.m20; g.row2[1] = H.m21; g.row2[2] = H.m22; g.row2[3] = 0.0f;
    g.tile          = s.tile;
    g.paletteOffset = paletteOffset;
    g.flags         = packSpriteFlags(s.flipX, s.flipY);
    g.size          = packAssetSize(s.size);
    return g;
}

// A layer carries exactly one content alternative. The active alternative is the variant's
// identity; LayerContentKind mirrors it for explicit, switch-friendly dispatch.
enum class LayerContentKind : std::uint8_t { Tiles, Sprites };
using LayerContent = std::variant<TileContent, SpriteContent>;
[[nodiscard]] constexpr LayerContentKind contentKind(const LayerContent& c) noexcept {
    return c.index() == 0 ? LayerContentKind::Tiles : LayerContentKind::Sprites;
}

// ── Effect region — the shape an effect is confined to ───────────────────────────────────

// A point in VIEWPORT PIXELS (top-left origin) — the screen space an effect composites in, the same
// units as Sprite::x/y and the Transform pivots (deliberately pixels, not normalized UV, so points and
// `radius` share one unit). Identity is the named fields.
struct Point {
    float x = 0.0f;
    float y = 0.0f;
    [[nodiscard]] constexpr bool operator==(const Point&) const noexcept = default;
};
static_assert(sizeof(Point) == 8 && alignof(Point) == 4,
              "Point must match the shader's float2 — it is memcpy'd into the points storage buffer");

// The shape an effect is confined to. A polygon of ordered VIEWPORT-PIXEL vertices, inflated by
// `radius` (a signed-distance rounding), warped by `transform`. The points ARE the position — there
// is no separate origin. Containment is an SDF, not a rasterized polygon, so one type covers every
// shape (see regionContains / sdPolygon in postprocess.h):
//   empty                → NO region: the effect covers the whole viewport (the default).
//   1 point + radius     → a CIRCLE (distance-to-point ≤ radius).
//   2 points + radius    → a CAPSULE / stadium (distance-to-segment ≤ radius).
//   ≥ 3 points, radius 0 → a sharp polygon (triangle / quad / N-gon; arbitrary CONCAVE outlines OK).
//   ≥ 3 points, radius>0 → a rounded polygon.
// `transform` (identity default) is a Transform composed on top — scale / stretch (non-uniform
// scale) / skew / rotate / perspective / translate the placed shape, evaluated by the same inverse-
// homography the tile path uses. Move a shape by rewriting points OR via transform translation.
//
// `points` is a std::vector with no cap in the API; the renderer packs the vertices into the
// region-select cbuffer, which carries up to 64 — a longer polygon is truncated there with a logged
// warning. The presets are the named-constructor idiom (a placed shape is parametric); a raw
// ShapePoints{ .points = {...}, .radius = r } stays allowed for the unnamed.
//
// `curve` (the default-empty member) makes the boundary a CLOSED CURVE rather than a straight-edged
// polygon — exact between control points, no facets, no vertex cap. When `curve` is non-empty it IS
// the boundary and `points` is ignored; `radius` (SDF inflation) and `transform` (the inverse-
// homography warp) compose on top of the curve distance exactly as they do for a polygon. Linear and
// quadratic segments evaluate analytically (exact). A cubic / Catmull-Rom segment has no closed-form GPU
// distance: attach a baked mask (`curveMask`, from Renderer::bakeCurveMask / bakeCurveRegion) to evaluate
// it exactly, or leave it unset and the boundary samples to a faceted polygon (the points path) — see
// fromCurve and the region gate. Empty `curve` ⇒ the polygon path, identical to a curve-free region.

// A handle to a baked curve signed-distance mask (Renderer::bakeCurveMask). A cubic / Catmull-Rom /
// arbitrary closed boundary has no closed-form GPU distance, so its Curve::signedDistance is baked once
// into a texture the region samples per fragment. 0 (the default) = none — the region carries no baked mask.
enum class CurveMaskId : std::uint32_t {};

struct ShapePoints {
    std::vector<Point>        points;        // ordered viewport-pixel vertices; empty = no polygon
    float                     radius = 0.0f; // SDF inflation: 0 = sharp polygon edges
    Transform                 transform{};   // additional warp, identity default
    std::vector<CurveSegment> curve;         // a CLOSED curve boundary (viewport px); empty = none
    // Treat the OUTSIDE of the shape as the region instead of the inside. A region-confined effect then
    // applies OUTSIDE the shape; the inside/outside test simply flips. Default false (the inside is the
    // region). An empty region ignores it.
    bool                      invert = false;
    // Confine to a BAND along the shape's boundary instead of its filled interior — the shape's outline.
    // 0 (default) = the filled region (the whole inside). > 0 = a stroke of that width (viewport px),
    // centered on the radius-inflated boundary, so the region's effects trace a hoop / border / curved
    // PATH rather than fill the interior. Composes with `radius` (the band rides the inflated edge),
    // `transform`, `curve`, and `invert` (stroke then invert = everything except the band). Applies to
    // EVERY region consumer — the effect-confinement gate AND the Transparency see-through path — because
    // it is one transform of the signed distance both already compute. A stroke is symmetric about the
    // boundary, hence sign-independent, so an OPEN fromCurve(...) strokes into an open band (an effect
    // follows an arbitrary curved path); a polygon (via `points`) is always a closed-loop band.
    float                     strokeWidth = 0.0f;
    // A baked signed-distance mask for a CUBIC / arbitrary curved boundary — the exact GPU evaluation of a
    // boundary the analytic linear+quadratic path cannot solve in closed form. 0 (default) = none. Bake one
    // with Renderer::bakeCurveMask (or take a ready shape from Renderer::bakeCurveRegion); the region samples
    // the mask per fragment instead of sampling the curve to a faceted polygon. `radius`, `strokeWidth`,
    // `transform`, and `invert` compose on the sampled distance unchanged. Consulted only when `curve` carries
    // a cubic segment (a linear/quadratic boundary stays on the exact analytic path; a polygon ignores it).
    CurveMaskId               curveMask{};

    [[nodiscard]] bool operator==(const ShapePoints&) const = default;
    [[nodiscard]] bool hasRegion() const noexcept { return !points.empty() || !curve.empty(); }

    // A copy of this shape with its inside and outside swapped (toggles `invert`). Use it to confine a
    // standalone Region's effects to the OUTSIDE of a shape you authored:
    //   Region{ .shape = ShapePoints::circle(c, r).inverted(), .effects = {…} }.
    // The stencil() helper uses it to put a side effect on the OUTSIDE of a Transparency's shape.
    [[nodiscard]] ShapePoints inverted() const { ShapePoints s = *this; s.invert = !s.invert; return s; }

    // Named-constructor presets (the Transform::rotation() idiom). All coordinates are viewport pixels.
    [[nodiscard]] static ShapePoints circle(Point c, float r);
    [[nodiscard]] static ShapePoints capsule(Point a, Point b, float r);
    [[nodiscard]] static ShapePoints triangle(Point a, Point b, Point c);
    [[nodiscard]] static ShapePoints rectangle(Point topLeft, float w, float h);
    [[nodiscard]] static ShapePoints roundedRectangle(Point topLeft, float w, float h, float r);
    [[nodiscard]] static ShapePoints regularPolygon(Point c, float r, int sides);

    // A region whose boundary is the curve `c` (treated as closed — the last segment's end joins the
    // first's start). `radius` inflates it; `transform` warps it. The front door for genuinely curved
    // boundaries authored with Curve::quadratic / quadraticTo / line / lineTo (and cubic / Catmull-Rom,
    // which the gate samples to a faceted polygon).
    [[nodiscard]] static ShapePoints fromCurve(const Curve& c, float radius = 0.0f, Transform t = {});
};

inline ShapePoints ShapePoints::circle(Point c, float r)        { return ShapePoints{.points = {c}, .radius = r}; }
inline ShapePoints ShapePoints::capsule(Point a, Point b, float r) { return ShapePoints{.points = {a, b}, .radius = r}; }
inline ShapePoints ShapePoints::triangle(Point a, Point b, Point c) { return ShapePoints{.points = {a, b, c}}; }

inline ShapePoints ShapePoints::rectangle(Point tl, float w, float h) {
    return ShapePoints{.points = {{tl.x, tl.y}, {tl.x + w, tl.y}, {tl.x + w, tl.y + h}, {tl.x, tl.y + h}}};
}

// A rounded rectangle that FITS within [topLeft, topLeft+{w,h}]: the polygon is inset by r on every side
// and the SDF inflates it back out by r, so corners round with radius r and the outer extent is exactly
// w×h (for r ≤ min(w,h)/2). r = 0 reduces to the sharp rectangle's footprint.
inline ShapePoints ShapePoints::roundedRectangle(Point tl, float w, float h, float r) {
    const float x0 = tl.x + r, y0 = tl.y + r, x1 = tl.x + w - r, y1 = tl.y + h - r;
    return ShapePoints{.points = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, .radius = r};
}

// A regular n-gon (n floored at 3, no upper cap) centred at c with circumradius r, first vertex at the
// top. The unbounded points make a high `sides` a smooth-looking circle approximation if you want one.
inline ShapePoints ShapePoints::regularPolygon(Point c, float r, int sides) {
    const int n = sides < 3 ? 3 : sides;
    ShapePoints s;
    s.points.reserve(static_cast<std::size_t>(n));
    constexpr float kTwoPi  = 6.283185307179586f;
    constexpr float kHalfPi = 1.5707963267948966f;
    for (int i = 0; i < n; ++i) {
        const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(n) - kHalfPi;  // start at top
        s.points.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    return s;
}

// The boundary IS the curve `c` (its segments, treated as a closed loop). `points` stays empty so the
// gate takes the curve path; radius/transform ride along unchanged.
inline ShapePoints ShapePoints::fromCurve(const Curve& c, float radius, Transform t) {
    return ShapePoints{.radius = radius, .transform = t, .curve = c.segments};
}

// ── Screen-space effects ──────────────────────────────────────────────────────────────────

enum class Axis : std::uint8_t { Horizontal, Vertical };

enum class ScreenSpaceEffectKind : std::uint8_t {
    None,            // pass-through — no effect (the default)
    RowDisplacement, // wavy water / heat haze / per-line SCX = f(row, time) in a shader
    Ripple,          // radial concentric ripple — a water droplet; built-in
    Custom,          // a game-registered shader — see PostProcessStageId + .customShader
    Transparency,    // make the effect's region SEE-THROUGH (reveal what's behind); built-in
    ColorFill,       // paint a colour onto the effect's region — out.rgb = fill (a solid fill); built-in
};

// Which side of a Transparency effect's region goes see-through (kind == Transparency). The region is the
// shape of the Region that owns the effect; this picks which side of it the layer turns transparent along.
//   TransparentInside  — the pixels INSIDE the shape go see-through → a hole; the layers below show through. (default)
//   TransparentOutside — the pixels OUTSIDE the shape go see-through → a porthole; only the inside stays solid.
// Transparency is the subtractive sibling of region-as-fill: where a region confines an effect to ADD inside
// a shape, Transparency makes the layer SEE-THROUGH to reveal what is behind it. (The `stencil()` free helper
// builds the equivalent Region(s) for the common "make a shape see-through" case — see below.)
enum class StencilMode : std::uint8_t { TransparentInside, TransparentOutside };

// The engine's BUILT-IN effect library is the set of ScreenSpaceEffectKinds the engine
// owns a shader for — RowDisplacement (the axis-aligned wave), Ripple (the radial droplet), and ColorFill
// (a colour painted onto a region). A game sets `.kind` on a ScreenSpaceEffect and fills the fields that
// kind consults (plain designated-init — every field is settable inline); the engine supplies the shader.
// No registration, no shader authoring — that is the Custom path. New built-ins land behind this enum; the
// candidate menu is docs/effect-library-roadmap.md.
// Which fields each built-in consults (the rest stay at their defaults, ignored):
//   RowDisplacement → amplitude, frequency, phase, axis, edge
//   Ripple          → amplitude, frequency, phase, center, decay
//   Custom          → none of the above — the game's own shader + uniform define the behaviour
//   Transparency    → stencil, feather — makes its REGION see-through (no colour effect); the region is the
//                     shape of the Region that owns it (like every other effect; confinement comes from a Region)
//   ColorFill       → fill, fillIntensity — paints a colour (out.rgb = fill · fillIntensity) onto its region;
//                     via the owning region's blend mode it is the day/night (Multiply), tint (Add), and
//                     fade/flash (Normal) workhorse; fillIntensity > 1 lets Multiply brighten (float16 path)
//   (any kind)      → paramTable — a generic per-row data table (one Vec4 per scanline / per region id);
//                     in v1 only a Custom shader reads it (via the preamble's paramRow / paramRowAtUv)
// (scope applies to EVERY kind: it is a compositing decision the engine makes, not the shader's. NO effect
//  carries its own geometry — every kind is region-agnostic; confinement comes from a Region.)

// A handle to a game-registered custom shader stage the renderer owns.
// Identity is the typed handle, mirroring AtlasId/PaletteId; the renderer maps it to the pipeline
// pair it built from the game's fragment in registerPostProcessStage(). A custom shader is a
// first-class effect KIND: an effect with kind == Custom carries one of these in .customShader and
// runs through the SAME per-layer (Layer/Below) and frame-level (postEffects) machinery the built-in
// effects use — wherever a built-in effect works, a custom one does too.
enum class PostProcessStageId : std::uint32_t {};

// What a displacement does at the frame edge, where a row/column pulled inward exposes a strip with
// no source pixel behind it. Developer-selectable per effect:
//   Blank   — the exposed strip is the backdrop colour (nothing there). The faithful default: a
//             whole-frame displacement has no off-screen content to reveal, so it shows blank.
//   Stretch — the edge pixel is duplicated outward (CLAMP_TO_EDGE), smearing the border colour.
enum class DisplacementEdge : std::uint8_t { Blank, Stretch };

// Which pixels a per-layer effect transforms — the composable Photoshop-layer model.
// (Meaningful for DrawLayer::effects; FrameDrawState::postEffects is inherently whole-frame and
// ignores it.)
//   Layer — ISOLATED: displace ONLY this layer's own content, before it composites. A wavy water
//           layer distorts while the layers/sprites composited above it stay still. The default.
//   Below — ADJUSTMENT LAYER: displace the WHOLE accumulated image at this layer's z — this layer's
//           own content AND everything beneath it, coherently — then layers above this z composite
//           on top, undisplaced. A content-less Below layer just under a HUD wobbles the world while
//           the HUD rides steady; a content-bearing Below layer wobbles itself together with the
//           scene beneath. Multiple Below effects compose by z.
enum class ScreenSpaceEffectScope : std::uint8_t { Layer, Below };

// A screen-space effect declaration: the parameters carried as data, interpreted by the effect's
// shader stage. In screen space the fragment's row coordinate IS the scanline, so a continuous
// effect is a function f(row, time, frame-state) the GPU evaluates per-pixel — no reconstructed LY
// counter, no HBlank ISR. The game advances `phase` per frame to animate (runtime-dynamic).
struct ScreenSpaceEffect {
    ScreenSpaceEffectKind kind = ScreenSpaceEffectKind::None;  // identity, first member

    // kind == Custom only — WHICH registered custom shader runs (the handle from
    // Renderer::registerPostProcessStage(path)). A Custom effect carries its handle here, then sets the
    // shader's OWN declared parameters as inline fields below (the generated union) — exactly the way a
    // built-in sets amplitude/center/etc. Placed right after `kind` so the call reads
    //   ScreenSpaceEffect{ .kind = Custom, .customShader = h, .<param> = …, … }.
    PostProcessStageId customShader{};

    // ── Built-in effect parameters (amplitude/frequency/phase/axis ignored for kind == Custom — a custom
    //    shader uses its OWN params, below; `edge` and `scope` apply to ALL kinds incl. Custom) ──
    float amplitude = 0.0f;   // displacement magnitude, viewport px (RowDisplacement / Ripple)
    float frequency = 0.0f;   // cycles across the axis (RowDisplacement) / rings across the field (Ripple)
    float phase     = 0.0f;   // animation phase — the game advances it off frame time to animate
    Axis  axis      = Axis::Horizontal;                            // RowDisplacement
    // Edge policy at the frame border — governs RowDisplacement's exposed strip AND a Custom shader's
    // sampleSource() (the engine forwards it to the shader): Blank (default) = transparent (reveal the
    // backdrop / layers below); Stretch = clamp/smear. A layer that doesn't want clamping never gets it.
    DisplacementEdge edge = DisplacementEdge::Blank;
    ScreenSpaceEffectScope scope = ScreenSpaceEffectScope::Layer;  // per-layer reach; Layer (isolated) default

    // Ripple: a RADIAL concentric displacement (a water droplet) — the sample is pushed along
    // the radius from `center` by sin(2π·(frequency·dist − phase)), faded by exp(−decay·dist). `center` is
    // VIEWPORT PIXELS (like Point / Sprite::x,y — the engine normalizes to UV and aspect-corrects so the
    // rings stay circular); `decay` is the radial falloff.
    Point center{};
    float decay = 0.0f;

    // ── Transparency parameters (kind == Transparency) ──
    // A Transparency makes its REGION see-through (the shape comes from the owning Region, like every other
    // effect — it carries no geometry of its own). `stencil` picks which side of that region goes see-through
    // (TransparentInside = the inside, TransparentOutside = the outside); `feather` softens the boundary (shape-local px,
    // the same units as shape.radius — 0 = a hard edge, > 0 = a coverage ramp centered on the boundary).
    // `scope` and `edge` apply as for every kind; the other built-in params are ignored. The `stencil()` free
    // helper (below) builds the Region(s) for the common "make a shape see-through, optionally run effects on
    // each side" case.
    StencilMode stencil = StencilMode::TransparentInside;
    float       feather = 0.0f;

    // ── ColorFill parameters (kind == ColorFill) ──
    // Paint a colour onto the pixels the effect covers (its region): a stroked Region draws a colored
    // line/path, a filled Region a solid shape. out.rgb = fill; the layer alpha sets the opacity. The
    // owning Region's `blend` grades how the fill combines over the scene — Multiply for a shadow / tint /
    // day-night, Add for a glow, Screen for bloom — with Normal replacing the covered pixels (a fade / flash).
    // The CPU mirror of the fill colour is retropp::applyColorFill; the grade is retropp::applyBlendMode.
    Rgba8 fill{};

    // A scalar applied to `fill` so the painted colour can exceed 1 (the fill is fill_rgb/255 · fillIntensity).
    // Default 1 = the plain fill. Above 1 only shows through a container blend that carries the headroom —
    // Multiply (scene · fill, a multiplicative exposure that BRIGHTENS while preserving contrast), Screen, Add
    // — and only because the offscreen intermediates are float16 (an 8-bit intermediate would clamp it to 1).
    // Below 1 dims the fill; 0 paints black. Unbounded — a sane range is the game's responsibility.
    float fillIntensity = 1.0f;

    // ── Per-row data table (a generic effect input) ──
    // An optional array the game fills each frame, one Vec4 per row — a per-scanline value (indexed by the
    // fragment's scanline) or a per-region value (indexed by id); the consumer's shader decides what the
    // index means. It is the table counterpart to the scalar params above: where amplitude / phase are one
    // value, this is an array the effect reads by row. Delivered to the shader as a small data texture it
    // Loads by row (the preamble's paramRow / paramRowAtUv). In v1 only a Custom shader reads it (built-in
    // kinds ignore it); empty (the default) means no table. Game-owned, valid for the renderFrame call —
    // the immediate-mode span contract, like a layer's cells / sprites / palettes.
    std::span<const Vec4> paramTable{};

    // ── Custom-shader parameters (kind == Custom) ──
    // The UNION of every game-authored custom shader's OWN cbuffer params, surfaced here BY NAME (a shader
    // declares `cbuffer Params { float2 pivot; float blend; }`, the build reflects it, and `.pivot`/`.blend`
    // become fields here). A Custom effect sets the ones its shader declares — inline, exactly like a
    // built-in's named params, no per-game uniform struct / byte span / size. The renderer fills the
    // shader's cbuffer from these via that shader's generated packer (it never reads the fields directly,
    // so this generated set never changes the engine's view of the struct). Generated empty when no custom
    // shader is referenced in the build (gen_effect_fields.cmake).
#include "retropp/generated/custom_effect_fields.inc"
};

// ── Container blend mode ──────────────────────────────────────────────────────────────────────
//
// How a compositing CONTAINER's pixels combine with what they composite over. A container — a Region,
// a DrawLayer, or the whole FrameDrawState — carries a BlendMode beside its `alpha`: `alpha` is HOW
// MUCH the container contributes, `blend` is HOW it combines. Normal is the alpha-over of a Photoshop-
// style layer stack (the default, and the exact output when every container is Normal); the
// others are the standard separable blend operators a retro look reaches for — Add (glows / fire /
// light), Subtract, Multiply (shadows / tints), Screen (bloom), and Half (a halved average,
// (dst+src)/2, for translucency). Blend is a property of the CONTAINER that owns the pixels, never of a
// screen-space effect: an effect is a colour SOURCE, and the region / layer / frame that holds it
// decides how that source merges. The math is the separable operator B(dst, src) per mode applied
// source-alpha-weighted; retropp::applyBlendMode (postprocess.h) is the single authority the
// compositor shaders mirror.
//
// Distinct from the frame-level `Blend` (the cutscene flash, a colour-mix toward a target) — that is a
// different concept and is unchanged.
enum class BlendMode : std::uint8_t {
    Normal,    // alpha-over: (1-srcA)·dst + srcA·src — the default, byte-identical to no blend
    Add,       // additive: dst + src           (glows, fire, light)
    Subtract,  // subtractive: dst − src
    Multiply,  // multiplicative: dst · src      (shadows, tints)
    Screen,    // 1 − (1−dst)(1−src) — inverse-multiply (bloom)
    Half,      // (dst + src) / 2 — a halved average (translucency)
};

// ── Region — a shape that owns the effects applied inside it ──────────────────────────────────
//
// Ownership runs shape → effects: a Region binds a SHAPE to the effects applied INSIDE that shape, in
// list order. An effect itself carries no shape; the confinement comes from the Region that owns it (an
// effect with no region covers its whole scope via DrawLayer::effects / FrameDrawState::postEffects). A
// layer (DrawLayer::regions) and the frame (FrameDrawState::regions) own a list of regions; one region
// can carry several effects (a Transparency that makes a shape see-through AND a Ripple that fills the same
// shape), and one effect drops into many regions with no duplication. Confine effects to the OUTSIDE of a
// shape with ShapePoints::inverted(). The same SDF gate the engine already had does the confinement, per
// effect. Regions are OPTIONAL and ADDITIVE — the whole-reach effects / postEffects paths are unchanged, so
// a frame that uses neither renders exactly as before. An empty `effects` list is a no-op region.
struct Region {
    ShapePoints                    shape;    // the confinement (viewport pixels); shape.inverted() = outside
    std::vector<ScreenSpaceEffect> effects;  // applied inside `shape`, in list order
    float                          alpha = 1.0f;  // opacity of this region's effects over the scene, [0,1]; 1 = full
    BlendMode                      blend = BlendMode::Normal;  // how its effects combine over the scene; Normal = alpha-over
};

// ── stencil() — the "make a shape see-through" sugar ──────────────────────────────────────────
//
// Build the Region(s) that make `shape` see-through and (optionally) run effects on each side. A free
// helper, no engine state: it expands to the equivalent Region model and the renderer treats the result
// like any other regions. Push it onto a layer's or the frame's `regions` (replace or append):
//   layer.regions  = stencil(ShapePoints::circle({80, 72}, 30));               // a plain hole in this layer
//   frame.regions  = stencil(shape, StencilMode::TransparentOutside, /*feather=*/8); // a soft frame-wide porthole
//
// `mode` picks which side goes see-through (TransparentInside = the inside → a hole the layers below show through;
// TransparentOutside = the outside → a porthole keeping only the inside). `feather` softens the boundary (0 = hard).
// `scope` is the Transparency's reach: Layer (default) turns only THIS layer see-through (reveal the layers
// at lower z); Below turns this layer AND everything beneath it see-through (reveal the backdrop). `insideRegion`
// / `outsideRegion` are effects confined to the shape's inside / its outside — they run at Below scope on the
// composited scene, so they distort what shows THROUGH the see-through area, not just the (transparent) layer.
[[nodiscard]] inline std::vector<Region>
stencil(ShapePoints shape,
        StencilMode mode = StencilMode::TransparentInside,
        float feather = 0.0f,
        ScreenSpaceEffectScope scope = ScreenSpaceEffectScope::Layer,
        std::vector<ScreenSpaceEffect> insideRegion  = {},
        std::vector<ScreenSpaceEffect> outsideRegion = {}) {
    std::vector<Region> regions;
    // The see-through: a Transparency confined to `shape`, at the caller's scope.
    regions.push_back(Region{shape, {ScreenSpaceEffect{.kind    = ScreenSpaceEffectKind::Transparency,
                                                       .scope   = scope,
                                                       .stencil = mode,
                                                       .feather = feather}}});
    // Each side effect: confined to the shape (inside) or its inverse (outside), at Below scope so it
    // resolves on the composited scene the see-through reveals.
    for (ScreenSpaceEffect e : insideRegion) {
        e.scope = ScreenSpaceEffectScope::Below;
        regions.push_back(Region{shape, {e}});
    }
    for (ScreenSpaceEffect e : outsideRegion) {
        e.scope = ScreenSpaceEffectScope::Below;
        regions.push_back(Region{shape.inverted(), {e}});
    }
    return regions;
}

// ── Layer + frame ─────────────────────────────────────────────────────────────────────

struct LayerScroll { int x = 0; int y = 0; };

// One layer in the frame's arbitrary, Z-sorted stack. No semantic role is imposed by the
// engine. Runtime-dynamic: every field is fresh each frame.
struct DrawLayer {
    LayerId           id{};                 // stable identity label — first member; no role in depth
    std::int32_t      z = 0;                // back-to-front sort key; unique within a frame
    PixelSize         size{};               // independent per-layer dimensions
    LayerScroll       scroll{};             // independent scroll offset
    float             alpha = 1.0f;         // [0,1], default opaque
    BlendMode         blend = BlendMode::Normal;  // how this layer composites over the accumulator; Normal = alpha-over
    LayerContent      content{ TileContent{} };
    std::vector<ScreenSpaceEffect> effects; // per-layer WHOLE-REACH effect chain (no shape); empty = none
    std::vector<Region> regions;            // per-layer confined effects; each region's effects fill its shape (optional)
    Transform         transform{};          // per-layer geometric transform (scale/rotate/skew/perspective); identity default
    DisplacementEdge  transformEdge = DisplacementEdge::Blank;  // exposed-footprint policy: Blank reveals below / Stretch clamps
};

// The whole frame's draw state. The game clears() + refills `layers` each frame (clear()
// preserves capacity → arbitrary N with no steady-state heap churn). This is RUNTIME engine
// state, not ROM data — the BoundedVec fixed-cap idiom does not apply.
struct FrameDrawState {
    std::vector<DrawLayer>         layers;           // arbitrary N; compositor stable-sorts by z
    // How the frame's WHOLE-FRAME postEffects / regions combine over the composited image — the container
    // blend mode beside `Region::blend` and `DrawLayer::blend`. Normal = the alpha-over default; the other
    // modes apply applyBlendMode. (Whole-frame colour — day/night, fades, flash, tints — is an effect:
    // a ColorFill region with the blend mode and alpha the look wants; the frame carries no bespoke
    // colour member.)
    BlendMode                      blend = BlendMode::Normal;
    std::vector<ScreenSpaceEffect> postEffects;      // frame-level WHOLE-FRAME effects on the composited image
    std::vector<Region>            regions;          // frame-level confined effects; each region's effects fill its shape (optional)
};

// ── Pure helpers (headlessly unit-tested) ─────────────────────────────────────────────

[[nodiscard]] constexpr float clampAlpha(float a) noexcept {
    return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}

// The tile shader's per-layer palette-set → flat-offset uniform has a fixed slot count; a
// TileCell::palette selects slot 0..K-1. K=16 covers GB's 8 BG palettes with headroom.
inline constexpr std::size_t kPaletteSetSlots = 16;

// The tile shader's per-layer ATLAS-set → store-region uniform slot count; a
// TileCell::atlasSelect selects slot 0..K-1. K=16 mirrors kPaletteSetSlots — plenty of sheets for
// one map layer — and keeps the per-layer region uniform small. The atlasSelect FIELD is 6-bit
// (headroom to 64) but a select beyond the set resolves to slot 0, exactly like a palette select.
inline constexpr std::size_t kAtlasSetSlots = 16;

// Resolve a layer's palette set to the per-layer uSetOffsets uniform: slot i holds the palette
// flat offset of palettes[i] (a PaletteId's underlying value IS its flat offset into the palette
// store), 0 for slots beyond the set. Pure mirror of the compositor's per-layer uniform fill —
// unit-tested. An empty set yields all-zero offsets (a degenerate but valid submission); a set
// longer than K is truncated to the first K.
[[nodiscard]] constexpr std::array<std::uint32_t, kPaletteSetSlots>
paletteSetOffsets(std::span<const PaletteId> set) noexcept {
    std::array<std::uint32_t, kPaletteSetSlots> offsets{};
    const std::size_t n = std::min(set.size(), kPaletteSetSlots);
    for (std::size_t i = 0; i < n; ++i) {
        offsets[i] = static_cast<std::uint32_t>(set[i]);
    }
    return offsets;
}

// A texel coordinate in the flat palette store. The store is an arbitrary-size flat array
// of colours wrapped `storeWidth` wide into a 2-D texture.
struct PaletteTexel {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    [[nodiscard]] constexpr bool operator==(const PaletteTexel&) const noexcept = default;
};

// CPU mirror of the tile + sprite fragment shaders' palette lookup: a palette's flat offset plus a
// colour index give a flat position into the store, which wraps to the texel (flat % W, flat / W).
// The store being flat (not a row-per-palette) is what makes palettes arbitrary size — a palette may
// straddle rows, and the flat space is bounded only by the store texture's capacity. Pure + constexpr
// so the flat→2-D addressing stays identical to the shaders (the packTileCell / sampleTilemap mirror
// discipline). Precondition: storeWidth > 0.
[[nodiscard]] constexpr PaletteTexel paletteStoreTexel(std::uint32_t paletteOffset,
                                                       std::uint32_t colorIndex,
                                                       std::uint32_t storeWidth) noexcept {
    const std::uint32_t flat = paletteOffset + colorIndex;
    return PaletteTexel{flat % storeWidth, flat / storeWidth};
}

// --- Layer-key collision detection (compile-time-capable) ---

// A detected violation of layer-key uniqueness within one frame. Two distinct layers MUST
// NOT share a z (their front-to-back order would be undefined) nor a LayerId (identity must
// be unambiguous). `kind` is the identity — first member.
struct LayerKeyCollision {
    enum class Kind : std::uint8_t { DuplicateZ, DuplicateId };
    Kind         kind;    // which invariant was violated — identity, first member
    LayerId      first;   // DuplicateZ: the earlier layer's id; DuplicateId: the shared id
    LayerId      second;  // DuplicateZ: the later layer's id;   DuplicateId: == first
    std::int32_t z;       // DuplicateZ: the shared z;           DuplicateId: first layer's z
};

// Scan a layer set for the first key collision (duplicate z OR duplicate id). constexpr and
// pure, so a static_assert over a compile-time-known layer set turns a fixed layer stack's
// collision into a BUILD error — caught before the game ever runs. Returns nullopt when the
// keys are unique. O(n²); the layer population is small (compositing planes, not per-sprite
// primitives).
[[nodiscard]] constexpr std::optional<LayerKeyCollision>
findLayerKeyCollision(std::span<const DrawLayer> layers) noexcept {
    for (std::size_t i = 0; i < layers.size(); ++i) {
        for (std::size_t j = i + 1; j < layers.size(); ++j) {
            if (layers[i].id == layers[j].id) {
                return LayerKeyCollision{LayerKeyCollision::Kind::DuplicateId,
                                         layers[i].id, layers[j].id, layers[i].z};
            }
            if (layers[i].z == layers[j].z) {
                return LayerKeyCollision{LayerKeyCollision::Kind::DuplicateZ,
                                         layers[i].id, layers[j].id, layers[i].z};
            }
        }
    }
    return std::nullopt;
}

// True when no two layers share a z or a LayerId. constexpr — the static_assert seam:
//   static_assert(layerKeysAreUnique(kMyFixedLayers), "z/id collision in layer stack");
// gives compile-time detection for any layer set known at compile time.
[[nodiscard]] constexpr bool layerKeysAreUnique(std::span<const DrawLayer> layers) noexcept {
    return !findLayerKeyCollision(layers).has_value();
}

// --- Runtime draw order + collision policy ---

// How layerDrawOrder() reacts to a key collision detected at RUNTIME. Orthogonal to the
// compile-time static_assert path (layerKeysAreUnique), which is always available.
//   Throw          — throw std::invalid_argument naming the colliding keys (fail fast; the
//                    development default, so a mistake surfaces the instant its frame runs).
//   WarnAndResolve — log a warning naming the colliding keys, then return the deterministic
//                    z → submission order anyway (a shipped game stays up).
enum class LayerKeyCollisionPolicy : std::uint8_t { Throw, WarnAndResolve };

// Default runtime policy, derived from build config: dev builds fail fast; release builds
// keep a shipped game running. Overridable at the call site / on the Renderer (the toggle).
inline constexpr LayerKeyCollisionPolicy kDefaultCollisionPolicy =
#ifdef NDEBUG
    LayerKeyCollisionPolicy::WarnAndResolve;
#else
    LayerKeyCollisionPolicy::Throw;
#endif

// Back-to-front draw order as indices into `layers`: ascending z (the sole depth key — the id is
// a pure label with no ordering role), equal z falling back to submission order (stable). Returns
// indices so the caller composites without copying. Validates key uniqueness first and reacts per
// `policy` (see above); under WarnAndResolve the returned order is still fully deterministic.
// Throws std::invalid_argument on a collision under Throw.
[[nodiscard]] std::vector<std::size_t>
layerDrawOrder(std::span<const DrawLayer> layers,
               LayerKeyCollisionPolicy policy = kDefaultCollisionPolicy);

// --- Tilemap sampling math (mirrors the tile fragment shader) ---

// `outside` is set only under TileWrap::Blank, when the world coord falls outside [0, mapPx) on
// either axis (the finite-map hole the shader discards); Repeat/Clamp never set it.
struct TileSample { int tileX; int tileY; int pixelX; int pixelY; bool outside = false; };

namespace detail {
// Floor-division / floor-modulo so negative scroll wraps correctly (C++ `/` and `%`
// truncate toward zero, which would mis-wrap negative world coordinates).
[[nodiscard]] constexpr int floorDiv(int a, int b) noexcept {
    const int q = a / b;
    const int r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
[[nodiscard]] constexpr int floorMod(int a, int n) noexcept {
    const int r = a % n;
    return (r < 0) ? r + n : r;
}
}  // namespace detail

// Given an output pixel within the layer (origin top-left, before scroll), the layer's scroll, the
// tilemap dimensions, and the wrap mode, return the wrapped tile coordinate and the within-tile
// pixel offset (and, for Blank, whether the pixel is a finite-map hole). Pure + constexpr so the
// (scroll, wrap, negative-scroll) mapping is unit-testable independent of the GPU; the tile
// fragment shader runs the identical math. Precondition: tilePx > 0. Degenerate (≤0) tilemap
// dimensions yield tile coord 0 on that axis (Repeat/Clamp) or a hole (Blank) rather than dividing
// by zero. `wrap` defaults to Repeat (toroidal).
[[nodiscard]] constexpr TileSample sampleTilemap(int px, int py, LayerScroll scroll,
                                                 int widthInTiles, int heightInTiles,
                                                 TileWrap wrap = TileWrap::Repeat,
                                                 int tilePx = 8) noexcept {
    const int worldX = px + scroll.x;
    const int worldY = py + scroll.y;
    const int mapPxX = widthInTiles  * tilePx;
    const int mapPxY = heightInTiles * tilePx;

    if (wrap == TileWrap::Blank) {
        // Finite map: outside [0, mapPx) on either axis is a hole. A degenerate (≤0) dimension is
        // entirely outside, so the whole layer is a hole.
        if (widthInTiles <= 0 || heightInTiles <= 0 ||
            worldX < 0 || worldX >= mapPxX || worldY < 0 || worldY >= mapPxY) {
            return TileSample{0, 0, 0, 0, /*outside=*/true};
        }
        return TileSample{worldX / tilePx, worldY / tilePx, worldX % tilePx, worldY % tilePx, false};
    }

    if (wrap == TileWrap::Clamp) {
        // Clamp the world coord to the map's last pixel, then decompose (non-negative ⇒ truncating
        // division == floor). A degenerate dimension pins that axis to tile 0.
        const int cx = widthInTiles  > 0 ? std::clamp(worldX, 0, mapPxX - 1) : 0;
        const int cy = heightInTiles > 0 ? std::clamp(worldY, 0, mapPxY - 1) : 0;
        return TileSample{widthInTiles  > 0 ? cx / tilePx : 0,
                          heightInTiles > 0 ? cy / tilePx : 0,
                          cx % tilePx, cy % tilePx, false};
    }

    // Repeat — toroidal (the default).
    const int tileX  = widthInTiles  > 0 ? detail::floorMod(detail::floorDiv(worldX, tilePx), widthInTiles)  : 0;
    const int tileY  = heightInTiles > 0 ? detail::floorMod(detail::floorDiv(worldY, tilePx), heightInTiles) : 0;
    const int pixelX = detail::floorMod(worldX, tilePx);
    const int pixelY = detail::floorMod(worldY, tilePx);
    return TileSample{tileX, tileY, pixelX, pixelY, false};
}

}  // namespace retropp
