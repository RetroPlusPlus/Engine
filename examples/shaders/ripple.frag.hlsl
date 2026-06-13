// Radial ripple — a CONSUMER-authored custom shader stage (ENG-2.C.3 / Issue 5 demo).
//
// This fragment lives under examples/, NOT engine shaders/src/ — it models exactly what a CONSUMING
// GAME ships: its own HLSL, compiled to this platform's bytecode by the engine's build-time generator
// (the gbcpp_generate_shader CMake function), registered via Renderer::registerPostProcessStage, and
// driven per frame through a ScreenSpaceEffect{ .kind = Custom, .customShader = <handle>, .uniform = … }.
//
// It does something the engine's built-in RowDisplacement CANNOT: RowDisplacement is 1-D and axis-
// aligned, but this is a RADIAL, concentric ripple — a water droplet dropped into the centre of the
// screen, rings expanding outward. Each fragment's sample UV is displaced ALONG THE RADIUS from the
// centre by a sine of the distance (so crests form rings), with the crest travelling outward as the
// game advances `uPhase`, and a gentle distance decay so it fades with radius like a real droplet.
// Aspect-corrected (the viewport is 160×144, not square) so the rings stay circular.
//
// Engine custom-stage fragment CONTRACT (identical to the built-in displacement stage): one sampled
// source texture + sampler in space2 (t0/s0 — the composited viewport, or the prior chain pass), and
// one uniform cbuffer in space3 (b0) the game fills. The engine's shared postprocess.vert is the
// vertex stage; the game supplies only this fragment.

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

cbuffer RippleUniforms : register(b0, space3) {
    float uCenterX;       // ripple centre, UV
    float uCenterY;
    float uAmplitude;     // displacement magnitude, viewport pixels
    float uFrequency;     // ring count across the distance field   — register 0
    float uPhase;         // expansion phase (game-advanced, slow)
    float uInvViewportW;  // 1 / viewport width  (amplitude px→UV, x)
    float uInvViewportH;  // 1 / viewport height (amplitude px→UV, y; also the aspect via H/W)
    float uDecay;         // radial falloff rate                    — register 1
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    const float kTwoPi = 6.283185307179586f;

    const float2 center = float2(uCenterX, uCenterY);
    const float2 delta  = uv - center;

    // Aspect-correct the distance so the rings are circular in screen space (a 160-wide / 144-tall
    // viewport would otherwise make ellipses): scale x by aspect = W/H = invH/invW.
    const float  aspect    = uInvViewportH / uInvViewportW;
    const float2 corrected = float2(delta.x * aspect, delta.y);
    const float  dist      = length(corrected);

    // Expanding ring: the crest travels outward as uPhase grows; exp(-decay·dist) fades it with radius.
    const float wave   = sin(kTwoPi * (uFrequency * dist - uPhase));
    const float env    = exp(-uDecay * dist);
    const float offset = uAmplitude * wave * env;  // viewport pixels

    // Displace the sample radially (outward from the centre), amplitude px→UV per axis.
    const float2 dir = dist > 1e-5f ? (delta / dist) : float2(0.0f, 0.0f);
    const float2 src = uv + dir * float2(offset * uInvViewportW, offset * uInvViewportH);

    return SourceTexture.Sample(SourceSampler, src);
}
