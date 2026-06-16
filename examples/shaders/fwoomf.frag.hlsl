// Fwoomf — a deliberately USELESS custom effect (ENG-2.I.b demo). The frame slowly, partially swaps its X
// and Y by `amount` (the game oscillates it gently) and slides back, so everything smears toward its
// across-the-diagonal mirror and returns. Useless and faintly nauseating — the long tail the custom hook is
// for. The engine injects the plumbing (retropp_effect.hlsli); this file declares its OWN param + body. It
// samples through sampleSource(), so where the swap pushes the sample off the (non-square) frame it is BLANK
// by default (the effect's edge setting decides — not this shader). Slow oscillation — no strobing
// (photosensitivity). The game sets the param inline:
//   ScreenSpaceEffect{ .kind = ScreenSpaceEffectKind::Custom, .customShader = fwoomf, .amount = ... }

cbuffer Params : register(b1, space3) {
    float amount;   // 0 = identity, 1 = full diagonal swap
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(lerp(uv, uv.yx, saturate(amount)));
}
