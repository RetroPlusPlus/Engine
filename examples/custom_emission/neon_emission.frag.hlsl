// custom_emission demo — the AUTHORED-emission shader (an emission() body).
//
// The `// @retropp:emission` declaration line makes this stage an emission consumer: the engine extracts a
// field for it, blurs the field by the effect's `.radius`, and hands it back through `sampleEmission(uv)`.
// The `emission()` body AUTHORS what goes into that field. Here it emits a blue-dominant MASK — the deep-blue
// neon of the scene, and nothing else. The stock brightpass keys on Rec.601 luminance (where blue weighs
// least), so a vivid but dark-blue neon never blooms under it; an authored body is the only way to single that
// signal out. That is the write half's whole reason to exist: emit a signal the stock brightpass cannot.
// main() adds the blurred field back over the source, so the neon spreads a glow that widens with `.radius`.
//
// @retropp:emission

// The field content: how blue-dominant a pixel is. Deep-blue neon emits at full strength; the warm lamps
// (whose blue is low) emit nothing, even though they are far brighter. This is the "emit from a mask channel"
// case the luminance brightpass cannot reproduce.
float4 emission(float2 uv) {
    float3 c = sampleSource(uv).rgb;
    float  e = saturate(c.b - max(c.r, c.g));   // blue in excess of the warm channels — the neon mask
    return float4(e, e, e, e);
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv) + sampleEmission(uv);   // the frame plus its authored neon glow
}
