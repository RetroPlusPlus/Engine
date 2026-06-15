// Row-displacement post-process fragment shader (ENG-2.C.2.a).
//
// The first engine-provided screen-space-effect stage: sample the source (the composited viewport,
// or a prior stage's output) at a DISPLACED UV so the whole image waves — the faithful modern
// expression of an effect a GB achieved by rewriting SCX every scanline (wavy water / heat haze /
// per-line SCX). In screen space the fragment's row coordinate IS the scanline, so the effect is a
// per-pixel function f(row, phase): no reconstructed LY counter, no HBlank ISR. The game advances
// `uPhase` per frame to animate.
//
//   Horizontal: srcU = uv.x + (amplitude/viewportW)·sin(2π·(frequency·uv.y + phase));  srcV = uv.y
//   Vertical:   srcV = uv.y + (amplitude/viewportH)·sin(2π·(frequency·uv.x + phase));  srcU = uv.x
//
// This is the byte-for-byte mirror of retropp::displaceSourceUv (postprocess.h) — the unit-tested CPU
// side. `amplitude` is in viewport pixels (normalized here by the inverse viewport dimension), so the
// effect is resolution-independent and the downstream blit scales the result with no change.
//
// BOUNDARY: a displaced UV outside [0,1] samples NOTHING. What "nothing" is depends on the scope of
// the effect, carried by uBlankIsTransparent:
//   0 (frame-level postEffects, and per-layer Below) — the opaque-black BACKDROP (matching the
//     viewport pass's clear). A whole-frame / whole-accumulator displacement has no off-screen
//     content to reveal, so the exposed strip is left blank-black.
//   1 (per-layer Layer, the ISOLATED scope — ENG-2.C.2.b) — fully TRANSPARENT (premultiplied 0), so
//     the exposed strip reveals the layers composited below this one rather than punching a black bar
//     through them.
// Either way the strip is BLANK (not a stretched edge duplicate) under the Blank edge; Stretch falls
// through to the CLAMP_TO_EDGE sampler. Supersedes the PLAN's CLAMP_TO_EDGE-only choice per dev
// verification (C.2.a Amendment A2; C.2.b adds the transparent variant for the isolated scope).
//
// Authored to SDL_GPU's HLSL conventions: a fragment shader's sampled texture + sampler live in
// register space2; its uniform buffer in space3.

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

cbuffer DisplaceUniforms : register(b0, space3) {
    float uAmplitude;     // displacement magnitude, viewport pixels
    float uFrequency;     // cycles across the modulated axis
    float uPhase;         // animation phase (game-advanced)
    uint  uAxis;          // 0 = Horizontal, 1 = Vertical   — register 0
    float uInvViewportW;       // 1 / viewport width
    float uInvViewportH;       // 1 / viewport height
    uint  uEdge;               // 0 = Blank, 1 = Stretch
    uint  uBlankIsTransparent; // 0 = opaque-black backdrop (frame-level / Below), 1 = transparent (Layer)
};                             // register 1

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    const float kTwoPi = 6.283185307179586f;
    float2 src = uv;
    if (uAxis == 0u) {  // Horizontal
        float s = sin(kTwoPi * (uFrequency * uv.y + uPhase));
        src.x = uv.x + uAmplitude * uInvViewportW * s;
    } else {            // Vertical
        float s = sin(kTwoPi * (uFrequency * uv.x + uPhase));
        src.y = uv.y + uAmplitude * uInvViewportH * s;
    }
    // Blank edge (default): an out-of-source UV samples nothing → opaque-black backdrop (frame-level
    // / Below) or fully transparent (Layer, so the strip reveals the layers below). Stretch edge:
    // fall through and let the CLAMP_TO_EDGE sampler duplicate the edge column. In-bounds always
    // samples, whatever the edge.
    if (uEdge == 0u && (src.x < 0.0f || src.x > 1.0f || src.y < 0.0f || src.y > 1.0f)) {
        return uBlankIsTransparent != 0u ? float4(0.0f, 0.0f, 0.0f, 0.0f)
                                         : float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    return SourceTexture.Sample(SourceSampler, src);
}
