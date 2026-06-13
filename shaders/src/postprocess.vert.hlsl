// Fullscreen-triangle post-process vertex shader.
//
// Shared by every post-process stage (the displacement stage now; future engine stages and the
// custom shader hook later). Emits a single triangle covering the whole render target from
// SV_VertexID alone — no vertex buffer is bound — and hands the interpolated UV to the fragment
// stage, which samples the source (the composited viewport, or a prior stage's output). Three
// vertices with UVs (0,0), (2,0), (0,2) expand to a triangle whose inscribed [0,1]^2 region is the
// visible quad; the clip-space mapping flips V so UV (0,0) lands at the top-left, matching SDL_GPU's
// top-left texture-coordinate origin.
//
// Kept separate from blit.vert.hlsl (identical body) so the post-process feature is purely additive
// and the working blit pipeline's shaders are untouched.
//
// Authored to SDL_GPU's HLSL conventions: vertex semantics start at TEXCOORD0; system-value outputs
// use SV_*.

struct Output {
    float2 uv  : TEXCOORD0;
    float4 pos : SV_Position;
};

Output main(uint vertexID : SV_VertexID) {
    Output output;
    output.uv  = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
