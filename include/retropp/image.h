#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "retropp/geometry.h"  // PixelSize, AssetDimensions
#include "retropp/palette.h"   // Rgba8

namespace retropp {

// How a decoded image carries colour. The PNG's colour type routes the load: a palette or
// grayscale PNG is INDEXED (one palette index per pixel, faithful console model); a truecolour
// PNG is RGBA (one colour per pixel). Identity is this field — first member.
enum class ImageColorKind : std::uint8_t { Indexed, Rgba };

// A decoded image. For an INDEXED source, `indices` holds one palette index per pixel (row-major)
// and `palette` holds the embedded palette (PLTE) when the PNG carries one — empty for grayscale
// (grayscale carries no embedded colour; the consumer supplies a palette separately). For an RGBA
// source, `pixels` holds one colour per pixel (row-major). Only the field matching `kind` is
// populated. Dimensions are in pixels.
struct LoadedImage {
    ImageColorKind            kind   = ImageColorKind::Indexed;  // identity, first member
    int                       width  = 0;
    int                       height = 0;
    std::vector<std::uint8_t> indices;   // kind == Indexed: one index per pixel, row-major
    std::vector<Rgba8>        palette;   // kind == Indexed: embedded palette (may be empty)
    std::vector<Rgba8>        pixels;    // kind == Rgba: one colour per pixel (ENG-2.B.3.b)
};

// Decode a PNG file into a LoadedImage. Indexed/grayscale PNGs populate `indices` (+ `palette`
// for PLTE PNGs). A truecolour PNG throws std::runtime_error("...ENG-2.B.3.b...") for now (the
// RGBA consumer is B.3.b). Throws std::runtime_error on a missing file or a decode failure.
[[nodiscard]] LoadedImage loadPng(const std::filesystem::path& path);

// Same, from an in-memory PNG byte span (used by the headless tests and embeddable assets).
[[nodiscard]] LoadedImage loadPngFromMemory(std::span<const std::uint8_t> bytes);

// ── Atlas asset ingestion: slicing a loaded image into addressable sub-assets (ENG-2.G) ─────────
//
// The renderer dices an atlas at DRAW time (a TileCell / Sprite carries a `tile` cell index on the
// 8px atlas grid). What this layer adds is an INGESTION-side description: given a loaded image and
// the size of one asset, carve the image into addressable sub-asset slots in a chosen traversal
// order, so the developer never hand-computes per-frame tile indices. Pure geometry — no GPU, no
// AtlasId — so it is fully headless-unit-testable; Renderer::loadAtlas (renderer.h) is the thin
// convenience that chains loadPng → uploadAtlas → sliceLayout into an AtlasManifest.

// A handle to uploaded atlas pixel data the renderer owns. Identity is the typed handle; the
// renderer maps it to its GPU texture. Sibling to PaletteId (palette.h). It lives here in image.h —
// beside the atlas-ingestion surface (AssetSlot / AtlasManifest), the semantically correct home for
// a handle to uploaded atlas *image* data — so a consumer can name a frame's atlas (animation.h)
// without pulling in the whole draw-state submission envelope. draw_state.h includes image.h to use
// it (TileContent / SpriteContent carry an AtlasId); the fully-qualified name is unchanged.
enum class AtlasId : std::uint32_t {};

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

}  // namespace retropp
