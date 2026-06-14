// Tile layer fragment shader (ENG-2.B.2.b — indexed atlas + runtime palettes).
//
// Per output pixel: reconstruct the layer-local pixel from the interpolated UV × the layer
// size, add the layer scroll, wrap toroidally into the tilemap (mirroring gbcpp::sampleTilemap),
// Load the packed tilemap cell (tile / palette-select / flip — mirroring gbcpp::unpackTileCell),
// flip the within-tile offset, Load the palette INDEX from the indexed atlas, resolve the cell's
// palette-select to a palette-store row, Load the colour from the palette store, and scale by
// the layer alpha. Everything is integer Load — there is NO sampler on the tile path now (the
// shared nearest sampler is the blit path's). This is the faithful GB/C model: colour is an
// index plus a palette chosen at render time, applied per-pixel — never a baked-RGBA atlas and
// never a palette-RAM poke.
//
// SDL_GPU HLSL conventions (see SDL_CreateGPUShader docs): with no sampled textures, the three
// read-only storage textures take t0/t1/t2 in space2; the uniform buffer is b0 in space3.
//   - t0 space2 : indexed atlas (R8_UINT; integer Load; one palette index per pixel)
//   - t1 space2 : tilemap cells (R32_UINT; integer Load; packTileCell layout)
//   - t2 space2 : palette store (RGBA8; integer Load; row = palette-store row, col = index)
//   - b0 space3 : per-layer uniforms (+ uSetRows palette-set → store-row map)
//
// The per-layer ScreenSpaceEffect is still ignored here (→ ENG-2.C); `priority` + sprites are
// ENG-2.B.2.c.

Texture2D<uint>   uAtlas        : register(t0, space2);
Texture2D<uint>   uTilemap      : register(t1, space2);
Texture2D<float4> uPaletteStore : register(t2, space2);

cbuffer TileUniforms : register(b0, space3) {
    float2 uScroll;       // layer scroll, pixels
    float2 uLayerSize;    // layer destination size, viewport pixels
    float2 uTilemapSize;  // tilemap dimensions, tiles (width, height)
    float2 uAtlasSize;    // atlas dimensions, tiles (cols, rows)
    float  uTilePx;            // tile edge length, pixels (8)
    float  uAlpha;             // layer alpha, [0,1]
    float  uTransparentIndex;  // per-source index-hole transparency; <0 = none (ENG-2.B.3.a)
    float  uPad1;
    uint4  uSetRows[4];        // palette-set slot → store row; 16 slots packed 4 per register
    float4 uInvRow0;           // ENG-2.D.1: inverse transform homography, row 0 (m00,m01,m02, _) — reg 7
    float4 uInvRow1;           //   row 1 (m10,m11,m12, _)                                          — reg 8
    float4 uInvRow2;           //   row 2 (m20,m21,m22, _) — perspective terms in .x/.y             — reg 9
    uint4  uTransformCtl;      //   x = hasTransform (0/1), y = footprint edge (0 Blank / 1 Stretch),
                               //   z = tilemap wrap (0 Repeat / 1 Clamp / 2 Blank, ENG-2.E)        — reg 10
};

// Floored modulo via floor() — well-defined for any sign, unlike HLSL integer %.
float floorModF(float a, float p) { return a - floor(a / p) * p; }

// uSetRows is uint4[4]; slot s lives at component (s & 3) of register (s >> 2). Mirrors the
// flat std::uint32_t setRows[16] the renderer fills via gbcpp::paletteSetRows.
uint paletteRow(uint slot) { return uSetRows[slot >> 2][slot & 3]; }

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float2 local = floor(uv * uLayerSize);     // layer-local destination pixel (top-left origin)

    // ENG-2.D.1 — per-layer geometric transform. When present, inverse-map the destination pixel
    // through the inverse homography (perspective divide included → the Mode-7-style floor) to the
    // CONTENT pixel to sample, then apply the FOOTPRINT edge policy outside [0, uLayerSize): Blank
    // discards (transparent, the layers below show through — the rotated diamond's corners), Stretch
    // clamps to the footprint edge. When absent (identity), `sample` stays `local` and the path below
    // is byte-identical to the pre-D.1 faithful behaviour.
    float2 sample = local;
    if (uTransformCtl.x != 0u) {
        float cw = uInvRow2.x * local.x + uInvRow2.y * local.y + uInvRow2.z;   // perspective weight
        // Behind the projection (above the Mode-7 horizon, w <= 0): NO content exists there — always
        // blank, in EITHER edge mode. The perspective divide flips sign here, so Stretch's clamp would
        // pin to the (0,0) content corner and smear it across the whole upper wedge (the navy triangle).
        // Conceptually this is the sky above the floor; it must discard before the footprint test.
        if (cw <= 0.0f) discard;
        float cx = (uInvRow0.x * local.x + uInvRow0.y * local.y + uInvRow0.z) / cw;
        float cy = (uInvRow1.x * local.x + uInvRow1.y * local.y + uInvRow1.z) / cw;
        if (cx < 0.0f || cx >= uLayerSize.x || cy < 0.0f || cy >= uLayerSize.y) {
            if (uTransformCtl.y == 0u) discard;                    // Blank → transparent, reveal below
            cx = clamp(cx, 0.0f, uLayerSize.x - 1.0f);             // Stretch → clamp-to-edge
            cy = clamp(cy, 0.0f, uLayerSize.y - 1.0f);
        }
        sample = float2(cx, cy);
    }

    float2 world = sample + uScroll;           // scrolled world pixel (may be negative)
    float2 mapPx = uTilemapSize * uTilePx;     // tilemap size in pixels
    int tilePx = (int)uTilePx;

    // ENG-2.E — per-layer tilemap wrap mode (uTransformCtl.z), mirroring gbcpp::sampleTilemap:
    //   0 Repeat — toroidal floorMod (the faithful default; byte-identical to pre-ENG-2.E)
    //   1 Clamp  — clamp the world coord to the map's last pixel (smear the edge tile)
    //   2 Blank  — finite map: discard outside [0, mapPx) on either axis → transparent, reveal below
    float wx, wy;
    if (uTransformCtl.z == 2u) {               // Blank
        if (world.x < 0.0f || world.x >= mapPx.x || world.y < 0.0f || world.y >= mapPx.y) discard;
        wx = world.x;
        wy = world.y;
    } else if (uTransformCtl.z == 1u) {        // Clamp
        wx = clamp(world.x, 0.0f, mapPx.x - 1.0f);
        wy = clamp(world.y, 0.0f, mapPx.y - 1.0f);
    } else {                                    // Repeat
        wx = floorModF(world.x, mapPx.x);      // wrapped to [0, mapPx) → non-negative
        wy = floorModF(world.y, mapPx.y);
    }
    int ix = (int)wx;
    int iy = (int)wy;

    int tileX  = ix / tilePx;
    int tileY  = iy / tilePx;
    int pixelX = ix % tilePx;
    int pixelY = iy % tilePx;

    // Load + unpack the tilemap cell (mirrors gbcpp::unpackTileCell's bit layout exactly).
    uint packed     = uTilemap.Load(int3(tileX, tileY, 0));
    uint tileIndex  = packed & 0xFFFF;
    uint paletteSel = (packed >> 16) & 0xFF;
    bool flipX      = ((packed >> 24) & 1) != 0;
    bool flipY      = ((packed >> 25) & 1) != 0;

    // Flip the within-tile offset before addressing the atlas cell.
    if (flipX) pixelX = tilePx - 1 - pixelX;
    if (flipY) pixelY = tilePx - 1 - pixelY;

    int atlasCols = (int)uAtlasSize.x;
    int atlasCol  = (int)tileIndex % atlasCols;
    int atlasRow  = (int)tileIndex / atlasCols;
    int2 atlasTexel = int2(atlasCol * tilePx + pixelX, atlasRow * tilePx + pixelY);

    uint   colorIndex = uAtlas.Load(int3(atlasTexel, 0));            // palette index 0..N-1

    // Per-source index-hole transparency (ENG-2.B.3.a): when this layer's atlas declares a
    // transparent index, that index is a HOLE — discard so the lower layer shows through. Gated
    // on uTransparentIndex >= 0, so the default (−1) leaves faithful opaque backgrounds untouched.
    if (uTransparentIndex >= 0.0 && colorIndex == (uint)(uTransparentIndex + 0.5)) discard;

    int    row        = (int)paletteRow(paletteSel);                 // its store row
    float4 colour     = uPaletteStore.Load(int3((int)colorIndex, row, 0));

    return float4(colour.rgb, colour.a * uAlpha);
}
