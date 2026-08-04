// Below-scope emission-consumer test shader (WITH an emission() body). As a Below-scope lens it reads the
// scene beneath through sampleSource() and adds its own halo through sampleEmission(); the emission() body
// AUTHORS that halo from the scene's BLUE channel — a marker the stock brightpass (Rec.601 luminance, where
// blue weighs least) gates out for a vivid-but-low-luminance blue. The reach rides the effect's `.radius`.
//
// @retropp:emission

// The field content: the scene's blue channel, promoted to a scalar. On the device scene the source beneath
// the lens is a vivid low-luminance blue, so this emits strongly where the luminance brightpass gates out.
float4 emission(float2 uv) {
    float e = sampleSource(uv).b;
    return float4(e, e, e, e);
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv) + sampleEmission(uv);   // the scene beneath plus its marker halo
}
