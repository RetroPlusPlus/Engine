# Tiles & colour

The faithful 8-/16-bit colour model: art is **palette indices**, and colour comes from a **palette
chosen at render time** — never baked into the art. This page covers indexed atlases, palette upload
and storage, per-layer palette sets, and the per-tile / per-sprite palette-select. The submission
types (`TileContent`, `Sprite`) are in [draw-state.md](draw-state.md); loading art from PNG is
[images-and-transparency.md](images-and-transparency.md).

```cpp
#include "retropp/palette.h"      // Rgba8, PaletteId, PaletteSize
#include "retropp/draw_state.h"   // TileCell, paletteSetOffsets, kPaletteSetSlots
```

## Contents

- [The colour model](#the-colour-model)
- [`Rgba8` — a final output colour](#rgba8--a-final-output-colour)
- [Uploading palettes: `uploadPalette` → `PaletteId`](#uploading-palettes-uploadpalette--paletteid)
- [Uploading art: `uploadAtlas` → `AtlasId`](#uploading-art-uploadatlas--atlasid)
- [Per-layer palette sets + per-tile select](#per-layer-palette-sets--per-tile-select)
- [Per-layer atlas sets + per-tile select](#per-layer-atlas-sets--per-tile-select)
- [Flip](#flip)
- [Wrap (finite vs infinite maps)](#wrap-finite-vs-infinite-maps)
- [Where to change things](#where-to-change-things)

## The colour model

An **indexed atlas** holds one palette index per pixel (`0..N-1`), uploaded once. A **palette** maps
each index to a final output colour. A tile or sprite picks *which* palette (within its layer's set)
it draws from. The shader applies the colour per-pixel: `index → selected palette → output colour`.

This is the faithful Game Boy / Game Boy Color model — colour is an index plus a palette selected at
render time — **not** a baked-RGBA atlas and **not** a hardware palette-RAM poke. Because the lookup
happens per-pixel in the shader, palette swaps, day/night, and animation stay data/shader concerns,
never a register write. The same indexed art renders in any colour scheme by changing the palette it
selects.

## `Rgba8` — a final output colour

```cpp
struct Rgba8 {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;   // named channels; opaque black by default
};
static_assert(sizeof(Rgba8) == 4);
```

The engine's output colour is `Rgba8` regardless of the source console — four named channels, opaque
by default, 4 bytes so it uploads as a tightly-packed RGBA8 texture row. A palette's colours come from
the game's assets at upload time (decoded from the console's own palette format consumer-side, or
hand-built).

## Uploading palettes: `uploadPalette` → `PaletteId`

```cpp
PaletteId Renderer::uploadPalette(std::span<const Rgba8> colors);
```

Upload one palette's colours once (amortized — at load time / on change). The renderer writes them
into a row of its internal palette store and returns a `PaletteId` handle. Arbitrary entry count (the
span length). `PaletteSize` is a set of **count mnemonics** — the enumerator value *is* the entry
count, so you can pass a preset or a raw integer interchangeably:

```cpp
enum class PaletteSize : std::uint32_t {
    GameBoy = 4, GameBoyColor = 4, Nes = 4,
    MasterSystem = 16, Genesis = 16, Snes = 16,
};
```

These set only *how many* entries, not a per-console colour model (the engine stores `Rgba8` output
regardless). A 4-colour Game Boy palette and a 16-colour SNES palette are the same call with a
different span length.

## Uploading art: `uploadAtlas` → `AtlasId`

```cpp
AtlasId Renderer::uploadAtlas(const std::uint8_t*  indices, int width, int height, int transparentIndex = -1);
AtlasId Renderer::uploadAtlas(const std::uint16_t* indices, int width, int height, int transparentIndex = -1);
AtlasId Renderer::uploadAtlas(const std::uint32_t* indices, int width, int height, int transparentIndex = -1);
```

Upload an **indexed** atlas once — one palette index per pixel, row-major, tightly packed (not RGBA;
colour comes from a palette at render time). Three overloads take 8-, 16-, or 32-bit source indices;
all widen into the renderer's 32-bit index store, so a tile pixel can address an arbitrarily large
palette no matter the source width. The atlas is addressed as an 8×8-tile grid: tile `t` lives at grid
`(t % cols, t / cols)`. The optional `transparentIndex` is the source's index-hole transparency
(default −1 = fully opaque) — see [images-and-transparency.md](images-and-transparency.md). To load an
atlas from a PNG instead of building the index array by hand, use `loadAtlas` (same page) — handing a
decoded image straight to `uploadAtlas` throws, so PNGs always go through `loadAtlas`.

## Per-layer palette sets + per-tile select

A tile layer carries a **palette set** (`TileContent::palettes` — a span of `PaletteId`), and each
cell selects *which* palette within that set it draws from:

```cpp
struct TileCell {
    std::uint16_t tile;        // which atlas tile
    std::uint8_t  palette;     // which palette WITHIN the layer's set (0..kPaletteSetSlots-1)
    bool          flipX, flipY;
    // ...
};
```

This is the mechanism that lets a small palette render a full-colour map: a single indexed tileset,
drawn through several palettes selected per cell, produces a multi-coloured scene. Sprites work the
same way (`Sprite::palette` selects within `SpriteContent::palettes`).

```cpp
inline constexpr std::size_t kPaletteSetSlots = 16;   // a cell selects slot 0..15
```

The per-layer set has up to 16 slots (covers the Game Boy's 8 background palettes with headroom). The
pure helper `paletteSetOffsets(set)` resolves a set to the shader's slot→flat-offset map; it is the
unit-tested mirror of the renderer's per-layer uniform fill (a `PaletteId`'s underlying value *is* its
flat offset into the palette store). A set larger than 16 is truncated to the first 16; an empty set is
a valid (degenerate) submission.

Palettes are **arbitrary size** — there is no 256-colour cap — and an atlas pixel is a full 32-bit
index, so one palette can hold as many colours as you upload and a tile (or sprite) pixel can address
any of them.

## Per-layer atlas sets + per-tile select

A tile layer can also draw from **several atlas sheets at once**, the exact parallel of the palette
set above. `TileContent::atlases` is the layer's atlas set and `TileCell::atlasSelect` picks which
sheet that cell draws from — so one layer mixes, say, a font sheet and a menu-border sheet, choosing
the sheet per cell. It mirrors the palette mechanism precisely: a cell selects its **sheet**
(`atlasSelect`) and its **palette** (`palette`) independently.

The single-atlas path is unchanged and faithful by default: leave `atlases` empty and the layer uses
the one `atlas` field, `atlasSelect` ignored. The set holds up to 16 sheets per layer; internally all
atlases live in one flat store the shader indexes by `atlasSelect` (the same flat-store pattern the
palette store uses), so you never manage per-sheet textures.

You rarely set `atlases`/`atlasSelect` by hand — building a tile layer from a map image and a catalog
fills them for you. See **[tilemaps.md](tilemaps.md)**.

## Flip

`flipX` / `flipY` on a `TileCell` (and on a `Sprite`) mirror the tile's pixels horizontally /
vertically at sample time — the within-tile offset is flipped before the atlas is addressed, so one
atlas tile serves all four orientations. No extra art needed.

## Wrap (finite vs infinite maps)

A tile layer's `TileContent::wrap` chooses what happens when scrolling samples beyond the map's
`widthInTiles × heightInTiles` bounds: `Repeat` (default) tiles the map infinitely, `Clamp` smears
the edge tile, and `Blank` makes the map **finite** — anything past the edge is transparent (the
layers below show through), so a finite overworld map ends cleanly instead of repeating. The full
description is in [draw-state.md](draw-state.md#tilecontent--a-scrolling-tile-map).

## Where to change things

- **Recolour a scene without new art:** change the `PaletteId`s in a layer's palette set, or the
  per-cell `palette` select.
- **Animate colour (water shimmer, palette cycling):** re-upload a palette (`uploadPalette` returns a
  new `PaletteId`) or swap which palette a layer/cell selects per frame — both are data changes, no
  shader edit.
- **Whole-frame fades / day-night:** that's the frame-level `ColorModifier` / `Blend`
  ([draw-state.md](draw-state.md)), a post-composite transform — distinct from the per-pixel palette
  colouring here.
- **Swap an entire tileset:** upload a new atlas and reference its `AtlasId` from the layer.
