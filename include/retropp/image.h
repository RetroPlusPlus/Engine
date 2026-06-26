#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "retropp/asset_policy.h"  // AssetPolicy (loadMapPng's optional embed/load override)
#include "retropp/geometry.h"  // PixelSize, AssetDimensions
#include "retropp/literal_path.h"  // LiteralPath (a build-managed asset path must be a string literal)
#include "retropp/palette.h"   // Rgba8

namespace retropp {

// How a decoded image carries colour. The PNG's colour type routes the load: a palette or
// grayscale PNG is INDEXED (one palette index per pixel, faithful console model); a truecolour
// PNG is RGBA (one colour per pixel). Identity is this field — first member.
enum class ImageColorKind : std::uint8_t { Indexed, Rgba };

// A decoded image. For an INDEXED source, `indices` holds one palette index per pixel (row-major)
// and `palette` holds the embedded palette (PLTE) when the PNG carries one — empty for grayscale
// (grayscale carries no embedded colour; the consumer supplies a palette separately). For an RGBA
// source, `pixels` holds one colour per pixel (row-major), in 16-bit-per-channel Rgba16 — an 8-bit
// truecolour PNG widens losslessly (×257), a 16-bit one decodes direct. Only the field matching
// `kind` is populated. Dimensions are in pixels.
struct LoadedImage {
    ImageColorKind            kind   = ImageColorKind::Indexed;  // identity, first member
    int                       width  = 0;
    int                       height = 0;
    std::vector<std::uint8_t> indices;   // kind == Indexed: one index per pixel, row-major
    std::vector<Rgba8>        palette;   // kind == Indexed: embedded palette (may be empty); PLTE is 8-bit
    std::vector<Rgba16>       pixels;    // kind == Rgba: one 16-bit colour per pixel, row-major
};

// Decode a PNG file into a LoadedImage. Indexed/grayscale PNGs populate `indices` (+ `palette`
// for PLTE PNGs); a truecolour PNG populates `pixels` (kind == Rgba) — 8-bit channels widen ×257,
// 16-bit channels decode direct. Throws std::runtime_error on a missing file or a decode failure.
[[nodiscard]] LoadedImage loadPng(const std::filesystem::path& path);

// Same, from an in-memory PNG byte span (used by the headless tests and embeddable assets).
[[nodiscard]] LoadedImage loadPngFromMemory(std::span<const std::uint8_t> bytes);

// ── Tilemap image import: a map PNG as a grid of raw index values ────────────────────────────────
//
// A MAP image is not art — it is a grid of NUMBERS. Each pixel's grayscale (or palette) sample value
// IS a raw index: for a tilemap, an index into a TileCatalog (which entry — i.e. which sheet/cell/
// palette/flip to draw); for a collision map, a raw collision id the GAME interprets. The engine
// never interprets the values — it only decodes them faithfully (the sample value is the index,
// never scaled or reverse-derived from a colour, exactly as the atlas path treats indices).
//
// 16-bit grayscale is the headline case (a tilemap may reference >256 catalog entries — 65 536 ids),
// but 8-/4-/2-/1-bit grayscale and palette PNGs decode too (smaller index spaces); every value widens
// into the uint16 grid. Truecolour is rejected (a map carries indices, not colours). This is the
// SAME decode the atlas path uses (no colour conversion), only widened to 16 bits and carrying no
// palette — the consumer supplies meaning, here a TileCatalog.
struct IndexGrid {
    int                        width  = 0;
    int                        height = 0;
    std::vector<std::uint16_t> values;   // one raw index per pixel, row-major (width * height)

    [[nodiscard]] std::uint16_t at(int x, int y) const {
        return values[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }
};

// Decode a map PNG (grayscale 1/2/4/8/16-bit, or palette) into an IndexGrid of raw index values.
// Throws std::runtime_error on a missing file, a decode failure, or a truecolour source (a map is
// indices, not colour). Collision maps use the SAME function — the game reads IndexGrid::values raw.
//
// The path is a LITERAL, project-root-relative logical path (a string literal — the build-time scan
// reads it to bake or copy the asset, so a runtime/computed path is a compile error; load a runtime
// file with loadMapPngFromMemory(readFile(...)) instead). `policy` selects whether the asset
// is read from disk (LoadFromPath) or decoded from bytes baked into the binary at build time (Embed).
// nullopt (the default) resolves by precedence: EngineConfig::defaultAssetPolicy, then loadMapPng's
// per-type default (Embed — a map PNG is bespoke build-time index data). A LoadFromPath asset resolves
// against the runtime asset root (assetRoot()); an Embed asset the build did not bake falls through to
// that disk read.
[[nodiscard]] IndexGrid loadMapPng(LiteralPath path, std::optional<AssetPolicy> policy = {});

// Same, from an in-memory PNG byte span (headless tests / embeddable map assets).
[[nodiscard]] IndexGrid loadMapPngFromMemory(std::span<const std::uint8_t> bytes);

// ── Atlas asset ingestion: slicing a loaded image into addressable sub-assets ───────────────────
//
// The renderer dices an atlas at DRAW time (a TileCell / Sprite carries a `tile` cell index on the
// 8px atlas grid). What this layer adds is an INGESTION-side description: given a loaded image and
// the size of one asset, carve the image into addressable sub-asset slots in a chosen traversal
// order, so the developer never hand-computes per-frame tile indices. Pure geometry — no GPU, no
// AtlasId — so it is fully headless-unit-testable; Renderer::loadAtlas (renderer.h) is the thin
// convenience that chains loadPng → uploadAtlas → sliceLayout into an AtlasManifest.

// A handle to uploaded atlas pixel data the renderer owns. Identity is the typed handle; the
// renderer maps it (via the global atlas-region table) to its placement in the flat atlas store.
// Sibling to PaletteId (palette.h). It lives here in image.h — beside the atlas-ingestion surface
// (AssetSlot / AtlasManifest), the semantically correct home for a handle to uploaded atlas *image*
// data — so a consumer can name a frame's atlas (animation.h) without pulling in the whole draw-state
// submission envelope. draw_state.h includes image.h to use it (TileCell / Sprite carry an AtlasId);
// the fully-qualified name is unchanged. 16-bit: a tilemap cell carries it directly every frame, so it
// is sized to the real ceiling (thousands of sheets) with headroom, not to 32-bit billions.
enum class AtlasId : std::uint16_t {};

// The atlas addressing cell: 8px. This is the atomic tile/OBJ cell of the whole 8/16-bit era — GB/GBC,
// NES, SMS, SNES, and Genesis all dice their art into 8×8 cells, and nothing in the paradigm is finer —
// and it is the unit the entire engine addresses atlas content by (Sprite/TileCell::tile are cell
// indices; sampleTilemap's tilePx). It is the granularity of an asset's atlas *origin*, NOT a floor on
// its *size*: AssetDimensions is flexible — any whole number of cells (8×8, 8×16, 16×16, 24×16, 64×64,
// non-square), so an asset spans N×M cells and its `tile` is its top-left cell index. (Pixel-precise
// *placement* is a screen-space concern — Sprite::x/y, arbitrary pixels, exactly as on real hardware;
// only the atlas art itself is cell-gridded.)
inline constexpr int kAtlasCellPx = 8;

// What a loaded image holds — how sliceLayout carves it. The carve itself only ever branches Single
// vs grid; every grid kind (Tileset / SpriteSeries / SingleAnimation / AnimationSeries) produces the
// same flat slot list. The distinct names let the call site read its own intent, and the two
// animation kinds drive manifest-level grouping (AtlasManifest::framesPerAnimation), NOT a different
// carve — grouping is a manifest concern, not a carve concern.
enum class ContentKind : std::uint8_t {
    Single,           // the whole image is ONE asset → exactly 1 slot
    Tileset,          // a grid of independent tiles   → N slots
    SpriteSeries,     // a grid of independent sprites  → N slots
    SingleAnimation,  // the grid = the frames of ONE animation (carves identically to a grid)
    AnimationSeries,  // the grid = MULTIPLE animations × framesPerAnimation frames each (grouped on
                      // the manifest; the flat carve is still one slot per frame in read order)
};

// The order sliceLayout walks a grid of cells — all 2×2×2 = 8 permutations are nameable, because
// some 1990s carts laid their frames in non-western orders. `fill` chooses whether a row is filled
// before stepping to the next (Rows) or a column is filled before stepping across (Columns);
// `horizontal`/`vertical` choose each axis's direction. A raw ReadOrder{ .fill, .horizontal,
// .vertical } builds any combination by hand; the eight presets name them all.
struct ReadOrder {
    enum class Fill          : std::uint8_t { Rows, Columns };
    enum class HorizontalDir : std::uint8_t { LeftToRight, RightToLeft };
    enum class VerticalDir   : std::uint8_t { TopToBottom, BottomToTop };

    Fill          fill       = Fill::Rows;
    HorizontalDir horizontal = HorizontalDir::LeftToRight;
    VerticalDir   vertical   = VerticalDir::TopToBottom;
    [[nodiscard]] constexpr bool operator==(const ReadOrder&) const noexcept = default;

    static const ReadOrder LeftRightThenDown;   // Rows,    L→R, T→B  ← western default
    static const ReadOrder RightLeftThenDown;   // Rows,    R→L, T→B
    static const ReadOrder LeftRightThenUp;     // Rows,    L→R, B→T
    static const ReadOrder RightLeftThenUp;     // Rows,    R→L, B→T
    static const ReadOrder TopBottomThenRight;  // Columns, L→R, T→B
    static const ReadOrder BottomTopThenRight;  // Columns, L→R, B→T
    static const ReadOrder TopBottomThenLeft;   // Columns, R→L, T→B
    static const ReadOrder BottomTopThenLeft;   // Columns, R→L, B→T
};

inline constexpr ReadOrder ReadOrder::LeftRightThenDown{
    ReadOrder::Fill::Rows, ReadOrder::HorizontalDir::LeftToRight, ReadOrder::VerticalDir::TopToBottom};
inline constexpr ReadOrder ReadOrder::RightLeftThenDown{
    ReadOrder::Fill::Rows, ReadOrder::HorizontalDir::RightToLeft, ReadOrder::VerticalDir::TopToBottom};
inline constexpr ReadOrder ReadOrder::LeftRightThenUp{
    ReadOrder::Fill::Rows, ReadOrder::HorizontalDir::LeftToRight, ReadOrder::VerticalDir::BottomToTop};
inline constexpr ReadOrder ReadOrder::RightLeftThenUp{
    ReadOrder::Fill::Rows, ReadOrder::HorizontalDir::RightToLeft, ReadOrder::VerticalDir::BottomToTop};
inline constexpr ReadOrder ReadOrder::TopBottomThenRight{
    ReadOrder::Fill::Columns, ReadOrder::HorizontalDir::LeftToRight, ReadOrder::VerticalDir::TopToBottom};
inline constexpr ReadOrder ReadOrder::BottomTopThenRight{
    ReadOrder::Fill::Columns, ReadOrder::HorizontalDir::LeftToRight, ReadOrder::VerticalDir::BottomToTop};
inline constexpr ReadOrder ReadOrder::TopBottomThenLeft{
    ReadOrder::Fill::Columns, ReadOrder::HorizontalDir::RightToLeft, ReadOrder::VerticalDir::TopToBottom};
inline constexpr ReadOrder ReadOrder::BottomTopThenLeft{
    ReadOrder::Fill::Columns, ReadOrder::HorizontalDir::RightToLeft, ReadOrder::VerticalDir::BottomToTop};

// One carved sub-asset — PURE GEOMETRY, no draw-state type leaks in (the manifest stays generalized;
// there are no Sprite/TileCell builder helpers). `tile` is the top-left atlas cell on the 8px grid —
// exactly the value Sprite::tile / TileCell::tile take; `dimensions` is the slot's pixel size.
struct AssetSlot {
    std::uint16_t   tile = 0;
    AssetDimensions dimensions{};
    [[nodiscard]] constexpr bool operator==(const AssetSlot&) const noexcept = default;
};

// One cell of a grid, by column + row. The currency of the read-order walk: readOrderCells yields these
// in traversal order, and each slicer maps a cell to its own output (sliceLayout → an atlas-cell index;
// slicePaletteImage → that pixel's colour).
struct GridCell {
    int col = 0;
    int row = 0;
    [[nodiscard]] constexpr bool operator==(const GridCell&) const noexcept = default;
};

// The cells of a `cols` × `rows` grid in `order` — the single source of read-order truth that both
// sliceLayout and slicePaletteImage walk, so their traversal can never drift apart. `order.fill` picks
// the inner axis (Rows → a row fills before stepping down; Columns → a column fills before stepping
// across); `order.horizontal` / `order.vertical` choose each axis's direction. `count` caps how many
// cells are emitted: 0 (the default) = the whole grid; a positive `count` emits the first `count` in
// traversal order; a `count` past the grid capacity is clamped to capacity (and logged). Returns {} for
// a non-positive `cols` or `rows`. Never throws.
[[nodiscard]] std::vector<GridCell> readOrderCells(int cols, int rows, ReadOrder order, int count = 0);

// Carve an image of `imageSize` pixels into asset slots of `assetSize`, per `kind` + `order`:
//   Single                → exactly 1 slot: { tile 0, dimensions = the whole image }.
//   Tileset / SpriteSeries → a grid of (imageW/assetW) × (imageH/assetH) slots, emitted in the
//                            traversal `order`. Each slot's `tile` is its top-left 8px cell index:
//                            (pyTop/8) * (imageW/8) + (pxTop/8). The two kinds slice IDENTICALLY
//                            (both "grid of N"); they are distinct names so the call site reads its
//                            own intent. Internally only Single vs grid branches.
// `count` caps how many assets are carved: 0 (the default) carves the WHOLE grid; a positive `count`
// emits only the first `count` slots in the traversal `order` and drops the rest — for a sheet whose
// grid has room for more cells than the art actually uses (e.g. 8 cells of space, 5 real frames), so
// the manifest holds exactly the real assets, not trailing empties. A `count` larger than the grid
// capacity is clamped to capacity (and logged); ignored for `Single` (always one slot).
//
// Trailing pixels that do not complete a cell are dropped (full cells only) and logged — never
// silently claimed as covered. Returns {} on a degenerate input: non-positive image or asset size,
// assetSize not a whole number of atlas cells (not a positive multiple of kAtlasCellPx), or assetSize
// larger than the image on either axis. The slicer never throws (the throw surface stays loadPng /
// the GPU upload).
[[nodiscard]] std::vector<AssetSlot> sliceLayout(PixelSize imageSize, AssetDimensions assetSize,
                                                 ContentKind kind, ReadOrder order, int count = 0);

// ── Palette images: a colour image sliced one-pixel-per-entry into palette colours ───────────────
//
// A PALETTE image is the colour counterpart to a map image: where a map PNG is a grid of index
// NUMBERS, a palette PNG is a grid of COLOURS, read one pixel per palette entry. slicePaletteImage
// walks the decoded pixels in `order` (the SAME read-order traversal sliceLayout uses, via
// readOrderCells) and returns the entry colours in that order — so entry k is the k-th pixel in the
// walk. The colour analog of sliceLayout: pure geometry+colour, no GPU, fully headless-testable;
// Renderer::loadPaletteImage (renderer.h) is the thin convenience that chains loadPng → this →
// uploadPalette into a PaletteId.
//
// `img` must be an RGBA (truecolour) source (kind == ImageColorKind::Rgba) — an indexed image carries
// indices, not colours, so it is a misuse: throws std::runtime_error. Entries are Rgba16 (the decoded
// pixel type — an 8-bit source already widened ×257 at decode). `count` caps how many entries are
// taken (0 = every pixel; a positive count takes the first `count` in read order; past capacity clamps
// + logs, per readOrderCells). A non-positive / empty image yields {}.
[[nodiscard]] std::vector<Rgba16> slicePaletteImage(const LoadedImage& img,
                                                    ReadOrder order = ReadOrder::LeftRightThenDown,
                                                    int count = 0);

}  // namespace retropp
