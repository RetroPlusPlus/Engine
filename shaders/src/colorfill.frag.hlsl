// ColorFill post-process fragment — a BUILT-IN engine effect stage. Paints a colour onto the pixels it
// covers: the shaped, region-confinable sibling of the whole-frame blit colour transform. out =
// clamp(in*mul + add) then mix(in, fill, fillStrength) — a Region confines it to a shape, so a colour
// FILLS that shape (a stroked region → a colored line/path; a filled region → a solid shape; mul/add →
// a shaped grade/tint). Byte-for-byte the blit's colour math; the unit-tested CPU mirror is
// retropp::applyColorFill (postprocess.h). Engine stage contract (identical to displace/ripple): one
// sampled source in space2 (t0/s0), one uniform cbuffer in space3 (b0); shared postprocess.vert.
Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);
cbuffer ColorFillUniforms : register(b0, space3) {
    float3 uMul; float uPad0;          // register 0 — multiply (identity 1)
    float3 uAdd; float uPad1;          // register 1 — add      (identity 0)
    float3 uFill; float uFillStrength; // register 2 — blend-to-colour + strength (identity s=0)
};
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 c = SourceTexture.Sample(SourceSampler, uv);
    c.rgb = clamp(c.rgb * uMul + uAdd, 0.0, 1.0);
    c.rgb = lerp(c.rgb, uFill, uFillStrength);
    return c;  // alpha untouched
}
