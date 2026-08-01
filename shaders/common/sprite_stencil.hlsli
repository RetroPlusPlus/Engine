// Stencil coverage and the inline-polygon signed distance a sprite region is cut against.
//
// Engine-internal header — see blend_ops.hlsli for why shaders/common is separate from shaders/include.
// Self-contained: the polygon arrives as a parameter, so this reads no shader-declared state.
//
// Distinct from polygon_sdf.hlsli's sdPolygon, which reads the uPoints cbuffer of the frame-class region
// passes. Here the vertices ride in the sprite's own record, inline and bounded at 8.

#ifndef RETROPP_COMMON_SPRITE_STENCIL_HLSLI
#define RETROPP_COMMON_SPRITE_STENCIL_HLSLI

// Stencil coverage (how far inside, ramped over feather) and survival factor. Mirrors stencilCoverage /
// stencilSurvival. mode: 0 = TransparentInside, 1 = TransparentOutside.
float stencilCoverage(float sd, float feather) {
    if (feather > 0.0f) return saturate(0.5f - sd / feather);
    return sd <= 0.0f ? 1.0f : 0.0f;
}
float stencilSurvival(uint mode, float cov) { return mode == 0u ? (1.0f - cov) : cov; }

// Signed distance from a quad-space point to a record's inline polygon (1 pt = circle, 2 = capsule,
// ≥3 = polygon winding), then radius-inflated + stroke-banded. Mirrors sdPolygon + bandSignedDistance +
// spriteRegionSignedDistance. pointCount 0 ⇒ inside everywhere.
float spriteRegionSignedDistance(float2 p, float2 v[8], uint n, float radius, float stroke) {
    if (n == 0u) return -1e30f;
    float d;
    if (n == 1u) {
        d = length(p - v[0]);
    } else {
        float dd = dot(p - v[0], p - v[0]);
        float s  = 1.0f;
        for (uint i = 0u; i < n; i++) {
            uint   j = (i == 0u) ? (n - 1u) : (i - 1u);
            float2 e = v[j] - v[i];
            float2 w = p - v[i];
            float  ee = dot(e, e);
            float  t  = ee > 0.0f ? clamp(dot(w, e) / ee, 0.0f, 1.0f) : 0.0f;
            float2 bb = w - e * t;
            dd = min(dd, dot(bb, bb));
            if (n >= 3u) {
                bool c1 = p.y >= v[i].y;
                bool c2 = p.y <  v[j].y;
                bool c3 = e.x * w.y > e.y * w.x;
                if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
            }
        }
        d = s * sqrt(dd);
    }
    d = d - radius;
    if (stroke > 0.0f) d = abs(d) - stroke * 0.5f;
    return d;
}

#endif  // RETROPP_COMMON_SPRITE_STENCIL_HLSLI
