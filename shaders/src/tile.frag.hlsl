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
    float  uTilePx;       // tile edge length, pixels (8)
    float  uAlpha;        // layer alpha, [0,1]
    float2 uPad;
    uint4  uSetRows[4];   // palette-set slot → store row; 16 slots packed 4 per register
};

// Floored modulo via floor() — well-defined for any sign, unlike HLSL integer %.
float floorModF(float a, float p) { return a - floor(a / p) * p; }

// uSetRows is uint4[4]; slot s lives at component (s & 3) of register (s >> 2). Mirrors the
// flat std::uint32_t setRows[16] the renderer fills via gbcpp::paletteSetRows.
uint paletteRow(uint slot) { return uSetRows[slot >> 2][slot & 3]; }

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float2 local = floor(uv * uLayerSize);     // layer-local pixel (top-left origin)
    float2 world = local + uScroll;            // scrolled world pixel (may be negative)
    float2 mapPx = uTilemapSize * uTilePx;     // tilemap size in pixels

    int tilePx = (int)uTilePx;
    int ix = (int)floorModF(world.x, mapPx.x); // wrapped to [0, mapPx) → non-negative
    int iy = (int)floorModF(world.y, mapPx.y);

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
    int    row        = (int)paletteRow(paletteSel);                 // its store row
    float4 colour     = uPaletteStore.Load(int3((int)colorIndex, row, 0));

    return float4(colour.rgb, colour.a * uAlpha);
}
