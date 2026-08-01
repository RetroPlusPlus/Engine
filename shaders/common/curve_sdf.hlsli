// Signed-distance geometry for quadratic-Bezier region boundaries.
//
// Engine-internal header — see blend_ops.hlsli for why shaders/common is separate from shaders/include.
//
// REQUIRES the includer to have already declared `uSegs` (the packed segment array: two float4 per
// segment, start.xy / ctrl.zw then end.xy / degree.z). The accessors below read it directly, so this
// header is included AFTER the shader's cbuffer, not at the top of the file.

#ifndef RETROPP_COMMON_CURVE_SDF_HLSLI
#define RETROPP_COMMON_CURVE_SDF_HLSLI

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

#endif  // RETROPP_COMMON_CURVE_SDF_HLSLI
