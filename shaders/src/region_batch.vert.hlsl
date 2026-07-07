// Batched-region vertex shader (instanced additive quads) — engine-internal.
//
// The fast path that collapses N same-shader additive region-confined effects into ONE render pass:
// each region is one covering quad, drawn as an instance, and the batched fragment (the game's additive
// custom shader, delta-extracted) accumulates through hardware additive blending. This stage rasterizes
// each region's bounding box and hands the fragment the SAME frame-global uv the fullscreen post-process
// pass would have given it, plus the region's SDF spine + radius so the fragment can replicate the region
// gate. 6 × SV_VertexID unit-quad corners; the per-instance record is read from a storage buffer indexed
// by SV_InstanceID — no vertex buffer.
//
// STORAGE-ONLY (no uniform buffer), deliberately — the sprite.vert precedent: a vertex stage that mixes a
// storage buffer AND a uniform buffer collides in Metal's [[buffer]] namespace under the single-pass
// HLSL→SPIR-V→MSL toolchain (--msl-decoration-binding maps both t0 and b0 to [[buffer(0)]]). So the box is
// baked in UV space CPU-side (no compose-dims uniform needed) and each run draws from its own buffer with
// first_instance = 0 (no baseInstance uniform needed) — uRecords[instanceID] is 0-based and correct on
// every backend. Resource: t0 space0 = the instance records (StructuredBuffer; integer index).

// Mirrors the renderer's GpuRegionBatch (48 bytes): uvBox = the covering quad in normalized frame uv
// (u0, v0, u1, v1 — regionScissorRect, converted px→uv); spine = the shape's SDF spine (p0.xy, p1.xy) in
// VIEWPORT px (a circle repeats p0); radiusPad.x = the SDF radius in viewport px (yzw padding).
struct RegionBatchRecord {
    float4 uvBox;
    float4 spine;
    float4 radiusPad;
};

StructuredBuffer<RegionBatchRecord> uRecords : register(t0, space0);

struct Output {
    float2 uv    : TEXCOORD0;  // frame-global uv — the SAME uv the fullscreen pass gives the fragment
    float4 spine : TEXCOORD1;  // (p0.xy, p1.xy), viewport px
    float2 rad   : TEXCOORD2;  // (radius, pad), viewport px
    float4 pos   : SV_Position;
};

Output main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    // Two triangles of a unit quad: {(0,0),(1,0),(0,1)} and {(0,1),(1,0),(1,1)}.
    const float2 corners[6] = {
        float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(0.0f, 1.0f),
        float2(0.0f, 1.0f), float2(1.0f, 0.0f), float2(1.0f, 1.0f),
    };
    float2 corner = corners[vertexID];

    RegionBatchRecord rec = uRecords[instanceID];

    // The corner's frame-global uv (interpolate the box's uv corners), then px→NDC with the top-left-origin
    // V-flip — the exact mapping postprocess.vert uses, so the fragment sees the fullscreen pass's uv.
    float2 uv = float2(lerp(rec.uvBox.x, rec.uvBox.z, corner.x),
                       lerp(rec.uvBox.y, rec.uvBox.w, corner.y));

    Output output;
    output.pos   = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv    = uv;
    output.spine = rec.spine;
    output.rad   = rec.radiusPad.xy;
    return output;
}
