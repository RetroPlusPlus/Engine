// Glow — the authored-colour aura, one pass. Bloom's sibling over the same gather: where Bloom blurs the
// source's OWN bright colour, Glow blurs a SCALAR emission mask and tints it with a chosen colour, so the
// halo's chroma comes from the tint, never the source. For each pixel, gather the 2-D Gaussian
// neighbourhood of the mask, then add the tinted aura over the untouched source:
//
//   mask(c) = c.a · survive(lumStraight(c), threshold)   // survive = 1 at threshold 0 (whole coverage
//             emits); saturate((lum − t) / max(1 − t, 1/255)) above — Rec. 601 luminance
//   m       = Σ w(dx)·w(dy) · mask(sample(uv + (dx,dy) px)) · uInvNorm²,  (dx,dy) ∈ [−K, K]²
//   w(k)    = exp(−k² / (2σ²)),  σ = max(radius, 0.5) / 2,  K = min(⌈radius⌉, 32)
//   out.rgb = src.rgb + intensity · m · tint                                 // additive light, no clamp
//   out.a   = saturate(src.a + intensity · m)                                // the aura lifts coverage
//
// lumStraight un-premultiplies before keying (rgb / a, guarded at a = 0) so the key reads true brightness,
// not coverage-dimmed light. The mask rides the alpha (coverage), and threshold 0 is the whole-coverage
// emission mode (survival 1): every covered pixel emits fully, dark content included — the aura a dark
// shape radiates. threshold > 0 keys the emission on brightness. `tint` is the authored colour
// ((fill/255)·fillIntensity per channel); the rgb sum is deliberately unclamped (float16 headroom to the
// blit), so a hot aura keeps its energy. intensity 0 is a byte-exact identity, and radius ≤ 0 is too (the
// mask is gated to zero — no reach, no aura, whatever the intensity). The CPU mirror of the pieces is
// retropp::glowMask / gaussianKernelWeight / applyGlowAdd, composed the same way by the tests' 2-D oracle.
//
// Tap offsets, uSnap, and the border clamp behave exactly as in bloom.frag.
//
// SDL_GPU HLSL conventions: the fragment's sampled texture + sampler in space2, the uniform buffer in
// space3.

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

cbuffer GlowUniforms : register(b0, space3) {
    float uRadius;        // aura reach, viewport px (σ and K derive from it); ≤ 0 = identity
    float uTaps;          // K, per side (min(⌈radius⌉, 32), resolved on the CPU)
    float uInvNorm;       // 1 / Σ w(k) over k ∈ [−K, K] — the per-axis kernel normalization
    float uThreshold;     // emission floor, normalized 0..1                                   — register 0
    float uInvViewportW;  // 1 / viewport width
    float uInvViewportH;  // 1 / viewport height
    float uSnap;          // 1 = evaluate from the viewport-cell centre (crisp); 0 = per output pixel
    float uIntensity;     // aura strength, normalized 0..1; 0 = identity                     — register 1
    float uTintR;         // authored tint (fill.r/255 · fillIntensity)
    float uTintG;
    float uTintB;
    float _pad0;          //                                                                   — register 2
};

// The scalar emission mask at a PREMULTIPLIED source pixel — mirrors retropp::glowMask.
float glowMask(float4 s, float threshold) {
    if (s.a <= 0.0f) return 0.0f;
    if (threshold <= 0.0f) return s.a;  // whole-coverage emission — every covered pixel emits fully
    float3 straight = s.rgb / s.a;
    float  lum      = straight.r * 0.299f + straight.g * 0.587f + straight.b * 0.114f;
    float  den      = max(1.0f - threshold, 1.0f / 255.0f);
    return s.a * saturate((lum - threshold) / den);
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 src = SourceTexture.Sample(SourceSampler, uv);
    if (uRadius <= 0.0f) return src;  // no reach — byte-exact identity at any intensity

    float2 base = uv;
    if (uSnap != 0.0f) {
        float vpW = uInvViewportW > 0.0f ? 1.0f / uInvViewportW : 0.0f;
        float vpH = uInvViewportH > 0.0f ? 1.0f / uInvViewportH : 0.0f;
        if (vpW > 0.0f) base.x = (floor(uv.x * vpW) + 0.5f) / vpW;
        if (vpH > 0.0f) base.y = (floor(uv.y * vpH) + 0.5f) / vpH;
    }

    int   K      = (int)uTaps;
    float sigma  = max(uRadius, 0.5f) * 0.5f;
    float inv2s2 = 1.0f / (2.0f * sigma * sigma);

    float m = 0.0f;
    [loop]
    for (int dy = -K; dy <= K; dy++) {
        float wy = exp(-((float)(dy * dy)) * inv2s2);
        [loop]
        for (int dx = -K; dx <= K; dx++) {
            float  w = wy * exp(-((float)(dx * dx)) * inv2s2);
            float4 s = SourceTexture.Sample(SourceSampler,
                                            float2(base.x + (float)dx * uInvViewportW,
                                                   base.y + (float)dy * uInvViewportH));
            m += w * glowMask(s, uThreshold);
        }
    }
    m *= uInvNorm * uInvNorm;

    float  lift = uIntensity * m;
    float3 rgb  = src.rgb + lift * float3(uTintR, uTintG, uTintB);
    float  a    = saturate(src.a + lift);
    return float4(rgb, a);
}
