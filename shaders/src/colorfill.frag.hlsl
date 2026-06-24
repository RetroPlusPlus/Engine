// ColorFill post-process fragment — a BUILT-IN engine effect stage. Paints a colour onto the pixels it
// covers: out.rgb = fill — a solid fill; the layer alpha sets opacity. A Region confines it
// to a shape, so the colour FILLS that shape (a stroked region → a colored line/path; a filled region → a
// solid shape). The unit-tested CPU mirror is retropp::applyColorFill (postprocess.h). Engine stage
// contract (identical to displace/ripple): one sampled source in space2 (t0/s0), one uniform cbuffer in
// space3 (b0); shared postprocess.vert.
Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);
cbuffer ColorFillUniforms : register(b0, space3) {
    float3 uFill; float uPad;  // register 0 — fill colour (rgb), normalized
};
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 c = SourceTexture.Sample(SourceSampler, uv);
    c.rgb = uFill;
    return c;  // the pixel's own alpha is kept (a transparent area stays transparent under Layer scope)
}
