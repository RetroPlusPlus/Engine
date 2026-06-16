// Mirror-ghost — a deliberately USELESS custom effect (ENG-2.I.b demo). A faint, point-mirrored copy of the
// whole frame is cross-faded over the real one; the game slowly orbits the pivot and oscillates the blend. A
// pointless doppelgaenger wandering on top of your game — exactly the long tail the custom hook is for. The
// engine injects the plumbing (retropp_effect.hlsli); this file declares its OWN params + body. It samples
// through sampleSource(), so where the mirror falls off-frame it is BLANK by default (the effect's edge
// setting decides — not this shader). Slow orbit, low blend — no strobing (photosensitivity). The game sets
// the params inline:
//   ScreenSpaceEffect{ .kind = ScreenSpaceEffectKind::Custom, .customShader = ghost, .pivot = ..., .blend = ... }

cbuffer Params : register(b1, space3) {
    float2 pivot;   // the doppelgaenger's mirror point, in UV [0,1]
    float  blend;   // cross-fade amount [0,1]
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    const float4 real  = sampleSource(uv);
    const float4 ghost = sampleSource(2.0f * pivot - uv);  // point reflection through pivot; off-frame ⇒ blank
    return lerp(real, ghost, saturate(blend));
}
