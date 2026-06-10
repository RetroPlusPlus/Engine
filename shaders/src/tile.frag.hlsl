// Tile layer fragment shader.
//
// Per output pixel: reconstruct the layer-local pixel from the interpolated UV × the layer
// size, add the layer scroll, wrap toroidally into the tilemap, read the tile index from the
// tilemap, address the atlas cell, sample (nearest) the atlas texel, and scale it by the
// layer alpha. This mirrors gbcpp::sampleTilemap (the unit-tested reference) exactly — the
// wrap is done with floor() in float because HLSL integer % is undefined for negative
// operands across backends, so we first wrap to a NON-NEGATIVE world coordinate, then the
// integer tile/pixel split uses only non-negative operands.
//
// SDL_GPU HLSL conventions (see SDL_CreateGPUShader docs): for fragment shaders, sampled
// textures + samplers and read-only storage textures share register space2 in the t/s
// register files (sampled first, then storage); uniform buffers live in space3 (b).
//   - t0/s0 space2 : atlas (sampled, nearest)
//   - t1   space2 : tilemap index texture (read-only storage; integer Load, no sampler)
//   - b0   space3 : per-layer uniforms
//
// Per-tile attributes (TileCell::attributes) and the per-layer ScreenSpaceEffect are ignored
// here at ENG-2.B.2.a (attributes → ENG-2.B.2.b; effect → ENG-2.C).

Texture2D<float4> uAtlas        : register(t0, space2);
SamplerState      uAtlasSampler : register(s0, space2);
Texture2D<uint>   uTilemap      : register(t1, space2);

cbuffer TileUniforms : register(b0, space3) {
    float2 uScroll;       // layer scroll, pixels
    float2 uLayerSize;    // layer destination size, viewport pixels
    float2 uTilemapSize;  // tilemap dimensions, tiles (width, height)
    float2 uAtlasSize;    // atlas dimensions, tiles (cols, rows)
    float  uTilePx;       // tile edge length, pixels (8)
    float  uAlpha;        // layer alpha, [0,1]
    float2 uPad;
};

// Floored modulo via floor() — well-defined for any sign, unlike HLSL integer %.
float floorModF(float a, float p) { return a - floor(a / p) * p; }

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

    uint tileIndex = uTilemap.Load(int3(tileX, tileY, 0));

    int atlasCols = (int)uAtlasSize.x;
    int atlasCol  = (int)tileIndex % atlasCols;
    int atlasRow  = (int)tileIndex / atlasCols;

    // Atlas texel centre → normalized UV for a nearest sample (exact texel pick).
    float2 atlasPx = float2(atlasCol * tilePx + pixelX, atlasRow * tilePx + pixelY) + 0.5f;
    float2 atlasUV = atlasPx / (uAtlasSize * uTilePx);
    float4 texel = uAtlas.Sample(uAtlasSampler, atlasUV);

    return float4(texel.rgb, texel.a * uAlpha);
}
