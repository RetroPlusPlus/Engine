// Bloom — the threshold-blur-add glow, one pass. For each pixel, gather the 2-D Gaussian neighbourhood of
// the source, weighting each tap by the kernel and by its own brightness above the threshold, then add the
// glow back over the untouched source:
//
//   f(c)    = saturate((lum(c.rgb) − threshold) / max(1 − threshold, 1/255))   // Rec. 601 luminance
//   glow    = Σ w(dx)·w(dy) · sample(uv + (dx,dy) px) · f(sample) · uInvNorm²,  (dx,dy) ∈ [−K, K]²
//   w(k)    = exp(−k² / (2σ²)),  σ = max(radius, 0.5) / 2,  K = min(⌈radius⌉, 32)
//   out.rgb = src.rgb + intensity · glow.rgb                                   // additive light, no clamp
//   out.a   = saturate(src.a + intensity · glow.a · (1 − src.a))               // the halo lifts coverage
//
// The brightpass scales the full rgba by f in the pipeline's own (premultiplied) colour domain, so the
// glow's coverage rides the alpha. The rgb sum is deliberately unclamped — the float16 offscreen chain
// carries values above 1 to the final blit, so a hot halo keeps its energy through downstream blends.
// intensity 0 is a byte-exact identity (the source passes through untouched). uInvNorm is the per-axis
// kernel normalization (GaussianKernel::invNorm), applied squared for the two axes of the one gather —
// exactly the sprite fragment's art-kernel shape. The CPU mirror of the pieces is retropp::applyBrightpass /
// gaussianKernelWeight / applyBloomAdd, composed the same way by the tests' 2-D oracle.
//
// Tap offsets are whole viewport pixels (normalized by the inverse viewport dimensions), so the halo is
// resolution-independent. Under uSnap (the crisp Viewport grid) the taps walk from the fragment's
// viewport-cell centre, so every output pixel of one viewport cell sees the same glow; uSnap 0 (the Output
// grid) taps from the exact fragment position for a smooth halo. Off-frame taps clamp to the border texel
// (the pass sampler's CLAMP_TO_EDGE); there is no displaced strip, so no edge policy.
//
// SDL_GPU HLSL conventions: the fragment's sampled texture + sampler in space2, the uniform buffer in
// space3.

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

cbuffer BloomUniforms : register(b0, space3) {
    float uRadius;        // blur reach, viewport px (σ and K derive from it)
    float uTaps;          // K, per side (min(⌈radius⌉, 32), resolved on the CPU)
    float uInvNorm;       // 1 / Σ w(k) over k ∈ [−K, K] — the per-axis kernel normalization
    float uThreshold;     // luminance floor, normalized 0..1                                  — register 0
    float uInvViewportW;  // 1 / viewport width
    float uInvViewportH;  // 1 / viewport height
    float uSnap;          // 1 = evaluate from the viewport-cell centre (crisp); 0 = per output pixel
    float uIntensity;     // glow strength, normalized 0..1; 0 = identity                     — register 1
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 src = SourceTexture.Sample(SourceSampler, uv);

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
    float den    = max(1.0f - uThreshold, 1.0f / 255.0f);

    float4 glow = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [loop]
    for (int dy = -K; dy <= K; dy++) {
        float wy = exp(-((float)(dy * dy)) * inv2s2);
        [loop]
        for (int dx = -K; dx <= K; dx++) {
            float  w   = wy * exp(-((float)(dx * dx)) * inv2s2);
            float4 s   = SourceTexture.Sample(SourceSampler,
                                              float2(base.x + (float)dx * uInvViewportW,
                                                     base.y + (float)dy * uInvViewportH));
            float  lum = s.r * 0.299f + s.g * 0.587f + s.b * 0.114f;
            float  f   = saturate((lum - uThreshold) / den);
            glow += w * (s * f);
        }
    }
    glow *= uInvNorm * uInvNorm;

    float3 rgb = src.rgb + uIntensity * glow.rgb;
    float  a   = saturate(src.a + uIntensity * glow.a * (1.0f - src.a));
    return float4(rgb, a);
}
