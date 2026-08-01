// Signed-distance geometry for polygon region boundaries.
//
// Engine-internal header — see blend_ops.hlsli for why shaders/common is separate from shaders/include.
//
// REQUIRES the includer to have already declared `uPoints` (vertices packed two per float4), so this
// header is included AFTER the shader's cbuffer, not at the top of the file.

#ifndef RETROPP_COMMON_POLYGON_SDF_HLSLI
#define RETROPP_COMMON_POLYGON_SDF_HLSLI

float2 regionPoint(uint i) {
    float4 packed = uPoints[i >> 1u];
    return (i & 1u) != 0u ? packed.zw : packed.xy;
}

// Mirror of retropp::sdPolygon: winding-number sign + min-edge distance; degenerates to point (n==1)
// and segment (n==2) distance so one routine covers circle / capsule / polygon.
float sdPolygon(float2 p, uint n) {
    float2 v0 = regionPoint(0u);
    if (n == 1u) {
        return length(p - v0);
    }
    float d = dot(p - v0, p - v0);  // squared distance, seeded at vertex 0
    float s = 1.0;
    uint j = n - 1u;
    for (uint i = 0u; i < n; ++i) {
        float2 vi = regionPoint(i);
        float2 vj = regionPoint(j);
        float2 e = vj - vi;
        float2 w = p - vi;
        float ee = dot(e, e);
        float t = ee > 0.0 ? clamp(dot(w, e) / ee, 0.0, 1.0) : 0.0;
        float2 b = w - e * t;
        d = min(d, dot(b, b));
        if (n >= 3u) {
            bool c1 = p.y >= vi.y;
            bool c2 = p.y <  vj.y;
            bool c3 = (e.x * w.y) > (e.y * w.x);
            if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
        }
        j = i;
    }
    return s * sqrt(d);
}

#endif  // RETROPP_COMMON_POLYGON_SDF_HLSLI
