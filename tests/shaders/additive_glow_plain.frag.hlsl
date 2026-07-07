// Region-batching equivalence-test shader (PLAIN twin — the per-region reference). Byte-for-byte identical
// to additive_glow.frag.hlsl EXCEPT it carries NO `// retropp: additive` declaration, so no batched variant
// is compiled and every region takes the existing per-region path (runEffect + region-select gate). The GPU
// equivalence test renders this against the declared twin at each confined site; the two must match within
// Tol::OneStep (float-rounding / additive-order only). Keep the body in lockstep with the declared twin.

cbuffer Params : register(b1, space3) {
    float addGain;   // additive peak at the region centre
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 c = sampleSource(uv);
    const float2 d    = uv - 0.5f;
    const float  r    = saturate(length(d) * 2.0f);   // 0 at the (frame-global) centre → 1 at the edge
    const float  fall = (1.0f - r) * (1.0f - r);       // soft shoulders, bright core
    const float3 tint = float3(0.9f, 0.6f, 0.3f);
    c.rgb += tint * (addGain * fall);                  // additive — brightens whatever's behind
    return c;
}
