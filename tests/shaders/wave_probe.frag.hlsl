// Crisp-parity probe (golden_readback_test). Exercises BOTH halves of the custom-shader evaluation-grid
// contract in one unmodified shader: `bands` drives procedural spatial math on the uv main() receives
// (per-cell on the Viewport grid), and `wobble` drives a spatially-varying sampleSource() displacement
// (quantized to whole viewport pixels on the Viewport grid). The parity scenes assert a scale-3 capture
// byte-equals the scale-1 capture nearest-upscaled — with this file exactly as a game would write it.
// The engine injects the plumbing (retropp_effect.hlsli); params live at b1/space3.

cbuffer Params : register(b1, space3) {
    float wobble;   // horizontal displacement amplitude, in UV units (0 = sample straight through)
    float bands;    // procedural tint band count down the frame
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    const float4 src  = sampleSource(uv + float2(sin(uv.y * 41.0f) * wobble, 0.0f));
    const float  tint = 0.7f + 0.3f * sin(uv.y * bands * 6.2831853f);
    return float4(src.rgb * tint, src.a);
}
