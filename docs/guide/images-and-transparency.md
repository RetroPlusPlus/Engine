# Images & transparency

Loading art from PNG files, and the opt-in per-source transparency that lets one layer's holes reveal
the layer beneath. This feeds the indexed colour model in [tiles-and-colour.md](tiles-and-colour.md):
a PNG's index plane goes straight into `uploadAtlas`.

```cpp
#include "gbcpp/image.h"   // ImageColorKind, LoadedImage, loadPng, loadPngFromMemory
```

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

### How sources route

| PNG colour type | `kind` | `indices` | `palette` |
|---|---|---|---|
| **Palette (PLTE)** | `Indexed` | the raw PLTE index per pixel | the embedded palette, as `Rgba8` |
| **Grayscale** | `Indexed` | the grey sample value *as* the index (e.g. 2-bit → 0..3) | empty (grayscale carries no colour) |
| **Truecolour / truecolour-alpha** | — | — | rejected (throws — see below) |

The index is **read, never reverse-derived from a colour**: the decoder preserves the source's own
format and the engine unpacks the (possibly sub-byte) samples itself, so it is exact for any bit depth
(1/2/4/8-bit). A palette PNG yields its PLTE index plane plus the embedded palette; a grayscale PNG
yields its sample-as-index plane and an empty palette (you supply colour separately via
`uploadPalette` — the indexed model never bakes colour into the art). Errors — a missing file, a
corrupt PNG — throw `std::runtime_error`.

> **Faithful default.** Indexed/grayscale is the faithful console source format. Decoding works
> headlessly (pure CPU), so image loading is unit-testable with no window or GPU device.

### Truecolour is not yet supported

A truecolour (RGB / RGBA) PNG is **detected and rejected** today — `loadPng` throws an error naming
the future direct-RGBA path. The `Rgba` kind and the `pixels` field are the declared seam for it; the
consumer (a direct-RGBA atlas format) is **deferred** — gated on an engine consumer needing
non-indexed art, not currently scheduled. Indexed/grayscale is the faithful console source format and
the only one the engine renders; author art as indexed or grayscale PNGs.

## Per-source index-hole transparency

By default every atlas index is opaque. A tile layer can declare one index as **transparent** — a
hole the layer beneath shows through — per source, at upload time:

```cpp
// Index 0 in this atlas becomes a hole on any TILES layer that draws from it.
const AtlasId holed = renderer.uploadAtlas(indices, w, h, /*transparentIndex=*/0);

// Default (-1) = no transparent index → fully opaque, byte-identical to the faithful baseline.
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
