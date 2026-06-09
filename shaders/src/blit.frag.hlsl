// Blit fragment shader.
//
// Samples the source (internal viewport) texture at the interpolated UV and writes
// it straight to the swapchain. No colour transform — the faithful baseline blit;
// scaling/sampling enhancements live later in the composition chain (ENG-2.C), and
// the sampler's nearest filtering is set on the engine side at sampler creation.
//
// Authored to SDL_GPU's HLSL conventions (see SDL_CreateGPUShader docs): a fragment
// shader's sampled texture and its sampler live in register space2.

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return SourceTexture.Sample(SourceSampler, uv);
}
