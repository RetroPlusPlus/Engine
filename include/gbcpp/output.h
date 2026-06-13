#pragma once

namespace gbcpp {

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

}  // namespace gbcpp
