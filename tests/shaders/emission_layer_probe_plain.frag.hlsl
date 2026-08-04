// The Layer-scope baseline sibling: a plain (non-emission) Custom chain step that returns the sprite's art
// unchanged. It runs the identical inline custom dispatch as emission_layer_probe but adds no sampleEmission
// halo, so the difference between the two scenes is exactly the emission field the declared stage reads back.

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv);   // the art, no halo — the control
}
