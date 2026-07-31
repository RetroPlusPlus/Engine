// Rect-instanced emission extract vertex shader (below-scope fields) — engine-internal.
//
// One instance per below-scope Bloom / Glow field: the instance's quad is that field's RECT in the
// emission atlas, so a layer's whole set of below fields extracts in ONE draw however many lenses it
// authors. Six SV_VertexID corners trace the rect; the per-instance record comes from a storage buffer
// indexed by SV_InstanceID — no vertex buffer.
//
// STORAGE-ONLY (no uniform buffer), the sprite.vert / region_batch.vert precedent: a vertex stage that
// mixes a storage buffer AND a uniform buffer collides in Metal's [[buffer]] namespace under the
// single-pass HLSL→SPIR-V→MSL toolchain. So the rect arrives already in atlas UV, converted CPU-side,
// and each draw starts at first_instance 0. Resource: t0 space0 = the instance records as a
// ByteAddressBuffer (byte address = instanceID * 32), NOT a StructuredBuffer: SDL's D3D12 backend leaves
// StructureByteStride 0, which AMD uses to index a StructuredBuffer, collapsing every dynamic index to
// element 0. Byte addressing is stride-independent — correct on every driver.

// Mirrors the renderer's GpuEmissionExtract (32 bytes): uvBox = the field's rect in atlas uv
// (u0, v0, u1, v1); read = (offsetX, offsetY, threshold, glow) — the offset that carries an atlas texel
// back to the viewport position it holds, the emission floor, and which extract produced it
// (0 = Bloom, 1 = Glow).
struct EmissionExtractRecord {
    float4 uvBox;
    float4 read;
};

ByteAddressBuffer uRects : register(t0, space0);

struct Output {
    nointerpolation float4 read : TEXCOORD0;  // (offsetX, offsetY, threshold, glow)
    float4 pos : SV_Position;
};

Output main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    // Two triangles of a unit quad: {(0,0),(1,0),(0,1)} and {(0,1),(1,0),(1,1)}.
    const float2 corners[6] = {
        float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(0.0f, 1.0f),
        float2(0.0f, 1.0f), float2(1.0f, 0.0f), float2(1.0f, 1.0f),
    };
    float2 corner = corners[vertexID];

    uint base = instanceID * 32u;
    EmissionExtractRecord rec;
    rec.uvBox = asfloat(uRects.Load4(base + 0u));
    rec.read  = asfloat(uRects.Load4(base + 16u));

    float2 uv = float2(lerp(rec.uvBox.x, rec.uvBox.z, corner.x),
                       lerp(rec.uvBox.y, rec.uvBox.w, corner.y));

    Output output;
    output.pos  = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.read = rec.read;
    return output;
}
