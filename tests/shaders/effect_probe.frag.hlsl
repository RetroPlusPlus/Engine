// Device-free reflection/packing probe (ENG-2.I.b — custom_stage_test). Declares a KNOWN cbuffer so the
// build reflects it and generates pack_effect_probe_frag + surfaces `.offset`/`.strength` on
// ScreenSpaceEffect; the test asserts the packed bytes + offsets. Self-contained to the test target
// (retropp_autocompile_shaders scans this path literal in custom_stage_test.cpp), so the packing coverage
// never depends on whether the example demos are built. The engine injects the plumbing (retropp_effect.hlsli);
// a custom shader's own params live at b1/space3.

cbuffer Params : register(b1, space3) {
    float2 offset;   // @0  (8 bytes)
    float  strength; // @8  (4 bytes)  → cbuffer rounds to 16
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv + offset * strength);
}
