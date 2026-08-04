// Emission-consumer test shader (NO emission() body) — the stock-default sibling. The `@retropp:emission`
// declaration still makes the stage an emission consumer, but with no emission() body the demand extracts
// through the stock brightpass at the effect's `.threshold`, so only bright content emits (the luminance
// key). The device test checks that a DARK covered region produces NO glow here, where emission_probe (which
// authors its field by coverage) does — proving the null-extract-variant default and the two extract paths.
//
// @retropp:emission
// @retropp:no-sprite

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv) + sampleEmission(uv);
}
