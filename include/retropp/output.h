#pragma once

namespace retropp {

// How the internal viewport is sampled when it is blitted onto the window. This is a
// blit-stage SAMPLER choice, not a shader change: the same blit pipeline binds whichever
// sampler the current mode selects.
//
//   Nearest  — point sampling: crisp, square pixels. The FAITHFUL baseline and the default.
//   Bilinear — linear filtering: smooths the upscale (softer, non-pixelated look).
//
// Identity is the named enumerator — never a raw filter constant at the call site. Richer
// post-process filtering (CRT and friends) is a separate post-process stage, not a sampler
// mode, and attaches in the post-process chain — it is deliberately NOT a SamplingMode here.
enum class SamplingMode {
    Nearest,   // faithful default — crisp integer pixels
    Bilinear,  // smoothed upscale
};

// The grid the analytic render paths (transformed tiles, effect regions, the sampling effects) evaluate
// their spatial math on when the compositor runs above viewport resolution (sub-pixel interpolated
// placement composites onto a finer grid). Evaluation granularity is separate from placement granularity:
// placement stays sub-pixel for steady motion regardless of this choice; this chooses only where the
// geometry is sampled.
//
//   Viewport — evaluate on the viewport grid: the image is pixel-identical to the viewport-resolution
//              rasterization, nearest-upscaled (crisp, square pixels). The default. A mathematical no-op
//              when the compositor runs at viewport resolution.
//   Output   — evaluate on the output grid: the geometry is sampled per output pixel, so edges and
//              displacement resolve smoothly at the higher resolution (softer under upscale).
//
// Identity is the named enumerator — never a raw flag at the call site. Lives here beside SamplingMode so
// the SDL-free config layer (EngineConfig) can carry it without reaching into the GPU renderer header.
enum class EvaluationGrid {
    Viewport,  // crisp default — evaluate on the viewport grid
    Output,    // evaluate on the output grid — smooth
};

}  // namespace retropp
