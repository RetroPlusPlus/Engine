// Region-stencil post-process fragment shader — curve boundary (analytic linear + quadratic).
//
// The curve-boundary peer of region_stencil.frag: the same see-through (source × survival, one texture), but
// the boundary is a CLOSED CURVE of Linear and Quadratic Bezier segments instead of a straight-edged
// polygon — exact between control points, no facets, no vertex cap. Containment mirrors
// retropp::sdCurveAnalytic exactly (postprocess.h): the unsigned distance is the closed-form Bezier
// distance (a depressed-cubic solve for the quadratic), the sign is an even-odd +x ray cast (half-open
// [0,1) per segment), and the fragment is mapped into shape-local space by the region transform's
// inverse homography before the SDF. A boundary carrying a cubic segment is sampled to a polygon on the
// CPU and routed to region_stencil.frag instead — this shader only ever sees linear + quadratic.
//
// SDL_GPU HLSL conventions: one fragment sampled texture + sampler in space2 (t0/s0 = the source); the
// uniform buffer in space3.

Texture2D<float4> SrcTexture : register(t0, space2);
SamplerState      SrcSampler : register(s0, space2);

// Per-segment control points ride the cbuffer two registers each (up to kCurveRegionMaxSegments = 32);
// the inverse homography + misc tail mirror region_select_curve.frag exactly; uStencil adds the two
// stencil scalars.
cbuffer CurveStencilUniforms : register(b0, space3) {
    float4 uSegs[64]; // 2 regs/segment (registers 0..63), xy/zw packed
    float4 uInvRow0;  // region transform inverse homography, row 0 (xyz; w = invert flag) — register 64
    float4 uInvRow1;  //                          row 1 (xyz; w = stroke band width, px) — register 65
    float4 uInvRow2;  //                                       row 2             — register 66
    float4 uMisc;     // x = 1/viewportW, y = 1/viewportH, z = segment count, w = radius — register 67
    float4 uStencil;  // x = mode (0 TransparentInside, 1 TransparentOutside), y = feather (px),
                      //   z = snap (1 = viewport grid, crisp); w pad                             — register 68
};

float2 segStart(uint i)  { return uSegs[2u * i].xy; }
float2 segCtrl(uint i)   { return uSegs[2u * i].zw; }
float2 segEnd(uint i)    { return uSegs[2u * i + 1u].xy; }
float  segDegree(uint i) { return uSegs[2u * i + 1u].z; }

// Distance from p to the segment a->b (clamped projection) — the linear distance and the degenerate
// quadratic fallback.
float pointSegDist(float2 p, float2 a, float2 b) {
    float2 ab = b - a;
    float2 ap = p - a;
    float  ee = dot(ab, ab);
    float  t  = ee > 0.0 ? clamp(dot(ap, ab) / ee, 0.0, 1.0) : 0.0;
    return length(p - (a + ab * t));
}

// Sign-preserving cube root (HLSL has no cbrt) — matches std::cbrt in the CPU mirror.
float cbrtf(float x) { return sign(x) * pow(abs(x), 1.0 / 3.0); }

// Exact unsigned distance from pos to the quadratic Bezier A, B(control), C — the closed-form solve
// (mirror of retropp::detail::quadraticDist / curve.cpp's quadraticUnsignedDistance).
float quadDist(float2 pos, float2 A, float2 B, float2 C) {
    float2 a  = B - A;
    float2 b  = A - 2.0 * B + C;
    float  bb = dot(b, b);
    if (bb < 1e-9) return pointSegDist(pos, A, C);
    float2 c  = 2.0 * a;
    float2 d  = A - pos;
    float  kk = 1.0 / bb;
    float  kx = kk * dot(a, b);
    float  ky = kk * (2.0 * dot(a, a) + dot(d, b)) / 3.0;
    float  kz = kk * dot(d, a);
    float  p  = ky - kx * kx;
    float  p3 = p * p * p;
    float  q  = kx * (2.0 * kx * kx - 3.0 * ky) + kz;
    float  h  = q * q + 4.0 * p3;
    float  res;
    if (h >= 0.0) {  // one real root
        float  hs = sqrt(h);
        float  x0 = (hs - q) * 0.5;
        float  x1 = (-hs - q) * 0.5;
        float  t  = clamp(cbrtf(x0) + cbrtf(x1) - kx, 0.0, 1.0);
        float2 w  = d + (c + b * t) * t;
        res = dot(w, w);
    } else {  // three real roots — take the nearest
        float  z  = sqrt(-p);
        float  v  = acos(q / (p * z * 2.0)) / 3.0;
        float  m  = cos(v);
        float  n  = sin(v) * 1.7320508;  // sqrt(3)
        float  t0 = clamp((m + m) * z - kx, 0.0, 1.0);
        float  t1 = clamp((-n - m) * z - kx, 0.0, 1.0);
        float  t2 = clamp((n - m) * z - kx, 0.0, 1.0);
        float2 w0 = d + (c + b * t0) * t0;
        float2 w1 = d + (c + b * t1) * t1;
        float2 w2 = d + (c + b * t2) * t2;
        res = min(dot(w0, w0), min(dot(w1, w1), dot(w2, w2)));
    }
    return sqrt(max(res, 0.0));
}

// Signed distance to the closed curve boundary (negative inside): the min unsigned segment distance,
// signed by an even-odd +x ray cast. Mirror of retropp::sdCurveAnalytic.
float sdCurve(float2 p, uint segCount) {
    float d      = 1e30;
    bool  inside = false;
    for (uint i = 0u; i < segCount; ++i) {
        float2 p0 = segStart(i);
        float2 e  = segEnd(i);
        if (segDegree(i) > 1.5) {  // quadratic
            float2 ctrl = segCtrl(i);
            d = min(d, quadDist(p, p0, ctrl, e));
            float A = p0.y - 2.0 * ctrl.y + e.y;
            float B = 2.0 * (ctrl.y - p0.y);
            float C = p0.y - p.y;
            if (abs(A) < 1e-7) {                       // degenerate to linear in t
                if (abs(B) > 1e-12) {
                    float t = -C / B;
                    if (t >= 0.0 && t < 1.0) {
                        float mt = 1.0 - t;
                        if (mt * mt * p0.x + 2.0 * mt * t * ctrl.x + t * t * e.x > p.x) inside = !inside;
                    }
                }
            } else {
                float disc = B * B - 4.0 * A * C;
                if (disc >= 0.0) {
                    float sq = sqrt(disc);
                    float ta = (-B - sq) / (2.0 * A);
                    float tb = (-B + sq) / (2.0 * A);  // a tangency (ta==tb) toggles twice → cancels
                    if (ta >= 0.0 && ta < 1.0) {
                        float mt = 1.0 - ta;
                        if (mt * mt * p0.x + 2.0 * mt * ta * ctrl.x + ta * ta * e.x > p.x) inside = !inside;
                    }
                    if (tb >= 0.0 && tb < 1.0) {
                        float mt = 1.0 - tb;
                        if (mt * mt * p0.x + 2.0 * mt * tb * ctrl.x + tb * tb * e.x > p.x) inside = !inside;
                    }
                }
            }
        } else {  // linear (cubic chords never reach this shader)
            d = min(d, pointSegDist(p, p0, e));
            float dy = e.y - p0.y;
            if (abs(dy) >= 1e-12) {
                float t = (p.y - p0.y) / dy;
                if (t >= 0.0 && t < 1.0) {
                    if (p0.x + t * (e.x - p0.x) > p.x) inside = !inside;
                }
            }
        }
    }
    return inside ? -d : d;
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 src = SrcTexture.Sample(SrcSampler, uv);

    uint  segCount = (uint)(uMisc.z + 0.5);
    float radius   = uMisc.w;
    uint  mode     = (uint)(uStencil.x + 0.5);
    float feather  = uStencil.y;
    float stroke   = uInvRow1.w;  // > 0 → make a band along the boundary see-through (a ring), not the fill

    // Coverage = how far inside the boundary, ramped over `feather` (mirror of stencilCoverage).
    // segCount 0 → whole viewport inside (coverage 1) — the no-region degenerate.
    float coverage;
    if (segCount == 0u) {
        coverage = 1.0;
    } else {
        // Fragment UV → viewport pixels → shape-local via the inverse homography (perspective divide).
        float2 fragPx = float2(uv.x / uMisc.x, uv.y / uMisc.y);
        // Crisp evaluation (uStencil.z): snap the fragment to its viewport-cell centre before the SDF, so
        // the stencil resolves per viewport pixel (pixel-identical upscale). A no-op at compose scale 1.
        if (uStencil.z != 0.0) fragPx = floor(fragPx) + 0.5;
        float  wgt    = uInvRow2.x * fragPx.x + uInvRow2.y * fragPx.y + uInvRow2.z;
        float2 local  = float2(uInvRow0.x * fragPx.x + uInvRow0.y * fragPx.y + uInvRow0.z,
                               uInvRow1.x * fragPx.x + uInvRow1.y * fragPx.y + uInvRow1.z) / wgt;
        float signedDist = sdCurve(local, segCount) - radius;
        if (stroke > 0.0) signedDist = abs(signedDist) - stroke * 0.5;  // boundary → band (mirror of bandSignedDistance)
        if (uInvRow0.w > 0.5) signedDist = -signedDist;  // region invert: make the opposite side see-through
        coverage = feather > 0.0 ? clamp(0.5 - signedDist / feather, 0.0, 1.0)
                                 : (signedDist <= 0.0 ? 1.0 : 0.0);
    }

    // Survival = mode-selected (mirror of stencilSurvival). Scale all four premultiplied channels.
    float survival = (mode == 0u) ? (1.0 - coverage) : coverage;
    return src * survival;
}
