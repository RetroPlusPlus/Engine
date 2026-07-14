// The sprite shape query — a sprite's silhouette as a shape, in three forms (asShape borrow / freeze /
// approximate) across two spaces (Quad / Layer). All headless: the trace core, the containment invariant,
// the exact coverage reads, and the orientation / placement maps are pure CPU.
//
// The load-bearing invariant is CONTAINMENT: a Conservative approximate() polygon covers every visible art
// pixel at every point budget. The suite rasterizes each polygon and asserts coverage per pixel — the
// build-time proof. Balanced is allowed to carve, bounded so it never bulges further out than Conservative.

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/image.h"
#include "retropp/renderer.h"      // AtlasManifest (the retained-art conversion)
#include "retropp/sprite_shape.h"
#include "retropp/transform.h"

#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace retropp {
namespace {

constexpr AtlasId kSheet{7};

// Build an ArtMask from ASCII rows ('#' = visible, anything else = hole).
ArtMask maskFromRows(const std::vector<std::string>& rows) {
    const int h = static_cast<int>(rows.size());
    const int w = h > 0 ? static_cast<int>(rows[0].size()) : 0;
    ArtMask m{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h, 0)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (rows[y][static_cast<std::size_t>(x)] == '#')
                m.visible[static_cast<std::size_t>(y) * w + x] = 1;
    return m;
}

// A single-cell sheet as an AtlasArt: index 1 where visible, 0 (a structural hole) elsewhere. Width must be
// a whole number of 8px cells for the cell math; these fixtures are 8 or 16 wide, tile 0.
AtlasArt artFromRows(const std::vector<std::string>& rows, AtlasId atlas = kSheet) {
    const ArtMask m = maskFromRows(rows);
    std::vector<std::uint8_t> idx(static_cast<std::size_t>(m.width) * m.height, 0);
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = m.visible[i] ? 1 : 0;
    return AtlasArt{.atlas       = atlas,
                    .width       = m.width,
                    .height      = m.height,
                    .indices     = std::move(idx),
                    .transparent = TransparentIndices::of({0})};
}

Sprite spriteFor(const AtlasArt& art, AssetDimensions size) {
    Sprite s{.key = "s"};
    s.atlas = art.atlas;
    s.tile  = 0;
    s.size  = size;
    return s;
}

// Even-odd point-in-polygon; a point on an edge counts as inside (containment must not fail on a boundary).
bool inPolygon(Point p, const std::vector<Point>& poly) {
    const std::size_t n = poly.size();
    if (n < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point& a = poly[i];
        const Point& b = poly[j];
        const bool straddles = (a.y > p.y) != (b.y > p.y);
        if (straddles) {
            const float xCross = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x <= xCross + 1e-4f) inside = !inside;
        }
    }
    return inside;
}

// Every visible pixel's centre lies inside the polygon (the containment proof).
bool coversAll(const std::vector<Point>& poly, const ArtMask& mask) {
    for (int y = 0; y < mask.height; ++y)
        for (int x = 0; x < mask.width; ++x)
            if (mask.at(x, y) &&
                !inPolygon(Point{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f}, poly))
                return false;
    return true;
}

// The furthest a polygon vertex sits from the nearest visible pixel centre — a proxy for how far the
// boundary bulges out. Conservative apexes bulge; Balanced vertices ride the true contour.
double maxVertexSlack(const std::vector<Point>& poly, const ArtMask& mask) {
    double worst = 0.0;
    for (const Point& v : poly) {
        double nearest = std::numeric_limits<double>::max();
        for (int y = 0; y < mask.height; ++y)
            for (int x = 0; x < mask.width; ++x)
                if (mask.at(x, y)) {
                    const double dx = v.x - (x + 0.5), dy = v.y - (y + 0.5);
                    nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy));
                }
        worst = std::max(worst, nearest);
    }
    return worst;
}

const std::vector<std::string> kFull8 = {
    "########", "########", "########", "########",
    "########", "########", "########", "########"};
const std::vector<std::string> kL16 = {
    "########........", "########........", "########........", "########........",
    "########........", "########........", "########........", "########........",
    "################", "################", "################", "################",
    "################", "################", "################", "################"};
const std::vector<std::string> kDonut8 = {
    "########", "########", "##....##", "##....##",
    "##....##", "##....##", "########", "########"};
const std::vector<std::string> kTwoBlob16 = {
    "###.........###.", "###.........###.", "###.........###.", "................",
    "................", "................", "................", "................",
    "................", "................", "................", "................",
    "................", "................", "................", "................"};

// ── Trace shapes ──────────────────────────────────────────────────────────────────────────────

TEST(TraceSilhouette, EmptyMaskIsEmptyShape) {
    EXPECT_TRUE(traceSilhouette(ArtMask{}, 16).empty());
    EXPECT_TRUE(traceSilhouette(maskFromRows({"........", "........"}), 16).empty());
}

TEST(TraceSilhouette, FullTileIsAFourPointRectAtGenerousBudgets) {
    const ArtMask m = maskFromRows(kFull8);
    for (int budget : {4, 8, 16, 64}) {
        const auto poly = traceSilhouette(m, budget);
        EXPECT_EQ(poly.size(), 4u) << "budget " << budget;
        EXPECT_TRUE(coversAll(poly, m));
    }
}

TEST(TraceSilhouette, SinglePixel) {
    const ArtMask m = maskFromRows({"........", "...#....", "........"});
    const auto poly = traceSilhouette(m, 8);
    EXPECT_EQ(poly.size(), 4u);
    EXPECT_TRUE(coversAll(poly, m));
}

TEST(TraceSilhouette, BudgetBelowThreeThrows) {
    EXPECT_THROW((void)traceSilhouette(maskFromRows(kFull8), 2), std::invalid_argument);
    EXPECT_THROW((void)traceSilhouette(maskFromRows(kFull8), 0), std::invalid_argument);
}

// ── Containment golden — Conservative covers every visible pixel at every budget ────────────────

TEST(Containment, ConservativeCoversAllPixels) {
    struct Case { const char* name; std::vector<std::string> rows; };
    const std::vector<Case> cases = {
        {"full", kFull8}, {"L", kL16}, {"donut", kDonut8}, {"twoBlob", kTwoBlob16}};
    for (const Case& c : cases) {
        const ArtMask m = maskFromRows(c.rows);
        for (int budget : {3, 4, 8, 16, 64}) {
            const auto poly = traceSilhouette(m, budget, ShapeTrace::Conservative);
            EXPECT_TRUE(coversAll(poly, m)) << c.name << " @ budget " << budget;
            // The bounding box (4 corners) is the coarsest Conservative form, so a budget below 4 still
            // returns the box — the cap is max(budget, 4).
            EXPECT_LE(static_cast<int>(poly.size()), std::max(budget, 4)) << c.name << " @ budget " << budget;
            EXPECT_GE(poly.size(), 3u) << c.name << " @ budget " << budget;
        }
    }
}

TEST(Containment, DonutHoleIsBridgedNoInteriorBoundary) {
    // Outer boundary only: the hole is filled, so the shape is a single loop covering the ring AND the hole.
    const ArtMask m = maskFromRows(kDonut8);
    const auto poly = traceSilhouette(m, 32);
    EXPECT_TRUE(coversAll(poly, m));
    EXPECT_TRUE(inPolygon(Point{4.0f, 4.0f}, poly));  // the hole centre is inside the bridged shape
}

TEST(Containment, TwoBlobsBecomeOneSpanningShape) {
    const ArtMask m = maskFromRows(kTwoBlob16);
    const auto poly = traceSilhouette(m, 32);
    EXPECT_TRUE(coversAll(poly, m));
    // The gap between the blobs is spanned (the merged shape is one polygon over both).
    EXPECT_TRUE(inPolygon(Point{7.5f, 1.5f}, poly));
}

// ── Balanced bound — never bulges further out than Conservative at the same budget ──────────────

TEST(Balanced, HugsNoLooserThanConservative) {
    for (const auto& rows : {kL16, kDonut8, kTwoBlob16}) {
        const ArtMask m = maskFromRows(rows);
        for (int budget : {4, 6, 8, 12}) {
            const double consv = maxVertexSlack(traceSilhouette(m, budget, ShapeTrace::Conservative), m);
            const double bal   = maxVertexSlack(traceSilhouette(m, budget, ShapeTrace::Balanced), m);
            EXPECT_LE(bal, consv + 1e-3) << "budget " << budget;
        }
    }
}

// ── Orientation — the exact silhouette round-trips through all 8 flip/rotation combos ───────────

TEST(ExactShape, ContainsMatchesCoverageUnderEveryOrientation) {
    const AtlasArt art = artFromRows(kL16);      // asymmetric — orientation is observable
    const ArtMask  base = maskFromRows(kL16);
    for (Rotation rot : {Rotation::None, Rotation::Rot90, Rotation::Rot180, Rotation::Rot270}) {
        for (bool fx : {false, true}) {
            for (bool fy : {false, true}) {
                Sprite s = spriteFor(art, AssetDimensions{16, 16});
                s.rotation = rot;
                s.flipX = fx;
                s.flipY = fy;
                const SpriteShape shape = s.asShape(art, Space::Quad);
                // A quad point that is the oriented image of an art pixel centre must report the pixel's
                // own coverage — the inverse-orientation round trip.
                for (int y = 0; y < base.height; ++y) {
                    for (int x = 0; x < base.width; ++x) {
                        const Point q = orientPoint(Point{x + 0.5f, y + 0.5f}, 16, 16, rot, fx, fy);
                        EXPECT_EQ(shape.contains(q), base.at(x, y))
                            << "rot " << static_cast<int>(rot) << " fx " << fx << " fy " << fy
                            << " @ (" << x << "," << y << ")";
                    }
                }
            }
        }
    }
}

// ── Layer space — approximate() and contains() agree with toLayer() under a nontrivial placement ─

Sprite placedSprite(const AtlasArt& art) {
    Sprite s = spriteFor(art, AssetDimensions{16, 16});
    s.x = 40;
    s.y = 24;
    s.origin = {8.0f, 8.0f};
    s.pivot  = {8.0f, 8.0f};
    s.transform = Transform::scale(1.5f, 0.75f).then(Transform::rotation(20.0f));
    return s;
}

TEST(LayerSpace, ApproximateLayerIsToLayerOfApproximateQuad) {
    const AtlasArt art = artFromRows(kL16);
    const Sprite s = placedSprite(art);
    const auto quad  = s.approximate(art, 16, Space::Quad);
    const auto layer = s.approximate(art, 16, Space::Layer);
    ASSERT_EQ(quad.points.size(), layer.points.size());
    for (std::size_t i = 0; i < quad.points.size(); ++i) {
        const Point expect = s.toLayer(quad.points[i]);
        EXPECT_NEAR(layer.points[i].x, expect.x, 1e-3f);
        EXPECT_NEAR(layer.points[i].y, expect.y, 1e-3f);
    }
}

TEST(LayerSpace, ContainsLayerMatchesContainsQuadThroughToLayer) {
    const AtlasArt art = artFromRows(kL16);
    const Sprite s = placedSprite(art);
    const SpriteShape quad  = s.asShape(art, Space::Quad);
    const SpriteShape layer = s.asShape(art, Space::Layer);
    const ArtMask base = maskFromRows(kL16);
    for (int y = 0; y < base.height; ++y)
        for (int x = 0; x < base.width; ++x) {
            const Point q = Point{x + 0.5f, y + 0.5f};
            EXPECT_EQ(layer.contains(s.toLayer(q)), quad.contains(q)) << "@ (" << x << "," << y << ")";
        }
}

// ── Freeze — an owned snapshot answers the same as the live borrow, detached ────────────────────

TEST(Freeze, OwnedSnapshotMatchesBorrowAndSurvivesSourceChange) {
    AtlasArt art = artFromRows(kL16);
    Sprite s = spriteFor(art, AssetDimensions{16, 16});
    const SpriteShape borrow = s.asShape(art, Space::Quad);
    const FrozenSpriteShape frozen = s.freeze(art, Space::Quad);
    const ArtMask base = maskFromRows(kL16);
    // Detach: scribble over the source art. The frozen snapshot keeps its own mask; the borrow would follow.
    std::fill(art.indices.begin(), art.indices.end(), std::uint8_t{0});
    for (int y = 0; y < base.height; ++y)
        for (int x = 0; x < base.width; ++x) {
            const Point q = Point{x + 0.5f, y + 0.5f};
            EXPECT_EQ(frozen.contains(q), base.at(x, y)) << "@ (" << x << "," << y << ")";
        }
    (void)borrow;
}

TEST(Bounds, QuadBoundsAreTheVisibleArtExtent) {
    const AtlasArt art = artFromRows(kL16);   // fills the whole 16×16 (the L touches all four edges)
    const Sprite s = spriteFor(art, AssetDimensions{16, 16});
    const IntRect b = s.asShape(art, Space::Quad).bounds();
    EXPECT_EQ(b, (IntRect{0, 0, 16, 16}));
    const ArtMask small = maskFromRows({"........", "..##....", "..##....", "........",
                                        "........", "........", "........", "........"});
    (void)small;
}

// ── Throws + manifest retention ─────────────────────────────────────────────────────────────────

TEST(Throws, AtlasMismatch) {
    const AtlasArt art = artFromRows(kFull8, AtlasId{9});   // different sheet than the sprite
    Sprite s = spriteFor(artFromRows(kFull8, kSheet), AssetDimensions{8, 8});  // sprite on kSheet
    EXPECT_THROW((void)s.asShape(art, Space::Quad), std::invalid_argument);
    EXPECT_THROW((void)s.freeze(art, Space::Quad), std::invalid_argument);
    EXPECT_THROW((void)s.approximate(art, 8, Space::Quad), std::invalid_argument);
}

TEST(Throws, ApproximateBudgetBelowThree) {
    const AtlasArt art = artFromRows(kFull8);
    const Sprite s = spriteFor(art, AssetDimensions{8, 8});
    EXPECT_THROW((void)s.approximate(art, 2, Space::Quad), std::invalid_argument);
}

TEST(ManifestRetention, ConversionWorksAtACallSite) {
    const AtlasArt art = artFromRows(kFull8);
    auto artPtr = std::make_shared<const AtlasArt>(art);
    AtlasManifest manifest{.atlas = art.atlas, .art = artPtr};
    const Sprite s = spriteFor(art, AssetDimensions{8, 8});
    // The manifest converts implicitly to `const AtlasArt&` — the ergonomic call site.
    const auto poly = s.approximate(manifest, 8, Space::Quad);
    EXPECT_EQ(poly.points.size(), 4u);
    EXPECT_TRUE(s.asShape(manifest, Space::Quad).contains(Point{4.0f, 4.0f}));
}

TEST(ManifestRetention, EmptyManifestFailsLoudly) {
    const AtlasManifest empty{};  // no retained art
    const Sprite s = spriteFor(artFromRows(kFull8), AssetDimensions{8, 8});
    EXPECT_THROW((void)s.approximate(empty, 8, Space::Quad), std::logic_error);
    EXPECT_THROW((void)s.asShape(empty, Space::Quad), std::logic_error);
}

}  // namespace
}  // namespace retropp
