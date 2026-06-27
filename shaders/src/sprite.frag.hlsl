// Sprite layer fragment shader (indexed atlas + runtime palettes + OBJ transparency).
//
// Per output pixel: turn the interpolated within-sprite UV into a within-sprite pixel, flip it per the
// sprite's flags, resolve the sprite's own atlas to its region in the flat store via the global
// atlas-region table, address the indexed atlas at the sprite's top-left cell origin + that pixel (a
// w×h sprite reads a contiguous w×h atlas rectangle — a 16×16 sprite spans a 2×2 cell block), Load the
// palette INDEX, DISCARD it if the sheet's transparent-index set marks it a hole (structural
// transparency), Load the colour from the palette store at the sprite's palette offset, DISCARD a
// fully-transparent palette entry (material transparency), and scale by the layer alpha. Everything is
// integer Load — no sampler.
//
// SDL_GPU HLSL conventions (see SDL_CreateGPUShader docs): with no sampled textures, the read-only
// storage textures take t0..t2 in space2; the uniform buffer is b0 in space3.
//   - t0 space2 : flat ATLAS STORE (R32_UINT; integer Load; all sheets stacked vertically)
//   - t1 space2 : palette store (RGBA8; integer Load; FLAT colours wrapped W wide → texel (flat%W, flat/W))
//   - t2 space2 : global atlas-region table (R32G32B32A32_UINT; texel x = AtlasId → (storeY, cols, transpMaskLo, transpMaskHi))
//   - b0 space3 : per-layer fragment uniforms (tile px + layer alpha + palette-store width)
//
// Sprites front-composite by layer z — depth is layer order only. Each sprite names its OWN sheet, so
// one sprite layer mixes sheets: the sheet's region is looked up per-sprite from the global table.

Texture2D<uint>   uAtlas        : register(t0, space2);
Texture2D<float4> uPaletteStore : register(t1, space2);
Texture2D<uint4>  uAtlasRegions : register(t2, space2);

cbuffer SpriteFragUniforms : register(b0, space3) {
    float uTilePx;          // register 0: tile edge length, pixels (8)
    float uAlpha;           // layer alpha, [0,1]
    float uPaletteStoreW;   // palette-store row width (colours); flat offset → (f%W, f/W)
    float _pad0;
};

float4 main(float2 spriteUV : TEXCOORD0,
            nointerpolation uint tile         : TEXCOORD1,
            nointerpolation uint atlasPalette : TEXCOORD2,
            nointerpolation uint flags        : TEXCOORD3,
            nointerpolation uint packedSize   : TEXCOORD4) : SV_Target0 {
    int2 sz = int2((int)(packedSize >> 16), (int)(packedSize & 0xFFFFu));  // pixel (width, height)
    // Within-sprite pixel [0,size); clamp guards the spriteUV==1 trailing edge.
    int2 px = clamp(int2(floor(spriteUV * float2(sz))), int2(0, 0), sz - int2(1, 1));

    bool flipX    = (flags & 1u) != 0u;
    bool flipY    = (flags & 2u) != 0u;
    uint rotation = (flags >> 2u) & 3u;
    // Orient the within-sprite pixel: flip first, then the 90° rotation (mirrors retropp::sourceCellTexel
    // with w = sz.x, h = sz.y). A non-square sprite at Rot90/Rot270 transposes the read — permitted.
    if (flipX) px.x = sz.x - 1 - px.x;
    if (flipY) px.y = sz.y - 1 - px.y;
    if (rotation == 1u)      { int rt = px.x; px.x = px.y;            px.y = sz.x - 1 - rt; }  // Rot90
    else if (rotation == 2u) { px.x = sz.x - 1 - px.x; px.y = sz.y - 1 - px.y; }               // Rot180
    else if (rotation == 3u) { int rt = px.x; px.x = sz.y - 1 - px.y;  px.y = rt; }            // Rot270

    uint atlasId       = atlasPalette & 0xFFFFu;       // this sprite's sheet handle
    uint paletteOffset = atlasPalette >> 16;           // this sprite's palette flat offset

    // The sprite's sheet region in the flat store, by its atlas handle: (storeY, cols). An unused/invalid
    // handle reads region 0 → cols 0 → discard (nothing to draw, and never a divide-by-zero).
    uint4 region    = uAtlasRegions.Load(int3((int)atlasId, 0, 0));
    int   storeY    = (int)region.x;
    int   atlasCols = (int)region.y;
    if (atlasCols == 0) discard;

    int tilePx = (int)uTilePx;
    int col    = (int)tile % atlasCols;
    int row    = (int)tile / atlasCols;
    int2 texel = int2(col * tilePx + px.x, storeY + row * tilePx + px.y);

    uint idx = uAtlas.Load(int3(texel, 0));   // palette index 0..N-1

    // Structural transparency: the sheet's transparent-index set is a 64-bit bitmask split across
    // region.z (indices 0–31) and region.w (indices 32–63). When this index is a member it is a HOLE —
    // discard so the layer below shows through. The empty set (the default) discards nothing.
    bool hole = (idx < 32u) ? (((region.z >> idx)         & 1u) != 0u)
              : (idx < 64u) ? (((region.w >> (idx - 32u)) & 1u) != 0u)
                            : false;   // indices ≥ 64 are alpha-only, never a structural hole
    if (hole) discard;

    uint   flat   = paletteOffset + idx;   // flat index into the palette store
    int    W      = (int)uPaletteStoreW;
    float4 colour = uPaletteStore.Load(int3((int)(flat % (uint)W), (int)(flat / (uint)W), 0));
    if (colour.a == 0.0f) discard;         // material transparency: a fully-transparent entry is a hole
    return float4(colour.rgb, colour.a * uAlpha);
}
