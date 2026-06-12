// Blit fragment shader.
//
// Samples the source (internal viewport) texture at the interpolated UV, applies the
// frame-level post-composite colour transform (ENG-2.B.2.c.2) — a whole-frame
// ColorModifier (out = clamp(in*mul + add)) then a Blend flash (mix toward a colour by
// strength) — and writes the result to the swapchain. The transform is the modern
// realization of effects the ROM achieved through palette writes (whole-frame fade /
// day-night / cutscene flash), applied AFTER compositing on already-coloured pixels;
// it is NOT the colouring mechanism (that is index + palette on the tile/sprite paths).
// With the identity uniform (mul=1, add=0, strength=0) this is the faithful baseline blit
// value-for-value. Scaling/sampling enhancements live later in the chain (ENG-2.C); the
// sampler's nearest filtering is set engine-side at sampler creation.
//
// Authored to SDL_GPU's HLSL conventions (see SDL_CreateGPUShader docs): a fragment
// shader's sampled texture + sampler live in register space2; its uniform buffer in space3.

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

cbuffer BlitUniforms : register(b0, space3) {
    float3 uMul;   float uPad0;          // register 0 — ColorModifier multiply (identity 1,1,1)
    float3 uAdd;   float uPad1;          // register 1 — ColorModifier add      (identity 0,0,0)
    float3 uFlash; float uFlashStrength; // register 2 — Blend flash colour + strength (identity s=0)
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 c = SourceTexture.Sample(SourceSampler, uv);
    c.rgb = clamp(c.rgb * uMul + uAdd, 0.0, 1.0);  // ColorModifier
    c.rgb = lerp(c.rgb, uFlash, uFlashStrength);   // Blend flash
    return c;                                      // alpha untouched
}
