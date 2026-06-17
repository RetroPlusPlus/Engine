# Tilemaps — building a tile layer from images

**Headers:** `tilemap.h`, `image.h` (`IndexGrid`, `loadMapPng`), `draw_state.h` (`TileContent::atlases`, `TileCell::atlasSelect`)

This page covers building a tile layer's content from **authored images** instead of hand-written
cell arrays: a *map* image of tile indices plus one or more *atlas* sheets, assembled into the
`cells` + atlas/palette sets a [`TileContent`](tiles-and-colour.md) wants. It also covers the
multi-atlas tile layer — one layer drawing tiles from **several sheets at once** — which this path
relies on.

If you only need the low-level draw-state surface (hand-built `cells`, the palette model, flips,
wrap modes), read [tiles-and-colour.md](tiles-and-colour.md) and [draw-state.md](draw-state.md)
first; this page builds on both. Loading the atlas sheets themselves (PNG → atlas) is in
[images-and-transparency.md](images-and-transparency.md).

---

## The shape of it

A tile layer is `cells` (a grid of `TileCell`) drawn through an *atlas* (the tile art) and a
*palette set* (the colours). The hand-written way is to fill the `cells` yourself and point a
`TileContent` at one atlas. The image-driven way is:

```
 map PNG ──loadMapPng──► IndexGrid ──┐
                                     ├──assembleTilemap──► AssembledTilemap ──asTileContent──► TileContent
 TileCatalog (declared) ─────────────┘
```

- **`IndexGrid`** — the map, decoded to one raw `uint16` index per cell.
- **`TileCatalog`** — a declared table mapping each index to a tile: *which sheet, which cell,
  which palette, flipped how*.
- **`AssembledTilemap`** — the result: the `cells` array plus the deduplicated **atlas set** and
  **palette set** the cells select within, ready to hand to a layer.

Each step is independent and testable; nothing here touches the GPU until you submit the resulting
`TileContent` in a frame.

---

## Multi-atlas tile layers

A `TileContent` historically carried one `atlas`. It now also carries an **atlas set** — and a
`TileCell` carries an **atlas-select** — exactly mirroring the palette set it already had:

```cpp
struct TileCell {
    std::uint16_t tile        = 0;  // cell index within the SELECTED atlas
    std::uint8_t  palette     = 0;  // which palette in the layer's set
    bool          flipX       = false;
    bool          flipY       = false;
    std::uint8_t  atlasSelect = 0;  // which ATLAS in the layer's set
};

struct TileContent {
    AtlasId                    atlas{};     // the single sheet when `atlases` is empty
    std::span<const PaletteId> palettes;    // palette set; TileCell::palette selects within
    int                        widthInTiles, heightInTiles;
    std::span<const TileCell>  cells;
    TileWrap                   wrap = TileWrap::Repeat;
    std::span<const AtlasId>   atlases = {}; // atlas set; TileCell::atlasSelect selects within
};
```

So **one layer can mix tiles from several sheets** — e.g. a font sheet for text and a border sheet
for a menu frame — by giving each cell a different `atlasSelect`. The two sets are independent: a
cell picks its sheet (`atlasSelect`) and its palette (`palette`) separately.

**The single-atlas path is unchanged and faithful by default.** If `atlases` is empty, the layer
uses the one `atlas` field and `atlasSelect` is ignored — byte-for-byte the behavior from before
this feature. You only opt into multi-atlas by populating `atlases`.

Under the hood the renderer keeps every uploaded atlas in one *flat atlas store* (the same pattern
the palette store uses — see [tiles-and-colour.md](tiles-and-colour.md)); a layer's atlas set
resolves to per-sheet regions the shader indexes by `atlasSelect`. You don't manage any of that —
you just list the sheets in `atlases` and set `atlasSelect` per cell (or let `assembleTilemap` do
it for you, below). The set holds up to `kAtlasSetSlots` (16) sheets per layer; `atlasSelect` is a
6-bit field with headroom beyond that.

---

## The map image — `loadMapPng` → `IndexGrid`

A *map* is not art; it's a grid of **numbers**. Each pixel's sample value **is** a tile index — for
a tilemap, an index into a `TileCatalog`; for a collision map, a raw id your game interprets.

```cpp
struct IndexGrid {
    int                        width = 0, height = 0;
    std::vector<std::uint16_t> values;   // one raw index per pixel, row-major
    std::uint16_t at(int x, int y) const;
};

IndexGrid loadMapPng(const std::filesystem::path& path);
IndexGrid loadMapPngFromMemory(std::span<const std::uint8_t> bytes);
```

- **16-bit grayscale is the headline format** — a tilemap may reference more than 256 tiles, and a
  16-bit sample carries ids up to 65535. (8-bit and sub-byte grayscale, and palette PNGs, also
  decode — their indices just widen into the `uint16` grid.)
- The sample value is the index, **never** scaled or reverse-derived from a colour — the same
  faithful "the stored value is the index" rule the atlas path uses.
- **Truecolour is rejected** (`loadMapPng` throws): a map carries indices, not colours.
- A 16-bit map of small ids looks near-black in an image viewer (id 15 is `15/65535` grey) — that's
  inherent to an index map, not a bug. If you want a map you can eyeball, spread the ids across the
  range (the demo does — see below).

**Collision maps use this exact function.** Decode your collision PNG with `loadMapPng` and read
`IndexGrid::values` directly; the engine assigns the numbers no meaning — collision is your game's
logic.

---

## The catalog — `TileCatalog`

A `TileCatalog` is a plain declared table. Each entry says what a map value draws:

```cpp
struct TileCatalogEntry {
    std::uint16_t id;          // the map value that selects this tile — identity, first field
    AtlasId       sheet;       // which loaded atlas
    std::uint16_t slot;        // the 8px cell index within that sheet (an AssetSlot::tile)
    PaletteId     palette;     // which uploaded palette colours it
    bool          flipX = false, flipY = false;
};

struct TileCatalog { std::vector<TileCatalogEntry> entries; };
```

**`id` is the entry's identity, not its position.** A map value selects the entry whose `id` matches
it, so ids are **sparse 16-bit values** — you can scatter them across `0..65535` freely (which is
also what genuinely exercises a 16-bit map). Build it inline:

```cpp
TileCatalog cat;
cat.entries = {
    {.id = 0,    .sheet = menu, .slot = 3, .palette = menuPal},                 // interior fill
    {.id = 4369, .sheet = menu, .slot = 0, .palette = menuPal},                 // top-left corner
    {.id = 8738, .sheet = menu, .slot = 0, .palette = menuPal, .flipX = true},  // top-right (mirrored)
    {.id = 39321,.sheet = font, .slot = 1, .palette = textPal},                 // letter 'H'
    // …
};
```

**Flips reuse a slot instead of storing another tile.** A rectangular menu border needs only four
distinct menu tiles — an outer corner, a horizontal edge, a vertical edge, and a fill — because a
flip mirrors a tile (it does **not** rotate 90°): the corner flips into all four corners, the
horizontal edge flips top↔bottom, the vertical edge flips left↔right. Don't author a tile a flip can
produce from one you already have.

---

## Assembling — `assembleTilemap` → `AssembledTilemap`

```cpp
struct AssembledTilemap {
    std::vector<TileCell>  cells;          // row-major widthInTiles * heightInTiles
    std::vector<AtlasId>   atlases;        // deduped atlas set (first-seen order)
    std::vector<PaletteId> palettes;       // deduped palette set (first-seen order)
    int                    widthInTiles = 0, heightInTiles = 0;

    TileContent asTileContent(TileWrap wrap = TileWrap::Repeat) const;  // one-call sugar
};

AssembledTilemap assembleTilemap(const IndexGrid& map, const TileCatalog& catalog);
```

`assembleTilemap` looks each map value up by `id`, collects the **distinct sheets** the map actually
uses into `atlases` and the **distinct palettes** into `palettes` (first-seen order), and emits a
`cells` array whose `atlasSelect` / `palette` index those sets. So a map mixing a font sheet and a
menu sheet comes out as one layer with a two-sheet atlas set and the right per-cell selects — the
multi-atlas result, computed for you.

It throws on a map value with no matching `id` (`std::out_of_range`), a duplicate catalog `id`
(`std::invalid_argument`), or more distinct sheets/palettes than a layer's sets hold
(`std::length_error`, caps `kAtlasSetSlots` / `kPaletteSetSlots`). An empty grid yields an empty
result.

### Handing it to a layer

`asTileContent(wrap)` is one-call sugar — it fills `cells`/`atlases`/`palettes`/dimensions into a
`TileContent` (the one display choice it can't know, the wrap mode, is the argument):

```cpp
AssembledTilemap map = assembleTilemap(loadMapPng("level1.png"), catalog);

DrawLayer layer;
layer.id      = "World";
layer.size    = {160, 144};
layer.content = map.asTileContent(TileWrap::Blank);   // finite map: outside the bounds is transparent
```

**It's not fixed — edit it freely.** `AssembledTilemap` holds plain mutable vectors. After
assembling, mutate `cells` (flip one, repoint its `atlasSelect`, swap a palette), or push/erase
cells, on the fly. `asTileContent()` hands out spans pointing **into** those vectors, so in-place
edits show up the next frame — but if a `push_back` **reallocates** a vector, call `asTileContent()`
again so the spans don't dangle. The fully-manual route (set `TileContent::cells`/`atlases`
yourself, no `AssembledTilemap` at all) is always available for layers that rebuild their tiles
every frame.

---

## Worked example — `examples/tilemap_demo.cpp`

The demo draws "HELLO / WORLD" inside a gold menu frame, in **one** tile layer mixing **two image
sheets**:

1. `loadAtlas` two indexed sheets — a font (`H E L O W R D` glyphs) and a menu border (corner, h-edge,
   v-edge, fill), each an 8×8 `Tileset` — → an `AtlasManifest` (the uploaded atlas + its sliced slots).
   (A PNG always goes through `loadAtlas`, never `loadPng`+`uploadAtlas` — `uploadAtlas` is for raw
   index arrays you author yourself; handing it a `LoadedImage` throws.)
2. `loadMapPng` a 16-bit grayscale map whose ids are spread across `0..65535` (so it both exercises
   16-bit decode and is faintly eyeball-distinguishable).
3. Declare a `TileCatalog`: the menu border tiles (with flips for the other corners/edges) on the
   menu sheet, the letters on the font sheet, each with its palette.
4. `assembleTilemap` → an `AssembledTilemap` whose atlas set is `{ menu, font }` and palette set is
   `{ menuPal, textPal }`; `asTileContent(TileWrap::Blank)` into the layer.

The frame's letters come from the font sheet and its border from the menu sheet, mixed per cell, in
a single submission — the headline of this feature.

---

## Where to change things

- **The decode** lives in `src/image.cpp` (`loadMapPng` + the 16-bit sample unpack). Add a map source
  format here if you ever need one beyond grayscale/palette PNG.
- **The assembly** is `src/tilemap.cpp` (`assembleTilemap` + the id lookup and set dedup). A future
  *alternate* assembler — a different way to turn source data into an `AssembledTilemap`, engine- or
  game-side — would live alongside it and produce the same type; that's deliberately why the type is
  named for the result (`AssembledTilemap`), not for one way of producing it.
- **The multi-atlas storage** (the flat atlas store + per-layer region uniform + the tile/sprite
  shaders that index it) is in `src/renderer.cpp` and `shaders/src/tile.frag.hlsl` /
  `sprite.frag.hlsl`. You don't touch this to *use* multi-atlas; it's where the mechanism lives if
  you're forking the renderer.

> **Naming note.** `AssembledTilemap` is intentionally distinct from the engine's *internal*
> "tilemap" vocabulary — `sampleTilemap()` (the cell-grid sampling math), `TilemapTex` (the
> renderer's GPU cell texture), and `TileContent::cells` — so the public assembled bundle and the
> internal cell grid never blur.
