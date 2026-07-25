// Bloom pass B — vertical blur of the pass-A result, added over the untouched source (the second half
// of the two-pass glow).
//
//   glow   = Σₖ w(k) · scratch(uv + k px) · uInvNorm,   k ∈ [−K, K]      // scratch = bloom_h's output
//   out.rgb = src.rgb + intensity · glow.rgb                              // additive light, no clamp
//   out.a   = src.a  + intensity · glow.a · (1 − src.a)                   // the halo lifts coverage
//
// w(k) and K are the same kernel as pass A (σ = max(radius, 0.5)/2, K = min(⌈radius⌉, 32)); uInvNorm is
// the same per-axis normalization, so the two passes together apply the separable 2-D Gaussian. The rgb
// sum is deliberately unclamped — the float16 offscreen chain carries values above 1 to the final blit,
// so a hot halo keeps its energy through downstream blends. intensity 0 is a byte-exact identity (the
// source passes through untouched). The CPU mirror of the composite is retropp::applyBloomAdd.
//
// Tap offsets, uSnap, and the border clamp behave exactly as in bloom_h, on the vertical axis.
//
// SDL_GPU HLSL conventions: fragment sampled textures + samplers in space2 (t0/s0 = the untouched
// source, t1/s1 = the pass-A scratch); the uniform buffer in space3.

Texture2D<float4> SourceTexture  : register(t0, space2);
SamplerState      SourceSampler  : register(s0, space2);
Texture2D<float4> ScratchTexture : register(t1, space2);
SamplerState      ScratchSampler : register(s1, space2);

cbuffer BloomVUniforms : register(b0, space3) {
    float uRadius;        // blur reach, viewport px (σ and K derive from it)
    float uTaps;          // K, per side (min(⌈radius⌉, 32), resolved on the CPU)
    float uInvNorm;       // 1 / Σ w(k) over k ∈ [−K, K] — the per-axis kernel normalization
    float uIntensity;     // glow strength, normalized 0..1; 0 = identity                      — register 0
    float uInvViewportW;  // 1 / viewport width
    float uInvViewportH;  // 1 / viewport height
    float uSnap;          // 1 = evaluate from the viewport-cell centre (crisp); 0 = per output pixel
    float _pad0;          //                                                                   — register 1
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

    float4 glow = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [loop]
    for (int k = -K; k <= K; k++) {
        float w = exp(-((float)(k * k)) * inv2s2);
        glow += w * ScratchTexture.Sample(ScratchSampler, float2(base.x, base.y + (float)k * uInvViewportH));
    }
    glow *= uInvNorm;

    float3 rgb = src.rgb + uIntensity * glow.rgb;
    float  a   = saturate(src.a + uIntensity * glow.a * (1.0f - src.a));
    return float4(rgb, a);
}
