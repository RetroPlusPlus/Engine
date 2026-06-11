// Sprite layer vertex shader (ENG-2.B.2.c.1 — instanced per-sprite quads).
//
// Each sprite is one instance; six SV_VertexID values trace the two triangles of a unit quad.
// The per-sprite record is read from a storage buffer indexed by SV_InstanceID — there is NO
// vertex buffer (the engine's integer-Load storage idiom, matching the tile path's storage
// textures). The record already holds the quad in CLIP space (the screen→clip transform —
// scroll subtraction, viewport scale, top-left-origin V-flip — is baked CPU-side in
// gbcpp::makeGpuSprite), so this stage needs NO uniform buffer. That is deliberate: a vertex
// stage with both a storage buffer and a uniform buffer collides in Metal's [[buffer]] namespace
// under the single-pass HLSL→SPIR-V→MSL toolchain (SDL_GPU offsets storage buffers past the
// uniform buffers, which the toolchain can't express alongside Vulkan's descriptor layout). See
// PLAN Amendment A2. The interpolated corner becomes the within-sprite UV; tile / paletteRow /
// flags / packed-size are passed flat (nointerpolation) to the fragment stage.
//
// SDL_GPU HLSL conventions (see SDL_CreateGPUShader docs): a vertex stage's read-only storage
// buffer is the only buffer here, at t0 space0.
//   - t0 space0 : sprite records (StructuredBuffer<GpuSprite>; integer index by SV_InstanceID)

// Mirrors gbcpp::GpuSprite (32 bytes): clip = (clipX, clipY, clipW, clipH) — quad top-left in clip
// space + clip-space span (clipH negative for the V-flip); attr = (tile, paletteRow, flags, size)
// where size is the pixel dimensions packed (width<<16)|height.
struct GpuSprite {
    float4 clip;
    uint4  attr;
};

StructuredBuffer<GpuSprite> uSprites : register(t0, space0);

struct Output {
    float2 spriteUV   : TEXCOORD0;                  // [0,1] within-sprite, interpolated
    nointerpolation uint tile       : TEXCOORD1;
    nointerpolation uint paletteRow : TEXCOORD2;
    nointerpolation uint flags      : TEXCOORD3;
    nointerpolation uint size       : TEXCOORD4;    // packed (width<<16)|height, pixels
    float4 pos        : SV_Position;
};

Output main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    // Two triangles of a unit quad: {(0,0),(1,0),(0,1)} and {(0,1),(1,0),(1,1)}.
    const float2 corners[6] = {
        float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(0.0f, 1.0f),
        float2(0.0f, 1.0f), float2(1.0f, 0.0f), float2(1.0f, 1.0f),
    };
    float2 corner = corners[vertexID];

    GpuSprite s = uSprites[instanceID];

    Output output;
    output.pos        = float4(s.clip.x + corner.x * s.clip.z,   // baked clip-space quad
                               s.clip.y + corner.y * s.clip.w, 0.0f, 1.0f);
    output.spriteUV   = corner;
    output.tile       = s.attr.x;
    output.paletteRow = s.attr.y;
    output.flags      = s.attr.z;
    output.size       = s.attr.w;
    return output;
}
