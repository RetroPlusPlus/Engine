// Layer-scope emission-consumer test shader. As a Layer-scope Custom chain step it runs inline over the
// sprite's own art (sampleSource) and reads its own blurred silhouette back through sampleEmission(). On the
// Layer path the field's content is the sprite's art brightpass at the effect's threshold — the art path has
// no composited scene to hand an emission() body, so this shader declares none; main() adds that blurred halo
// over the art, so the glow spreads by `.radius` and brightens the silhouette from within.
//
// @retropp:emission

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv) + sampleEmission(uv);   // the sprite's art plus its own blurred halo
}
