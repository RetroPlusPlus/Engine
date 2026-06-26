# Tiles & colour

The faithful 8-/16-bit colour model: art is **palette indices**, and colour comes from a **palette
chosen at render time** — never baked into the art. This page covers indexed atlases, palette upload
and storage, and how each tile and sprite names its own sheet and palette directly. The submission
types (`TileContent`, `Sprite`) are in [draw-state.md](draw-state.md); loading art from PNG is
[images-and-transparency.md](images-and-transparency.md).

```cpp
#include "retropp/palette.h"      // Rgba8, PaletteId, PaletteSize
#include "retropp/draw_state.h"   // TileCell, Sprite, AtlasId
```

## Contents

- [The colour model](#the-colour-model)
- [`Rgba8` — a final output colour](#rgba8--a-final-output-colour)
- [Uploading palettes: `uploadPalette` → `PaletteId`](#uploading-palettes-uploadpalette--paletteid)
- [Uploading art: `uploadAtlas` → `AtlasId`](#uploading-art-uploadatlas--atlasid)
- [Each tile / sprite names its own sheet + palette](#each-tile--sprite-names-its-own-sheet--palette)
- [The `tiles()` helper — a single-combo cell run](#the-tiles-helper--a-single-combo-cell-run)
- [Flip](#flip)
- [Wrap (finite vs infinite maps)](#wrap-finite-vs-infinite-maps)
- [Where to change things](#where-to-change-things)

## The colour model

An **indexed atlas** holds one palette index per pixel (`0..N-1`), uploaded once. A **palette** maps
each index to a final output colour. A tile or sprite names *which* sheet (`AtlasId`) and *which*
palette (`PaletteId`) it draws from, directly. The shader applies the colour per-pixel: `index →
named palette → output colour`.

This is the faithful Game Boy / Game Boy Color model — colour is an index plus a palette selected at
render time — **not** a baked-RGBA atlas and **not** a hardware palette-RAM poke. Because the lookup
happens per-pixel in the shader, palette swaps, day/night, and animation stay data/shader concerns,
never a register write. The same indexed art renders in any colour scheme by naming a different
palette.

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

## Each tile / sprite names its own sheet + palette

Every `TileCell` and every `Sprite` names its **own sheet** (`atlas`, an `AtlasId`) and its **own
palette** (`palette`, a `PaletteId`) — both directly, as handles:

```cpp
struct TileCell {
    std::uint16_t tile    = 0;   // cell index within its OWN sheet (`atlas`), on the 8px grid
    AtlasId       atlas{};       // which uploaded sheet this cell draws from
    PaletteId     palette{};     // which uploaded palette colours it
    bool          flipX   = false;
    bool          flipY   = false;
};
```

There is **no per-layer set and no cap.** Because each cell carries its sheet and palette, **one tile
layer freely mixes any number of sheets and any number of palettes** — a font sheet and a menu-border
sheet, several palettes, all in the same layer, with no slot limit. Getting more than one palette on
screen is just data: different cells carry different palette handles, and a palette swap is rewriting
a handle. Sprites work identically — `Sprite::atlas` and `Sprite::palette` name the sprite's own sheet
and palette, so one sprite layer mixes sheets and palettes freely too.

`TileContent` and `SpriteContent` carry no atlas or palette of their own — they hold the cells /
sprites, each self-describing:

```cpp
struct TileContent {
    int                       widthInTiles  = 0;
    int                       heightInTiles = 0;
    std::span<const TileCell> cells;            // row-major, each cell names its sheet + palette
    TileWrap                  wrap = TileWrap::Repeat;
};

struct SpriteContent {
    std::span<const Sprite> sprites;            // each sprite names its sheet + palette
};
```

Palettes are **arbitrary size** — there is no 256-colour cap — and an atlas pixel widens to a full
index, so one palette can hold as many colours as you upload and a tile (or sprite) pixel can address
any of them. A `PaletteId`'s underlying value *is* its flat offset into the palette store, and an
`AtlasId` names a region of the flat atlas store, so the shader reads both directly with no
indirection — you never manage per-sheet textures.

This is the mechanism that lets a small palette render a full-colour map: a single indexed tileset,
drawn through several palettes named per cell, produces a multi-coloured scene.

## The `tiles()` helper — a single-combo cell run

When a run of cells all draw from **one** sheet + palette, `tiles()` fills the repeated handles for
you over a list of slots, so you don't hand-write the same `atlas`/`palette` on every literal:

```cpp
#include "retropp/tilemap.h"   // tiles()

// Three cells, all from `fontAtlas` coloured by `textPal`, drawing slots 7, 8, 9 in order:
std::vector<TileCell> run = tiles(fontAtlas, textPal, {7, 8, 9});
```

It returns one `TileCell` per slot, in order, with no flip — plain mutable data, so set a flip or a
different handle on any cell afterward for anything that varies. It's the single convenience over
hand-writing `TileCell{ .tile = …, .atlas = …, .palette = … }` literals for the common single-combo
run; a layer mixing several sheets just concatenates several runs (or builds the cells from a map image
and a catalog — see **[tilemaps.md](tilemaps.md)**).

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

- **Recolour a scene without new art:** rewrite the `palette` handle on the cells / sprites you want
  recoloured — they each name their own.
- **Animate colour (water shimmer, palette cycling):** re-upload a palette (`uploadPalette` returns a
  new `PaletteId`) or rewrite the `palette` handle on a cell / sprite per frame — both are data changes,
  no shader edit.
- **Whole-frame fades / day-night:** that's a `ColorFill` region + a blend mode
  ([draw-state.md](draw-state.md#whole-frame-colour)) grading the composited frame — distinct from the
  per-pixel palette colouring here.
- **Swap an entire tileset:** upload a new atlas and reference its `AtlasId` from the layer.
