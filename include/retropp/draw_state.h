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

// One cell of a tilemap: which atlas tile + which palette in the layer's set + flip. The
// opaque `attributes` byte (ENG-2.B.2.a) is replaced by named fields per the no-positional-
// opacity discipline — identity is a field, never a packed byte behind a comment. `palette`
// selects which palette WITHIN the layer's set (TileContent::palettes) this cell draws from —
// the mechanism that lets a 4-entry palette render a full-colour map. `priority` (BG-over-OBJ)
// is carried here so the cell layout is final; its cross-layer interaction with sprite priority
// is realized in ENG-2.B.2.c.2 — the tile shader reads bits 0..25 only and ignores it.
struct TileCell {
    std::uint16_t tile     = 0;     // index into the layer's INDEXED tile atlas
    std::uint8_t  palette  = 0;     // which palette in the layer's set
    bool          flipX    = false;
    bool          flipY    = false;
    bool          priority = false; // BG-over-OBJ; carried — interaction realized in B.2.c.2
};

// How a tile layer's tilemap is sampled outside its [0, mapPx) bounds — the layer is no longer
// forced toroidal (ENG-2.E). Default Repeat ⇒ the pre-ENG-2.E behaviour byte-for-byte.
//   Repeat — toroidal: the map tiles infinitely on both axes (floorMod wrap). The original look.
//   Clamp  — clamp the world coord to the map's edge row/column (smear the border tile outward).
//   Blank  — FINITE map: a world coord outside [0, mapPx) on EITHER axis is a hole (transparent;
//            the layers below show through), so the map renders exactly once and can never show a
//            wrap seam. Crystal's overworld maps are finite — this is the mode they use.
// One mode governs both axes. The field lives on TileContent (it governs *tilemap* sampling; a
// sprite has no tilemap). Same blank-edge vocabulary as the transform footprint's
// DisplacementEdge::Blank — Blank discards to reveal the layers below.
enum class TileWrap : std::uint8_t { Repeat, Clamp, Blank };

// A tile layer's content: an INDEXED tile atlas (one palette index per pixel), the layer's
// palette set (the bank a cell's `palette` selects within), and a row-major tilemap
// (widthInTiles × heightInTiles). The map is sampled per-pixel in the tile shader against the
// layer's scroll, so arbitrary layer sizes and wrapping are handled on the GPU; `wrap` chooses how
// the tilemap is sampled beyond its bounds (Repeat/Clamp/Blank). `atlas`, `palettes`, and `cells`
// are game-owned; valid for the duration of the renderFrame() call that consumes them. A palette
// set of one is the single-palette case.
struct TileContent {
    AtlasId                    atlas{};         // indexed tile atlas (palette indices, not colour)
    std::span<const PaletteId> palettes;        // the layer's palette set; TileCell::palette selects within
    int                        widthInTiles  = 0;
    int                        heightInTiles = 0;
    std::span<const TileCell>  cells;           // row-major, widthInTiles * heightInTiles
    TileWrap                   wrap = TileWrap::Repeat;  // out-of-bounds sampling; Repeat = faithful (ENG-2.E)
};

// The R32_UINT tilemap cell layout the tile fragment shader unpacks:
//   [tile:16][palette:8][flipX:1][flipY:1][priority:1][reserved:5]
// This constexpr pair is the unit-tested mirror of the GPU packing — the shader unpacks the
// identical layout, so packTileCell(unpackTileCell(w)) == w for every valid cell. `priority`
// claims bit 26 (the first prior-reserved bit); the tile fragment shader still reads only bits
// 0..25, so adding the field is byte-transparent to the tile path (a priority=false cell packs
// identically to the pre-ENG-2.B.2.c layout).
[[nodiscard]] constexpr std::uint32_t packTileCell(const TileCell& c) noexcept {
    return static_cast<std::uint32_t>(c.tile)
         | (static_cast<std::uint32_t>(c.palette) << 16)
         | (static_cast<std::uint32_t>(c.flipX    ? 1u : 0u) << 24)
         | (static_cast<std::uint32_t>(c.flipY    ? 1u : 0u) << 25)
         | (static_cast<std::uint32_t>(c.priority ? 1u : 0u) << 26);
}

[[nodiscard]] constexpr TileCell unpackTileCell(std::uint32_t packed) noexcept {
    TileCell c;
    c.tile     = static_cast<std::uint16_t>(packed & 0xFFFFu);
    c.palette  = static_cast<std::uint8_t>((packed >> 16) & 0xFFu);
    c.flipX    = ((packed >> 24) & 1u) != 0u;
    c.flipY    = ((packed >> 25) & 1u) != 0u;
    c.priority = ((packed >> 26) & 1u) != 0u;
    return c;
}

// ── Sprite content ────────────────────────────────────────────────────────────────────

// A sprite's pixel dimensions are an AssetDimensions (geometry.h) — the same type the atlas slicer
// carves an image into (ENG-2.G), with the console-named presets (AssetDimensions::GameBoy8x8, …).

// One placed sprite. `x`/`y` are the top-left in the LAYER's coordinate space (before scroll —
// the vertex shader subtracts the layer scroll, so a sprite on a world-scroll layer tracks the
// background, and a HUD layer at scroll {0,0} stays fixed). `tile` is the top-left atlas cell
// (8px grid); the sprite reads a size.width × size.height pixel rectangle from the atlas at that
// cell's pixel origin (so a 16×16 sprite spans a 2×2 cell block laid out contiguously). `palette`
// selects which palette WITHIN the layer's set this sprite colours through. `priority` (behind-BG)
// is carried here; its cross-layer interaction is realized in ENG-2.B.2.c.2 (B.2.c.1 front-
// composites all sprites by layer z). Identity is the named fields — no packed attribute byte.
//
// `transform` (ENG-2.D.2) is the sprite's own geometric transform, applied in SPRITE-LOCAL pixel
// space — the [0, size.width] × [0, size.height] rectangle of the sprite's own art — about whatever
// pivot the caller encoded (the engine imposes no default; rotate an 8×8 about its centre with
// Transform::rotation(deg, 4, 4)). It composes with the layer's own DrawLayer::transform: a sprite
// quad goes sprite.transform first, then the layer transform, exactly as a tile layer's content does.
// Identity default → byte-for-byte the pre-D.2 axis-aligned quad. (Flips stay a fragment UV op,
// independent of the geometry — a flipped+rotated sprite mirrors its texture and rotates its quad.)
struct Sprite {
    int             x       = 0;
    int             y       = 0;
    AssetDimensions size    = AssetDimensions::GameBoy8x8;
    std::uint16_t   tile    = 0;       // top-left atlas cell
    std::uint8_t  palette   = 0;       // palette-select within the layer's set
    bool          flipX     = false;
    bool          flipY     = false;
    bool          priority  = false;   // behind-BG; carried — interaction realized in B.2.c.2
    Transform     transform{};         // per-sprite geometric transform, sprite-local space; identity default (D.2)
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
//          can't express alongside Vulkan's descriptor layout). See PLAN Amendment A2 (B.2.c.1).
//          The bottom row (m20, m21) carries the perspective terms — non-zero ⇒ the per-vertex w
//          varies ⇒ the GPU perspective-divides and interpolates the within-sprite UV perspective-
//          correct for free; zero ⇒ the affine case (w ≡ 1), reproducing the pre-D.2 quad exactly.
//          (ENG-2.D.2 generalizes the pre-D.2 axis-aligned (clipX,clipY,clipW,clipH) rect — that was
//          the degenerate affine case of this homography. PLAN §2 Q4: the full 3×3 supersedes the
//          partition's affine "origin + two edge vectors", which could not carry perspective.)
//   attr = (tile, paletteRow, flags, size): `paletteRow` is the RESOLVED palette-store row
//          (resolved CPU-side from the layer's set + the sprite's select); `flags` is packSpriteFlags;
//          `size` is the pixel size packed (width<<16)|height for the fragment's within-sprite
//          addressing. This is the unit-tested CPU↔GPU mirror, same discipline as packTileCell.
struct GpuSprite {
    float         row0[4];       // H row 0: m00 m01 m02 _   (unit-quad corner → clip homography)
    float         row1[4];       // H row 1: m10 m11 m12 _
    float         row2[4];       // H row 2: m20 m21 m22 _   (m20,m21 = perspective; w = m20·x + m21·y + m22)
    std::uint32_t tile;          // top-left atlas cell
    std::uint32_t paletteRow;    // resolved palette-store row
    std::uint32_t flags;         // bit0 flipX | bit1 flipY | bit2 priority
    std::uint32_t size;          // pixel size packed (width<<16)|height
};
static_assert(sizeof(GpuSprite) == 64);

[[nodiscard]] constexpr std::uint32_t packSpriteFlags(bool flipX, bool flipY, bool priority) noexcept {
    return (flipX ? 1u : 0u) | (flipY ? 2u : 0u) | (priority ? 4u : 0u);
}

// Pack an asset's pixel dimensions into one uint (width in the high 16 bits). The fragment shader
// unpacks this to map the interpolated within-sprite UV back to an atlas pixel.
[[nodiscard]] constexpr std::uint32_t packAssetSize(const AssetDimensions& sz) noexcept {
    return (static_cast<std::uint32_t>(sz.width) << 16) | static_cast<std::uint32_t>(sz.height & 0xFFFF);
}

// Resolve a sprite's palette-select to a palette-store row via the layer's set (mirrors the tile
// path's paletteSetRows mapping; a PaletteId's underlying value == its store row). An out-of-range
// select or an empty set resolves to row 0 (degenerate but valid).
[[nodiscard]] constexpr std::uint32_t spritePaletteRow(std::span<const PaletteId> set,
                                                       std::uint8_t select) noexcept {
    return select < set.size() ? static_cast<std::uint32_t>(set[select]) : 0u;
}

// Build the GPU record for one sprite. `viewportW`/`viewportH` are the offscreen viewport pixel
// size; `scrollX`/`scrollY` the layer scroll; `layerTransform` the per-layer DrawLayer::transform
// (D.1). The composed clip-space homography is baked here so the vertex shader is a pure storage-
// buffer read (no uniform). Pure + constexpr — the unit-tested CPU↔GPU mirror.
//
// The chain a unit-quad corner (cx, cy) travels (ENG-2.D.2 §2), via the constexpr Transform::then():
//   H = scale(w, h)                    // unit corner → sprite-local content pixel
//         .then(s.transform)           // per-sprite transform, sprite-local space (about its own pivot)
//         .then(translation(sox, soy)) // scrolled screen top-left  (sox = x − scrollX, soy = y − scrollY)
//         .then(layerTransform)        // per-layer transform, viewport-pixel space (D.1)
//         .then(screenToClip)          // viewport scale + top-left-origin V-flip
// Scroll is subtracted BEFORE the layer transform — matching the D.1 tile path, where the layer
// transform maps (world − scroll) to the destination — so a tile layer and a sprite layer carrying
// the same Transform line up and share the same pivot space. With identity sprite + layer transforms
// H reduces algebraically to the pre-D.2 (clipX + cx·clipW, clipY + cy·clipH), w ≡ 1 — byte-identical.
[[nodiscard]] constexpr GpuSprite makeGpuSprite(const Sprite& s, std::uint32_t paletteRow,
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
    g.tile       = s.tile;
    g.paletteRow = paletteRow;
    g.flags      = packSpriteFlags(s.flipX, s.flipY, s.priority);
    g.size       = packAssetSize(s.size);
    return g;
}

// A layer carries exactly one content alternative. The active alternative is the variant's
// identity; LayerContentKind mirrors it for explicit, switch-friendly dispatch.
enum class LayerContentKind : std::uint8_t { Tiles, Sprites };
using LayerContent = std::variant<TileContent, SpriteContent>;
[[nodiscard]] constexpr LayerContentKind contentKind(const LayerContent& c) noexcept {
    return c.index() == 0 ? LayerContentKind::Tiles : LayerContentKind::Sprites;
}

// ── Effect region — the shape an effect is confined to (ENG-2.F) ───────────────────────

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

// The shape an effect is confined to (ENG-2.F). A polygon of ordered VIEWPORT-PIXEL vertices (ANY
// count — `points` is unbounded), inflated by `radius` (a signed-distance rounding), warped by
// `transform`. The points ARE the position — there is no separate origin. Containment is an SDF, not a
// rasterized polygon, so one type covers every shape (see regionContains / sdPolygon in postprocess.h):
//   empty                → NO region: the effect covers the whole viewport (the byte-identical default).
//   1 point + radius     → a CIRCLE (distance-to-point ≤ radius).
//   2 points + radius    → a CAPSULE / stadium (distance-to-segment ≤ radius).
//   ≥ 3 points, radius 0 → a sharp polygon (triangle / quad / N-gon; arbitrary CONCAVE outlines OK).
//   ≥ 3 points, radius>0 → a rounded polygon.
// `transform` (identity default) is an ENG-2.D Transform composed on top — scale / stretch (non-uniform
// scale) / skew / rotate / perspective / translate the placed shape, evaluated by the same inverse-
// homography the tile path uses. Move a shape by rewriting points OR via transform translation.
//
// The vertices are a std::vector (NO fixed cap — truly complex shapes are supported); the renderer
// uploads them to a per-frame fragment storage buffer, exactly as it uploads sprite records and tilemap
// cells. The presets are the Transform::rotation() named-constructor idiom (a placed shape is
// parametric); a raw ShapePoints{ .points = {...}, .radius = r } stays allowed for the unnamed.
struct ShapePoints {
    std::vector<Point> points;          // ordered viewport-pixel vertices; empty = no region
    float              radius = 0.0f;   // SDF inflation: 0 = sharp polygon edges
    Transform          transform{};     // additional warp, identity default

    [[nodiscard]] bool operator==(const ShapePoints&) const = default;
    [[nodiscard]] bool hasRegion() const noexcept { return !points.empty(); }

    // Named-constructor presets (the Transform::rotation() idiom). All coordinates are viewport pixels.
    [[nodiscard]] static ShapePoints circle(Point c, float r);
    [[nodiscard]] static ShapePoints capsule(Point a, Point b, float r);
    [[nodiscard]] static ShapePoints triangle(Point a, Point b, Point c);
    [[nodiscard]] static ShapePoints rectangle(Point topLeft, float w, float h);
    [[nodiscard]] static ShapePoints roundedRectangle(Point topLeft, float w, float h, float r);
    [[nodiscard]] static ShapePoints regularPolygon(Point c, float r, int sides);
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

// ── Screen-space effects (type seam locked here; shader realization is ENG-2.C) ───────

enum class Axis : std::uint8_t { Horizontal, Vertical };

enum class ScreenSpaceEffectKind : std::uint8_t {
    None,            // pass-through — the ENG-2.B.2.a default; realization is ENG-2.C
    RowDisplacement, // wavy water / heat haze / per-line SCX = f(row, time) in a shader
    Ripple,          // radial concentric ripple — a water droplet; built-in (ENG-2.I.a)
    Custom,          // a game-registered shader (ENG-2.C.3) — see PostProcessStageId + .customShader
};

// The engine's BUILT-IN effect library (ENG-2.I.a) is the set of ScreenSpaceEffectKinds the engine
// owns a shader for — today RowDisplacement (the axis-aligned wave) and Ripple (the radial droplet).
// A game sets `.kind` on a ScreenSpaceEffect and fills the fields that kind consults (plain designated-
// init — every field is settable inline); the engine supplies the shader. No registration, no shader
// authoring — that is the Custom path. New built-ins land behind this enum; the candidate menu is
// docs/effect-library-roadmap.md.
// Which fields each built-in consults (the rest stay at their defaults, ignored):
//   RowDisplacement → amplitude, frequency, phase, axis, edge
//   Ripple          → amplitude, frequency, phase, center, decay
//   Custom          → none of the above — the game's own shader + uniform define the behaviour
// (scope/region apply to EVERY kind: they are compositing decisions the engine makes, not the shader's.)

// A handle to a game-registered custom shader stage the renderer owns (ENG-2.C.3 / Issue 5).
// Identity is the typed handle, mirroring AtlasId/PaletteId; the renderer maps it to the pipeline
// pair it built from the game's fragment in registerPostProcessStage(). A custom shader is a
// first-class effect KIND: an effect with kind == Custom carries one of these in .customShader and
// runs through the SAME per-layer (Layer/Below) and frame-level (postEffects) machinery the built-in
// effects use — wherever a built-in effect works, a custom one does too.
enum class PostProcessStageId : std::uint32_t {};

// What a displacement does at the frame edge, where a row/column pulled inward exposes a strip with
// no source pixel behind it. Developer-selectable per effect (ENG-2.C.2.a Amendment A3):
//   Blank   — the exposed strip is the backdrop colour (nothing there). The faithful default: a
//             whole-frame displacement has no off-screen content to reveal, so it shows blank.
//   Stretch — the edge pixel is duplicated outward (CLAMP_TO_EDGE), smearing the border colour.
enum class DisplacementEdge : std::uint8_t { Blank, Stretch };

// Which pixels a per-layer effect transforms — the composable Photoshop-layer model (ENG-2.C.2.b).
// (Meaningful for DrawLayer::effect; FrameDrawState::postEffects is inherently whole-frame and
// ignores it.)
//   Layer — ISOLATED: displace ONLY this layer's own content, before it composites. A wavy water
//           layer distorts while the layers/sprites composited above it stay still. The default.
//   Below — ADJUSTMENT LAYER: displace the WHOLE accumulated image at this layer's z — this layer's
//           own content AND everything beneath it, coherently — then layers above this z composite
//           on top, undisplaced. A content-less Below layer just under a HUD wobbles the world while
//           the HUD rides steady; a content-bearing Below layer wobbles itself together with the
//           scene beneath. Multiple Below effects compose by z.
enum class ScreenSpaceEffectScope : std::uint8_t { Layer, Below };

// A screen-space effect declaration. ENG-2.B.2.a locks the type + carries the parameters
// as data; the shader stage that interprets them is ENG-2.C / Issue 5. In screen space the
// fragment's row coordinate IS the scanline, so a continuous effect is a function
// f(row, time, frame-state) the GPU evaluates per-pixel — no reconstructed LY counter, no
// HBlank ISR. The game advances `phase` per frame to animate (runtime-dynamic).
struct ScreenSpaceEffect {
    ScreenSpaceEffectKind kind = ScreenSpaceEffectKind::None;  // identity, first member
    float amplitude = 0.0f;   // displacement magnitude, viewport pixels (RowDisplacement)
    float frequency = 0.0f;   // cycles across the displaced axis (RowDisplacement)
    float phase     = 0.0f;   // animation phase (game advances off frame time) (RowDisplacement)
    Axis  axis      = Axis::Horizontal;
    DisplacementEdge edge = DisplacementEdge::Blank;  // frame-edge behaviour; Blank is the default
    ScreenSpaceEffectScope scope = ScreenSpaceEffectScope::Layer;  // per-layer reach; Layer (isolated) default

    // kind == Ripple (ENG-2.I.a) only — ignored otherwise. A RADIAL concentric displacement (a water
    // droplet): each fragment's sample is pushed ALONG THE RADIUS from `center` by sin(2π·(frequency·dist
    // − phase)), the crest expanding outward as the game advances `phase`, faded by exp(−decay·dist).
    // Ripple REUSES amplitude (displacement magnitude, viewport px), frequency (rings across the field),
    // and phase (game-advanced, slow). `center` is in VIEWPORT PIXELS (like Point / Sprite::x,y — the
    // engine normalizes it to UV in rippleParams, and aspect-corrects so the rings stay circular);
    // `decay` is the radial falloff rate (0 = rings do not fade with radius).
    Point center{};
    float decay = 0.0f;

    // kind == Custom (ENG-2.C.3) only — ignored otherwise. `customShader` is the handle from
    // Renderer::registerPostProcessStage; `uniform` is the game's per-pass uniform bytes (opaque to
    // the engine, pushed verbatim to the fragment's b0/space3 cbuffer). The game owns the uniform
    // storage — the span must outlive the renderFrame() call that consumes it, like the layer content
    // spans. Its size must equal the size declared at registration (a multiple of 16; see
    // uniformSizeIsValid). The amplitude/frequency/phase/edge fields above are NOT consulted for a
    // Custom effect — the game's own shader + uniform define its behaviour; only `scope` still applies
    // (Layer vs Below), since that is a compositing decision the engine makes, not the shader.
    PostProcessStageId         customShader{};
    std::span<const std::byte> uniform{};

    // The shape the effect is confined to (ENG-2.F). Default (count == 0) ⇒ no region ⇒ the effect
    // covers its whole scope, byte-for-byte identical to pre-ENG-2.F. A non-empty region gates the
    // effect engine-side (a region_select compositor pass), identically for built-in and Custom kinds —
    // the custom-shader contract is untouched. Every other field above applies, unchanged, INSIDE the
    // shape; outside, the source passes through. Screen-space (viewport pixels); see ShapePoints.
    ShapePoints region{};
};

// ── Frame-level modifiers (types locked here; output realization is ENG-2.B.2.c) ──────

enum class ColorModifierKind : std::uint8_t { None, MultiplyAdd };

// Day/night-style whole-frame colour transform: out = clamp(in * mul + add). Default is the
// identity transform (None / mul=1 / add=0).
struct ColorModifier {
    ColorModifierKind kind = ColorModifierKind::None;  // identity, first member
    float mulR = 1.0f, mulG = 1.0f, mulB = 1.0f;
    float addR = 0.0f, addG = 0.0f, addB = 0.0f;
};

enum class BlendKind : std::uint8_t { None, Flash };

// Frame-level blend (cutscene flash): mix the composed frame toward (r,g,b) by `strength`.
struct Blend {
    BlendKind kind = BlendKind::None;  // identity, first member
    float r = 0.0f, g = 0.0f, b = 0.0f, strength = 0.0f;
};

// The blit-stage transform a frame's globalModifier + blend resolve to: a whole-frame post-
// composite transform on already-composited pixels — out = clamp(in*mul + add), then mix toward
// (flashR,flashG,flashB) by flashStrength. Default is the IDENTITY (mul=1, add=0, strength=0) →
// the blit is byte-identical to the ENG-2.B.2.c.1 pass-through. The unit-tested CPU mirror of the
// blit fragment shader's math (discipline of packTileCell / makeGpuSprite). NOT the colouring
// mechanism (that is index + palette) — this is the modern post-composite effect transform for
// whole-frame fades / day-night / cutscene flash (ENGINE_DECISIONS.md § "Colour model").
struct FrameColorTransform {
    float mulR = 1.0f, mulG = 1.0f, mulB = 1.0f;
    float addR = 0.0f, addG = 0.0f, addB = 0.0f;
    float flashR = 0.0f, flashG = 0.0f, flashB = 0.0f;
    float flashStrength = 0.0f;
    [[nodiscard]] constexpr bool operator==(const FrameColorTransform&) const noexcept = default;
};

// Resolve a frame's ColorModifier + Blend to the blit-stage transform. None on either input
// leaves that part identity, so a default frame yields FrameColorTransform{} (the identity) and
// the blit renders the faithful baseline byte-for-byte. flashStrength is clamped to [0,1] here so
// the GPU receives a valid value and this helper stays the authoritative mirror.
[[nodiscard]] constexpr FrameColorTransform
frameColorTransform(const ColorModifier& m, const Blend& b) noexcept {
    FrameColorTransform t;  // identity
    if (m.kind == ColorModifierKind::MultiplyAdd) {
        t.mulR = m.mulR; t.mulG = m.mulG; t.mulB = m.mulB;
        t.addR = m.addR; t.addG = m.addG; t.addB = m.addB;
    }
    if (b.kind == BlendKind::Flash) {
        t.flashR = b.r; t.flashG = b.g; t.flashB = b.b;
        t.flashStrength = b.strength < 0.0f ? 0.0f : (b.strength > 1.0f ? 1.0f : b.strength);
    }
    return t;
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
    LayerContent      content{ TileContent{} };
    ScreenSpaceEffect effect{};             // per-layer; None by default
    Transform         transform{};          // per-layer geometric transform (scale/rotate/skew/perspective); identity default (D.1)
    DisplacementEdge  transformEdge = DisplacementEdge::Blank;  // exposed-footprint policy: Blank reveals below / Stretch clamps (D.1)
};

// The whole frame's draw state. The game clears() + refills `layers` each frame (clear()
// preserves capacity → arbitrary N with no steady-state heap churn). This is RUNTIME engine
// state, not ROM data — the BoundedVec fixed-cap idiom does not apply.
struct FrameDrawState {
    std::vector<DrawLayer>         layers;           // arbitrary N; compositor stable-sorts by z
    ColorModifier                  globalModifier{}; // day/night; None by default (B.2.c)
    Blend                          blend{};          // cutscene flash; None by default (B.2.c)
    std::vector<ScreenSpaceEffect> postEffects;      // frame-level; carried, realized in 2.C
};

// ── Pure helpers (headlessly unit-tested) ─────────────────────────────────────────────

[[nodiscard]] constexpr float clampAlpha(float a) noexcept {
    return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}

// The tile shader's per-layer palette-set → store-row uniform has a fixed slot count; a
// TileCell::palette selects slot 0..K-1. K=16 covers GB's 8 BG palettes with headroom. A
// palette set larger than K (resolved via a storage buffer) is a forward option, not built
// now — cells can only select 0..K-1.
inline constexpr std::size_t kPaletteSetSlots = 16;

// Resolve a layer's palette set to the per-layer uSetRows uniform: slot i holds the palette-
// store row of palettes[i] (a PaletteId's underlying value IS its store row), 0 for slots
// beyond the set. Pure mirror of the compositor's per-layer uniform fill — unit-tested. An
// empty set yields all-zero rows (a degenerate but valid submission); a set longer than K is
// truncated to the first K.
[[nodiscard]] constexpr std::array<std::uint32_t, kPaletteSetSlots>
paletteSetRows(std::span<const PaletteId> set) noexcept {
    std::array<std::uint32_t, kPaletteSetSlots> rows{};
    const std::size_t n = std::min(set.size(), kPaletteSetSlots);
    for (std::size_t i = 0; i < n; ++i) {
        rows[i] = static_cast<std::uint32_t>(set[i]);
    }
    return rows;
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
// by zero. `wrap` defaults to Repeat ⇒ byte-identical to the pre-ENG-2.E mapping.
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

    // Repeat — toroidal (the default; byte-identical to the pre-ENG-2.E mapping).
    const int tileX  = widthInTiles  > 0 ? detail::floorMod(detail::floorDiv(worldX, tilePx), widthInTiles)  : 0;
    const int tileY  = heightInTiles > 0 ? detail::floorMod(detail::floorDiv(worldY, tilePx), heightInTiles) : 0;
    const int pixelX = detail::floorMod(worldX, tilePx);
    const int pixelY = detail::floorMod(worldY, tilePx);
    return TileSample{tileX, tileY, pixelX, pixelY, false};
}

}  // namespace retropp
