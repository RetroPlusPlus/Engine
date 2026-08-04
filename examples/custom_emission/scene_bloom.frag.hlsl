// custom_emission demo — the DECLARATION-ONLY shader (no emission() body).
//
// The `// @retropp:emission` line alone is the entire opt-in. With no `emission()` body beside main(), the
// engine fills the field with the STOCK brightpass at the effect's `.threshold` (threshold 0 = the whole
// content emits; higher keys on brightness), blurs it by `.radius`, and hands it back through
// `sampleEmission(uv)`. This is the one-line cheap case — a bloom of the source's own bright light with no
// shader math of its own — and it is exactly what a built-in `Bloom` does, obtained here by declaration.
// Blur is obtained by declaring the stage an emission consumer, never computed in the body.
//
// @retropp:emission

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv) + sampleEmission(uv);   // the source plus its own bloomed light
}
