// Tile layer vertex shader.
//
// Emits a single fullscreen triangle from SV_VertexID alone — no vertex buffer is bound.
// The triangle's inscribed [0,1]^2 region covers the render target (the offscreen internal
// viewport); the interpolated UV is handed to the fragment stage, which turns it into a
// layer-local pixel and samples the tilemap + atlas. The clip-space mapping flips V so UV
// (0,0) lands at the top-left of the target (SDL_GPU's top-left texture origin), matching
// the blit shader. A tile layer covers the whole viewport, so no per-layer
// destination transform is needed here; the fragment stage applies scroll and wrap.
//
// Authored to SDL_GPU's HLSL conventions (see SDL_CreateGPUShader docs): vertex semantics
// start at TEXCOORD0; system-value outputs use SV_*.

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
