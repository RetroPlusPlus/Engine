// Standard preamble PREPENDED to every game-authored custom post-process fragment (ENG-2.I.b). It declares
// the engine plumbing — the composited source frame + the engine-controlled edge mode. A custom shader
// declares its OWN parameter cbuffer (its own named fields, at b1/space3) and writes
//   float4 main(float2 uv : TEXCOORD0) : SV_Target0 { ... }
// using sampleSource() + its own params. The game sets those params as inline named fields on the
// ScreenSpaceEffect (.kind = Custom, .customShader = <path handle>, .<param> = ...), exactly like a
// built-in effect; the build reads the shader's cbuffer and surfaces those names. Low-friction by design:
// drop in a .hlsl with a cbuffer + body, reference its path, set params inline — nothing else.
//
//   sampleSource(uv)  — THE sample function. Inside [0,1] it samples the composited frame (or the prior
//                       chain pass). OUTSIDE [0,1] its behaviour is the EFFECT's edge setting, NOT the
//                       shader's choice: ScreenSpaceEffect::edge == Blank (the default) returns BLANK
//                       (transparent — the backdrop / layers below reveal through, never a smeared edge);
//                       edge == Stretch clamps (smears the border). A layer that doesn't want clamping
//                       never gets it — the same edge rule the engine's built-in effects obey. Custom
//                       shaders should ALWAYS sample through this, not SourceTexture directly.

// Engine-filled (b0): the edge mode for sampleSource, set from the effect's `edge` field (0 = Blank, the
// faithful default; 1 = Stretch/clamp). Reserved for future engine-provided values.
cbuffer RetroppEngineEffect : register(b0, space3) {
    uint uEdgeClamp;
    uint uEnginePad0;
    uint uEnginePad1;
    uint uEnginePad2;
};

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

// Sample the composited source with the EFFECT's edge policy. Blank (default) → transparent outside the
// frame; Stretch (uEdgeClamp == 1) → clamp (CLAMP_TO_EDGE smears the border). The shader never decides
// this — the layer/effect does.
float4 sampleSource(float2 uv) {
    if (uEdgeClamp == 0u && (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return SourceTexture.Sample(SourceSampler, uv);  // in-bounds, OR Stretch → CLAMP_TO_EDGE
}
