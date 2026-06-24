// Standard preamble PREPENDED to every game-authored custom post-process fragment. It declares
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

// Engine-filled (b0): values the engine controls for every effect. uEdgeClamp is the edge mode for
// sampleSource (0 = Blank, the default; 1 = Stretch/clamp). uRowTableY / uRowTableRows locate this
// effect's per-row data table in the shared data store (see RowDataTexture below); uRowTableRows == 0
// means the effect carries no table. One reserved lane remains.
cbuffer RetroppEngineEffect : register(b0, space3) {
    uint uEdgeClamp;
    uint uRowTableY;
    uint uRowTableRows;
    uint uEnginePad;
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

// A read-only per-frame data table the effect samples by row — an arbitrary float4 array the game fills
// each frame (a per-scanline value, or a per-region value indexed by id). Bound for the custom path; an
// effect with no table forwards uRowTableRows == 0 and the helpers return 0 (no contribution). Storage
// texture (integer Load, no sampler) — the same delivery as the tile / atlas / palette stores. It is the
// fragment stage's storage texture, so it follows the sampled SourceTexture (t0) at the next slot, t1,
// in the shared fragment texture space (space2) — the SDL_GPU convention: sampled textures first, then
// storage textures, numbered sequentially.
Texture2D<float4> RowDataTexture : register(t1, space2);

// Row i of this effect's table (i in [0, uRowTableRows)). Returns 0 when the effect has no table.
float4 paramRow(uint i) {
    if (uRowTableRows == 0u) return float4(0, 0, 0, 0);
    return RowDataTexture.Load(int3(0, uRowTableY + i, 0));
}

// The table row under this fragment's scanline — maps uv.y in [0,1) to a row in [0, uRowTableRows). The
// per-scanline case (a per-line scroll / warp / colour). Returns 0 when the effect has no table.
float4 paramRowAtUv(float2 uv) {
    if (uRowTableRows == 0u) return float4(0, 0, 0, 0);
    uint i = min((uint)(uv.y * (float)uRowTableRows), uRowTableRows - 1u);
    return paramRow(i);
}
