// Sprite layer fragment shader (ENG-2.B.2.c.1 — indexed atlas + runtime palettes + OBJ transparency).
//
// Per output pixel: turn the interpolated within-sprite UV into a within-sprite pixel, flip it
// per the sprite's flags, address the indexed atlas at the sprite's top-left cell origin + that
// pixel (a w×h sprite reads a contiguous w×h atlas rectangle starting at the cell origin — a
// 16×16 sprite spans a 2×2 cell block), Load the palette INDEX, DISCARD index 0 (the OBJ
// transparency convention — the background shows through), Load the colour from the resolved
// palette-store row, and scale by the layer alpha. Everything is integer Load — no sampler.
//
// SDL_GPU HLSL conventions (see SDL_CreateGPUShader docs): with no sampled textures, the two
// read-only storage textures take t0/t1 in space2; the uniform buffer is b0 in space3.
//   - t0 space2 : indexed atlas (R8_UINT; integer Load; one palette index per pixel)
//   - t1 space2 : palette store (RGBA8; integer Load; row = palette-store row, col = index)
//   - b0 space3 : per-frame fragment uniforms (atlas cols + tile px + layer alpha)
//
// The `priority` flag bit is read but IGNORED here (B.2.c.1 front-composites by layer z); the
// BG-over-OBJ per-pixel interaction is ENG-2.B.2.c.2.

Texture2D<uint>   uAtlas        : register(t0, space2);
Texture2D<float4> uPaletteStore : register(t1, space2);

cbuffer SpriteFragUniforms : register(b0, space3) {
    float uAtlasCols;  // atlas width in tiles (cols)
    float uTilePx;     // tile edge length, pixels (8)
    float uAlpha;      // layer alpha, [0,1]
    float uPad;
};

float4 main(float2 spriteUV : TEXCOORD0,
            nointerpolation uint tile       : TEXCOORD1,
            nointerpolation uint paletteRow : TEXCOORD2,
            nointerpolation uint flags      : TEXCOORD3,
            nointerpolation uint packedSize : TEXCOORD4) : SV_Target0 {
    int2 sz = int2((int)(packedSize >> 16), (int)(packedSize & 0xFFFFu));  // pixel (width, height)
    // Within-sprite pixel [0,size); clamp guards the spriteUV==1 trailing edge.
    int2 px = clamp(int2(floor(spriteUV * float2(sz))), int2(0, 0), sz - int2(1, 1));

    bool flipX = (flags & 1u) != 0u;
    bool flipY = (flags & 2u) != 0u;
    if (flipX) px.x = sz.x - 1 - px.x;
    if (flipY) px.y = sz.y - 1 - px.y;

    int tilePx    = (int)uTilePx;
    int atlasCols = (int)uAtlasCols;
    int col       = (int)tile % atlasCols;
    int row       = (int)tile / atlasCols;
    int2 texel    = int2(col * tilePx + px.x, row * tilePx + px.y);

    uint idx = uAtlas.Load(int3(texel, 0));   // palette index 0..N-1
    if (idx == 0u) discard;                    // OBJ transparency — index 0 is the hole

    float4 colour = uPaletteStore.Load(int3((int)idx, (int)paletteRow, 0));
    return float4(colour.rgb, colour.a * uAlpha);
}
