// Round-half-up, the tie rule the CPU mirrors use.
//
// Engine-internal header — see blend_ops.hlsli for why shaders/common is separate from shaders/include.
// Self-contained: reads no shader-declared state.
//
// HLSL's round() is round-to-even and disagrees with the CPU authority at a tie, so displacement offsets
// quantize through this instead.

#ifndef RETROPP_COMMON_ROUNDING_HLSLI
#define RETROPP_COMMON_ROUNDING_HLSLI

float roundHalfUp(float v) { return floor(v + 0.5f); }

#endif  // RETROPP_COMMON_ROUNDING_HLSLI
