// Emission composite — the final stage of the Bloom / Glow chain.
//
// Reads the untouched source (t0) and the blurred emission (t1) and adds the halo back over the source:
//
//   out.rgb = src.rgb + e.rgb                        // additive light, no clamp
//   Bloom:  out.a = saturate(src.a + e.a · (1 − src.a))   // the halo lifts coverage; opaque stays opaque
//   Glow:   out.a = saturate(src.a + e.a)                 // the aura lifts coverage
//
// The developer's intensity, threshold and tint are already folded into `e` by the extract stage, so this
// stage carries no strength of its own — it is exactly retropp::applyBloomAdd / applyGlowAdd with the
// intensity already applied to the emission. The rgb sum is deliberately unclamped: the float16 offscreen
// chain carries values above 1 to the final blit, so a hot halo keeps its energy through downstream blends.
//
// The two alpha rules are the one place the kinds differ. Bloom's halo is the source's own light spilling
// outward, so it fills only what the source does not already cover — the (1 − src.a) factor. Glow's aura is
// authored emission laid over the scene, so it lifts coverage directly.
//
// A zero emission (intensity 0) reduces both rules to the source unchanged, which is how the chain keeps
// its byte-exact identity. When the blur ran at quarter resolution the emission sampler is bilinear, so the
// low-resolution halo resolves smoothly back to full resolution; a full-resolution blur binds it nearest.
//
// SDL_GPU HLSL conventions: fragment sampled textures + samplers in space2 (t0/s0 = source,
// t1/s1 = emission); the uniform buffer in space3.

Texture2D<float4> SourceTexture   : register(t0, space2);
SamplerState      SourceSampler   : register(s0, space2);
Texture2D<float4> EmissionTexture : register(t1, space2);
SamplerState      EmissionSampler : register(s1, space2);

cbuffer EmissionCompositeUniforms : register(b0, space3) {
    float uGlow;   // 0 = Bloom (coverage-limited alpha lift); 1 = Glow (direct alpha lift)
    float _pad0;
    float _pad1;
    float _pad2;
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 src = SourceTexture.Sample(SourceSampler, uv);
    float4 e   = EmissionTexture.Sample(EmissionSampler, uv);

    float3 rgb = src.rgb + e.rgb;
    float  a   = uGlow != 0.0f ? saturate(src.a + e.a)
                               : saturate(src.a + e.a * (1.0f - src.a));
    return float4(rgb, a);
}
