// A custom effect that runs INLINE on a sprite (Layer scope). sampleSource(uv) reads the sprite's own art;
// this REMAPS it toward an electric energy look keyed off the art's own luminance — the bright body blazes
// blue-white, the dark rim stays dark — a nonlinear per-pixel recolor no built-in kind does. `charge` ramps
// it in (0 = the art unchanged, 1 = fully energized); the game advances it off the tick. Float-only params,
// so the shader runs on the sprite path.

cbuffer Params : register(b1, space3) {
    float charge;   // 0 = the art unchanged, 1 = fully energized
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 c   = sampleSource(uv);
    float  lum = dot(c.rgb, float3(0.299f, 0.587f, 0.114f));
    // A cool base that scales with luminance, plus a white-hot core on the brightest pixels.
    float3 energy = float3(0.10f, 0.70f, 1.05f) * lum + pow(lum, 3.0f) * float3(0.9f, 1.0f, 1.0f);
    return float4(saturate(lerp(c.rgb, energy, charge)), c.a);
}
