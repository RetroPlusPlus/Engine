// Sprite layer vertex shader (instanced quads; projective transform).
//
// Each sprite is one instance; six SV_VertexID values trace the two triangles of a unit quad.
// The per-sprite record is read from a storage buffer indexed by SV_InstanceID — there is NO
// vertex buffer (the engine's integer-Load storage idiom, matching the tile path's storage
// textures). The record holds the COMPOSED clip-space homography H (three rows) that maps a unit-
// quad corner directly to clip-space homogeneous coordinates: the whole chain — unit→sprite-pixel
// scale, the per-sprite Transform, the scrolled top-left, the per-layer Transform, and screen→clip
// (viewport scale + top-left-origin V-flip) — is baked CPU-side in retropp::makeGpuSprite, so this
// stage needs NO uniform buffer. That is deliberate: a vertex stage with both a storage buffer and
// a uniform buffer collides in Metal's [[buffer]] namespace under the single-pass HLSL→SPIR-V→MSL
// toolchain (SDL_GPU offsets storage buffers past the uniform buffers, which the toolchain can't
// express alongside Vulkan's descriptor layout). Emitting the real
// homogeneous w (row2 · corner) lets the GPU perspective-divide the position AND interpolate the
// within-sprite UV perspective-correct for free. tile / paletteOffset / flags / packed-size are passed
// flat (nointerpolation) to the fragment stage.
//
// SDL_GPU HLSL conventions (see SDL_CreateGPUShader docs): a vertex stage's read-only storage
// buffer is the only buffer here, at t0 space0.
//   - t0 space0 : sprite records (StructuredBuffer<GpuSprite>; integer index by SV_InstanceID)

// Mirrors retropp::GpuSprite (128 bytes): row0/row1/row2 = the composed unit-quad-corner → clip forward
// homography H (row-major; row0's 4th lane carries the per-sprite alpha, row1/row2's is padding);
// inv0/inv1/inv2 = the screen→unit INVERSE homography the
// fragment's analytic branch consults; attr = (tile, atlasPalette, flags, size) where atlasPalette packs
// the atlas handle (low 16) and palette flat offset (high 16), and size is the pixel dimensions packed
// (width<<16)|height. fx = (fxOffset, fxCount, _, _): the sprite's slice of the per-frame sprite-effect
// record store (fx.x = first record, fx.y = record count; 0 count = no effect). clip = H · (cx, cy, 1);
// placement = clip.xy / clip.w.
struct GpuSprite {
    float4 row0;
    float4 row1;
    float4 row2;
    float4 inv0;
    float4 inv1;
    float4 inv2;
    uint4  attr;
    uint4  fx;
};

StructuredBuffer<GpuSprite> uSprites : register(t0, space0);

struct Output {
    float2 spriteUV   : TEXCOORD0;                  // [0,1] within-sprite, interpolated (perspective-correct)
    nointerpolation uint tile         : TEXCOORD1;
    nointerpolation uint atlasPalette : TEXCOORD2;  // atlas (low 16) | palette flat offset (high 16)
    nointerpolation uint flags        : TEXCOORD3;
    nointerpolation uint size         : TEXCOORD4;  // packed (width<<16)|height, pixels
    nointerpolation float3 inv0 : TEXCOORD5;        // screen→unit inverse row 0 (m00,m01,m02)
    nointerpolation float3 inv1 : TEXCOORD6;        //   row 1 (m10,m11,m12)
    nointerpolation float3 inv2 : TEXCOORD7;        //   row 2 (m20,m21,m22) — perspective terms
    nointerpolation float  spriteAlpha : TEXCOORD8; // per-sprite alpha (row0.w) → the fragment's opacity multiply
    nointerpolation uint   fxOffset    : TEXCOORD9;  // first sprite-effect record index (fx.x)
    nointerpolation uint   fxCount     : TEXCOORD10; // sprite-effect record count (fx.y); 0 = no effect
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

    // clip = H · (corner, 1); emit the real homogeneous w so the GPU perspective-divides (a tilted
    // sprite foreshortens) and interpolates spriteUV perspective-correct. Affine sprites have w ≡ 1.
    float3 c = float3(corner, 1.0f);
    float cx = dot(s.row0.xyz, c);
    float cy = dot(s.row1.xyz, c);
    float cw = dot(s.row2.xyz, c);

    Output output;
    output.pos        = float4(cx, cy, 0.0f, cw);
    output.spriteUV   = corner;
    output.tile         = s.attr.x;
    output.atlasPalette = s.attr.y;
    output.flags        = s.attr.z;
    output.size         = s.attr.w;
    output.inv0         = s.inv0.xyz;   // screen→unit inverse, flat to the fragment's analytic branch
    output.inv1         = s.inv1.xyz;
    output.inv2         = s.inv2.xyz;
    output.spriteAlpha  = s.row0.w;      // per-sprite alpha rides row0's 4th lane (makeGpuSprite packs Sprite::alpha there)
    output.fxOffset     = s.fx.x;        // this sprite's slice of the per-frame sprite-effect record store
    output.fxCount      = s.fx.y;        // 0 ⇒ the fragment early-outs (no effect)
    return output;
}
