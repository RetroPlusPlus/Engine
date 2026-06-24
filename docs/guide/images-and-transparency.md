# Images & transparency

Loading art from PNG files, and the opt-in per-source transparency that lets one layer's holes reveal
the layer beneath. This feeds the indexed colour model in [tiles-and-colour.md](tiles-and-colour.md):
a PNG's index plane goes straight into `uploadAtlas`.

```cpp
#include "retropp/image.h"   // ImageColorKind, LoadedImage, loadPng/loadPngFromMemory;
                          // ContentKind, ReadOrder, AssetSlot, sliceLayout (slicing, below)
```

## Contents

- [Loading a PNG: `loadPng`](#loading-a-png-loadpng)
  - [How sources route](#how-sources-route)
  - [Truecolour is not yet supported](#truecolour-is-not-yet-supported)
- [Slicing an atlas into addressable assets <a id="slicing"></a>](#slicing-an-atlas-into-addressable-assets-a-idslicinga)
- [Per-source index-hole transparency](#per-source-index-hole-transparency)
- [A worked example](#a-worked-example)
- [Where to change things](#where-to-change-things)

## Loading a PNG: `loadPng`

```cpp
LoadedImage loadPng(const std::filesystem::path& path);
LoadedImage loadPngFromMemory(std::span<const std::uint8_t> bytes);

enum class ImageColorKind : std::uint8_t { Indexed, Rgba };

struct LoadedImage {
    ImageColorKind            kind   = ImageColorKind::Indexed;
    int                       width  = 0, height = 0;
    std::vector<std::uint8_t> indices;   // kind == Indexed: one index per pixel, row-major
    std::vector<Rgba8>        palette;   // kind == Indexed: embedded palette (may be empty)
    std::vector<Rgba8>        pixels;    // kind == Rgba: one colour per pixel (not yet — see below)
};
```

`loadPng` decodes a PNG and routes by its colour type. For the faithful indexed path it extracts the
**index plane** — one palette index per pixel — which is exactly what `uploadAtlas` consumes:

```cpp
const LoadedImage img = loadPng("assets/tileset.png");
const AtlasId atlas = renderer.uploadAtlas(img.indices.data(), img.width, img.height);
```

> Pass the index pointer (`img.indices.data()`), not the whole `LoadedImage` — `uploadAtlas(const
> LoadedImage&)` deliberately throws `std::logic_error`. A PNG should be sliced via `loadAtlas`
> ([below](#slicing)), which carves it into addressable assets; the raw-pointer overload is for art
> you've already decoded and don't need sliced.

### How sources route

| PNG colour type | `kind` | `indices` | `palette` |
|---|---|---|---|
| **Palette (PLTE)** | `Indexed` | the raw PLTE index per pixel | the embedded palette, as `Rgba8` |
| **Grayscale** | `Indexed` | the grey sample value *as* the index (e.g. 2-bit → 0..3) | empty (grayscale carries no colour) |
| **Truecolour / truecolour-alpha** | — | — | rejected (throws — see below) |

The index is **read, never reverse-derived from a colour**: the decoder preserves the source's own
format and the engine unpacks the (possibly sub-byte) samples itself, so it is exact for any bit depth
(1/2/4/8-bit). A 16-bit-sample PNG throws on this indexed path — that depth carries wider indices and
belongs to the map-import path (a separate loader). A palette PNG yields its PLTE index plane plus the
embedded palette; a grayscale PNG
yields its sample-as-index plane and an empty palette (you supply colour separately via
`uploadPalette` — the indexed model never bakes colour into the art). Errors — a missing file, a
corrupt PNG — throw `std::runtime_error`.

> **Faithful default.** Indexed/grayscale is the faithful console source format. Decoding works
> headlessly (pure CPU), so image loading is unit-testable with no window or GPU device.

### Truecolour is not yet supported

A truecolour (RGB / RGBA) PNG is **detected and rejected** today — `loadPng` throws. The `Rgba` kind
and the `pixels` field are the declared seam for direct-RGBA support; the
consumer (a direct-RGBA atlas format) is **deferred** — gated on an engine consumer needing
non-indexed art, not currently scheduled. Indexed/grayscale is the faithful console source format and
the only one the engine renders; author art as indexed or grayscale PNGs.

## Slicing an atlas into addressable assets <a id="slicing"></a>

A PNG is often a *grid* of tiles or sprite frames, not one asset. `loadAtlas` (a `Renderer` method)
uploads the image once **and** carves it into addressable sub-asset slots — the ergonomic chain over
`loadPng` → `uploadAtlas` → the pure `sliceLayout`:

```cpp
#include "retropp/renderer.h"   // AtlasManifest, Renderer::loadAtlas; ingestion types come from image.h

struct AssetSlot {            // one carved sub-asset — pure geometry, no draw-state types
    std::uint16_t   tile = 0; // its top-left atlas cell (8px grid) — feed to Sprite::tile / TileCell::tile
    AssetDimensions dimensions{};
};
struct AtlasManifest {        // what loadAtlas returns
    AtlasId                atlas{};
    std::vector<AssetSlot> slots;          // the carved assets, in read order
    std::size_t            count() const;  // slots.size()
    const AssetSlot&       operator[](std::size_t i) const;
};

const AtlasManifest sheet =
    renderer.loadAtlas("assets/tiles.png", AssetDimensions::GameBoy8x8, ContentKind::Tileset);
// sheet[i].tile / sheet[i].dimensions address the i-th carved tile.
```

**Content kind** — what the image holds:

| `ContentKind` | carves into |
|---|---|
| `Single` | exactly one slot covering the whole image |
| `Tileset` | a grid of N independent tiles |
| `SpriteSeries` | a grid of N independent sprite frames |

`Tileset` and `SpriteSeries` slice **identically** (both "grid of N") — distinct names so the call
site reads its intent. The animation content kinds are a later feature that reuses this same slicer
(each slot is already a per-frame reference).

**Read order** — the traversal across the grid. All eight permutations are named presets, because some
carts laid their frames in non-western orders:

```cpp
struct ReadOrder {
    enum class Fill          { Rows, Columns };          // fill a row first, or a column first
    enum class HorizontalDir { LeftToRight, RightToLeft };
    enum class VerticalDir   { TopToBottom, BottomToTop };
    // ... + the 8 named presets:
};
// ReadOrder::LeftRightThenDown (western default), RightLeftThenDown, LeftRightThenUp, RightLeftThenUp,
// TopBottomThenRight (column-major), BottomTopThenRight, TopBottomThenLeft, BottomTopThenLeft.
```

A raw `ReadOrder{ .fill, .horizontal, .vertical }` builds any combination by hand.

**On asset size + the 8px cell.** The atlas is addressed in **8px cells** — the atomic tile/sprite cell
of the whole 8/16-bit era (GB/GBC, NES, SMS, SNES, Genesis all dice art into 8×8 cells; nothing in the
paradigm is finer). That's the granularity of where an asset *sits* in the atlas, **not** a limit on its
*size*: `AssetDimensions` is flexible — an asset is any whole number of cells (8×8, 8×16, 16×16, 24×16,
64×64, non-square …), the natural sizes those consoles actually use. (Pixel-precise *placement* on
screen is separate — `Sprite::x/y` are arbitrary pixels, exactly as on hardware.) So a size that
straddles the grid (10×10, 12×7) is what's rejected, not "anything but 8×8."

**Partly-used sheets — `count`.** When a sheet has room for more cells than the art actually uses (8
cells of space, 5 real frames), pass a `count` so the manifest holds exactly the real assets instead of
trailing empties: `loadAtlas(path, size, kind, order, /*count=*/5)` carves the first 5 cells in read
order and stops. `count = 0` (the default) carves the whole grid; a `count` past the grid's capacity is
clamped to capacity (and logged); `Single` ignores it.

A **trailing partial cell is dropped** (full cells only, logged); a degenerate request (non-positive
size, asset bigger than the image, asset not a whole number of cells) yields an **empty** manifest — the
slicer never throws (load/decode/GPU failures still throw from `loadPng`/`uploadAtlas`). To re-carve an
already-uploaded atlas in a different order/kind/count without re-uploading, call the pure
`sliceLayout(imageSize, assetSize, kind, order, count)` directly and pair its slots with the existing
`AtlasId`. `loadAtlasFromMemory` is the in-memory-bytes overload.

## Per-source index-hole transparency

By default every atlas index is opaque. A tile layer can declare one index as **transparent** — a
hole the layer beneath shows through — per source, at upload time:

```cpp
// Index 0 in this atlas becomes a hole on any TILES layer that draws from it.
const AtlasId holed = renderer.uploadAtlas(indices, w, h, /*transparentIndex=*/0);

// Default (-1) = no transparent index → fully opaque (nothing discarded).
const AtlasId solid = renderer.uploadAtlas(indices, w, h);
```

The transparency is a property of the **source** (the atlas), not of the layer or the palette — the
same art uploaded with `transparentIndex = 0` has holes, uploaded with the default `-1` is fully
opaque. Where a tile pixel's index matches the atlas's transparent index, the tile shader discards it
and whatever layer sits below (by `z`) shows through. With the default `-1`, nothing is discarded and
the output is **byte-identical** to a faithful opaque background.

This is the **TILES** path. The sprite path has its own transparency: colour **index 0** is always
OBJ-transparent on sprites (the conventional sprite-transparency convention), independent of this
per-source setting. Unifying the two under one policy — and native-alpha / colour-key transparency for
the future RGBA sources — is planned.

## A worked example

Two layers from one PNG — an opaque background and, above it, the same art with index 0 holed so the
background shows through:

```cpp
const LoadedImage art = loadPng("assets/tileset.png");
const AtlasId opaque = renderer.uploadAtlas(art.indices.data(), art.width, art.height);          // -1
const AtlasId holed  = renderer.uploadAtlas(art.indices.data(), art.width, art.height, /*idx0*/0);
// lower z = opaque layer using `opaque`; higher z = a layer using `holed` → its index-0 pixels
// reveal the lower layer through the holes.
```

## Where to change things

- **Use a different art file:** point `loadPng` at another indexed/grayscale PNG; feed `indices` to
  `uploadAtlas`.
- **Make a colour see-through:** pass that palette index as `transparentIndex` when you upload the
  atlas.
- **Decode a console palette file (`.gbcpal`, etc.):** that's consumer-side — build the `Rgba8`
  palette from your asset format and `uploadPalette` it; the engine's image loader handles the index
  plane, you handle the colour table.
- **RGBA / truecolour art:** not supported yet (the seam is declared); author indexed/grayscale for
  now.
