// The emission keying function shared by the extract passes.
//
// Engine-internal header — see blend_ops.hlsli for why shaders/common is separate from shaders/include.
// Self-contained: reads no shader-declared state, so it may be included anywhere in the file.

#ifndef RETROPP_COMMON_EMISSION_MASK_HLSLI
#define RETROPP_COMMON_EMISSION_MASK_HLSLI

// The scalar emission mask at a PREMULTIPLIED source pixel — mirrors retropp::glowMask. Un-premultiplies
// before keying (rgb / a, guarded at a = 0) so the key reads true brightness, not coverage-dimmed light.
// threshold 0 is the whole-coverage emission mode: every covered pixel emits fully, dark content included.
float glowMask(float4 s, float threshold) {
    if (s.a <= 0.0f) return 0.0f;
    if (threshold <= 0.0f) return s.a;
    float3 straight = s.rgb / s.a;
    float  lum      = straight.r * 0.299f + straight.g * 0.587f + straight.b * 0.114f;
    float  den      = max(1.0f - threshold, 1.0f / 255.0f);
    return s.a * saturate((lum - threshold) / den);
}

#endif  // RETROPP_COMMON_EMISSION_MASK_HLSLI
