// Blend composite post-process fragment shader.
//
// Composites a container's isolated render `src` over what it sits on, `dst` (the accumulator), under a
// BlendMode. The programmable peer of the fixed-function premultiplied-over composite: where the over
// path can only alpha-blend, this evaluates the separable blend operator the container selects (Add /
// Subtract / Multiply / Screen / Half, plus Normal). It samples both operands, writes the full blended
// RGBA (the pass REPLACES its target — the caller swaps it into the accumulator).
//
// `src` arrives PREMULTIPLIED: the container renders alone into a transparent scratch, so its rgb holds
// colour·alpha and its alpha holds the coverage. The operator B(dst, colour) takes the STRAIGHT source
// colour, so the shader un-premultiplies (rgb / a) before evaluating it, then applies alpha once as the
// weight. The result matches retropp::applyBlendMode(dst, {colour, a}, mode) — the CPU authority over
// straight-colour operands (its region-effect callers hand it straight colours).
//
// SDL_GPU HLSL conventions: fragment sampled textures + samplers in space2 (t0/s0 = dst accumulator,
// t1/s1 = src container render); the uniform buffer in space3.

Texture2D<float4> DstTexture : register(t0, space2);
SamplerState      DstSampler : register(s0, space2);
Texture2D<float4> SrcTexture : register(t1, space2);
SamplerState      SrcSampler : register(s1, space2);

cbuffer BlendUniforms : register(b0, space3) {
    float4 uBlend;  // x = blend mode (BlendMode as float, rounded to uint); yzw unused
};

// The separable blend operator B(d, s) per BlendMode (mirror of retropp::blendChannel). Normal returns s.
float3 blendOp(uint mode, float3 d, float3 s) {
    if (mode == 1u) return d + s;                        // Add
    if (mode == 2u) return d - s;                        // Subtract
    if (mode == 3u) return d * s;                        // Multiply
    if (mode == 4u) return 1.0 - (1.0 - d) * (1.0 - s);  // Screen
    if (mode == 5u) return (d + s) * 0.5;                // Half
    return s;                                            // Normal
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 dst = DstTexture.Sample(DstSampler, uv);
    float4 src = SrcTexture.Sample(SrcSampler, uv);

    uint  mode = (uint)(uBlend.x + 0.5);
    float sa   = src.a;
    // src is premultiplied — recover the straight source colour for the operator (sa == 0 ⇒ no coverage).
    float3 sc  = sa > 0.0 ? src.rgb / sa : float3(0.0, 0.0, 0.0);
    // out.rgb = (1 - srcA)·dst + srcA·B(dst, colour); out.a = srcA + dstA·(1 - srcA) — the applyBlendMode
    // math. At sa == 1, sc == src.rgb, so full-coverage composites are byte-identical to the premultiplied
    // form; Normal (B returns colour) reduces to the exact premultiplied-over equation.
    float3 rgb = saturate((1.0 - sa) * dst.rgb + sa * blendOp(mode, dst.rgb, sc));
    float  a   = saturate(sa + dst.a * (1.0 - sa));
    return float4(rgb, a);
}
