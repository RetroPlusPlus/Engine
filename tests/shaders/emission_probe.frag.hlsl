// Emission-consumer test shader (WITH an emission() body). The `@retropp:emission` declaration makes the
// stage an emission consumer: the engine extracts + blurs a field for it and hands it back through
// sampleEmission(). The emission() body AUTHORS that field from the source's BLUE channel — a marker the
// stock brightpass (which keys on Rec.601 luminance, where blue weighs least) cannot produce for a vivid but
// low-luminance blue region. That is exactly the write half's reason to exist: emit a signal the stock
// brightpass cannot. main() adds the blurred field back over the source: a glow that spreads by `.radius`.
//
// no-sprite because the sprite emission paths have their own probes; this shader targets
// the frame-class site alone, so it declares itself off the sprite path to keep its variants frame-class.
//
// @retropp:emission
// @retropp:no-sprite

// The field content: the blue channel, promoted to a scalar. On the device scene the emitter is a vivid but
// low-luminance blue on a black backdrop, so this emits only over the emitter — a channel-marker emission the
// luminance brightpass gates out.
float4 emission(float2 uv) {
    float e = sampleSource(uv).b;
    return float4(e, e, e, e);
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv) + sampleEmission(uv);   // additive marker glow
}
