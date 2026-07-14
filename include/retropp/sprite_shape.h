#pragma once

#include <cstdint>
#include <vector>

#include "retropp/draw_state.h"  // Sprite, Point, ShapePoints, Space, Rotation
#include "retropp/geometry.h"    // IntRect, AssetDimensions
#include "retropp/image.h"       // AtlasArt, LoadedImage, TransparentIndices, ShapeTrace
#include "retropp/transform.h"   // Transform (the quad<->layer homography)

// The sprite shape query — a sprite's own silhouette, offered in three forms along one axis of ownership
// and one of fidelity, in whichever coordinate Space you ask for:
//   sprite.asShape(sheet, space)          → SpriteShape       — a BORROW: exact, live, non-owning (frame life)
//   sprite.freeze(sheet, space)           → FrozenSpriteShape — OWNED: an exact snapshot, detached, storable
//   sprite.approximate(sheet, n, space)   → ShapePoints       — OWNED: a coarse ≤ n-point polygon (a real shape)
// The exact forms answer contains(point) with one coverage read (no polygon). The coarse form is an
// ordinary ShapePoints, so it drops straight into a Region / stencil() / physics that already speak
// polygons. Every form reads the sprite's CURRENT tile — re-query after a frame change. Pure CPU: no GPU,
// no renderer, tick-state only (the same discipline the anchors follow).

namespace retropp {

// ── ArtMask — the binary visibility of one sprite cell, in ART space ─────────────────────────────
// One bit per art pixel of a sprite's cell: 1 = visible (the palette index is not a structural hole). It
// is the device-free coverage the whole query reads — traced to a polygon (traceSilhouette), snapshotted
// (freeze), or sampled point-by-point (asShape). Build it from a sheet's retained AtlasArt + a cell, or
// straight from a decoded LoadedImage + a cell rect (the pure headless route, no renderer needed).
struct ArtMask {
    int                       width  = 0;
    int                       height = 0;
    std::vector<std::uint8_t> visible;  // 1 = visible, row-major (width * height)

    [[nodiscard]] bool at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return false;
        return visible[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)] != 0;
    }
    [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
};

// The pixel rect of a sprite's cell within its sheet — the inverse of the atlas slicer's tile math: `tile`
// is the top-left 8px cell index, so its column/row on the sheet's cell grid give the pixel origin, and
// `size` gives the extent. A degenerate sheet width yields an empty rect.
[[nodiscard]] IntRect spriteCellRect(int sheetWidth, std::uint16_t tile, AssetDimensions size) noexcept;

// Build the ArtMask for a cell. The AtlasArt overload is the ergonomic post-upload route (the AtlasManifest
// hands over the retained art); the LoadedImage overload is the pure headless one. Both apply the same
// visibility rule (the index is not a structural hole). A cell that falls outside the sheet masks the
// in-bounds part and treats the rest as not visible.
[[nodiscard]] ArtMask artMask(const AtlasArt& art, std::uint16_t tile, AssetDimensions size);
[[nodiscard]] ArtMask artMask(const LoadedImage& img, IntRect cell, TransparentIndices transparent);

// Trace a mask's OUTER silhouette to a polygon of at most `maxPoints` vertices, in ART pixels. The boundary
// is the marching-squares outer contour (interior holes are bridged — outer boundary only; disconnected
// blobs merge through their common convex hull). Simplification honours `trace`: Conservative keeps the
// silhouette CONTAINED at every budget (only ever adding area, degenerating toward the hull then the box);
// Balanced hugs it minimax-tight. Returns {} for an empty / fully-transparent mask. Throws
// std::invalid_argument when maxPoints < 3.
[[nodiscard]] std::vector<Point> traceSilhouette(const ArtMask& mask, int maxPoints,
                                                 ShapeTrace trace = ShapeTrace::Conservative);

// ── The exact silhouette forms — coverage-queryable, GPU-free ────────────────────────────────────
//
// A silhouette query needs to map a Quad-space or Layer-space point back to an art pixel and read coverage.
// The map is captured as one homography: the whole (x, y) + (pivot − origin) + transform · (p − pivot)
// chain a sprite's toLayer() applies composes into a single quad→layer Transform, so Layer queries invert
// it and Quad queries skip it. A projection behind the camera plane gets no special guard — the same
// exposure Sprite::anchor(k, Space::Layer) documents.

// The forward quad→layer homography for a sprite (M with sprite.toLayer(p) == M · p). freeze() captures it
// so the snapshot is self-contained; the borrow computes it live.
[[nodiscard]] Transform spriteQuadToLayer(const Sprite& sprite) noexcept;

// A sprite's silhouette as a BORROW — a non-owning view over the sprite and its sheet art, valid only while
// BOTH outlive it (immediate-mode / tick lifetime, like a span). It reads the sprite's live coverage on
// demand, so it tracks the sprite's flip / rotation / transform / placement for free. To keep a silhouette
// past the frame, freeze() it. The Space is baked in at construction (asShape(sheet, space)); every answer
// is in that space.
struct SpriteShape {
    const Sprite*   sprite = nullptr;   // borrowed — must outlive this view
    const AtlasArt* art    = nullptr;   // borrowed — the sheet the coverage is read from
    Space           space  = Space::Quad;

    // Is `p` (in this shape's Space) inside the silhouette? One exact coverage read: the point maps back to
    // an art pixel and the pixel's visibility is the answer. Outside the art (or a fully-transparent pixel)
    // is false. O(1) — no polygon.
    [[nodiscard]] bool contains(Point p) const;

    // A conservative axis-aligned bounds of the silhouette, in this shape's Space (Quad: exact art-pixel
    // extent under orientation; Layer: the placed extent, outward-rounded to whole pixels). Empty rect for
    // a fully-transparent tile. A cheap broad-phase before contains().
    [[nodiscard]] IntRect bounds() const;
};

// A sprite's silhouette as an OWNED snapshot — it copies the coverage mask and the sprite's placement at
// freeze() time, so it answers contains() / bounds() long after the sprite (and its sheet) are gone. The
// owning peer of SpriteShape; mint it when a silhouette must be stored (a trail, a history buffer, a
// collider cached across frames). Detached: it never reads the sprite again.
struct FrozenSpriteShape {
    ArtMask   mask;                        // owned coverage (cell-local art space)
    Rotation  rotation = Rotation::None;   // the orientation ops the mask is read under
    bool      flipX = false;
    bool      flipY = false;
    Space     space  = Space::Quad;
    Transform quadToLayer{};               // the captured placement (identity when frozen in Quad space)

    [[nodiscard]] bool    contains(Point p) const;
    [[nodiscard]] IntRect bounds() const;
};

}  // namespace retropp
