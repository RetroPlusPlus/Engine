#pragma once

#include <cstddef>
#include <optional>
#include <utility>

#include <SDL3/SDL_gpu.h>

namespace retropp {

// One compiled form of a shader, plus the entrypoint name to create it with. SPIR-V and
// DXIL keep "main"; the Metal path is renamed by the cross-compiler, so each variant
// carries its own entrypoint (the generated headers record it per format).
struct ShaderBytecode {
    const unsigned char* data = nullptr;
    std::size_t          size = 0;
    const char*          entrypoint = "main";
};

// A shader in every backend format the engine ships. The generated per-shader headers
// (shaders/generated/*.h) supply the byte arrays; the renderer assembles them into one
// of these and asks selectShader() for the variant the live device accepts.
struct ShaderVariants {
    ShaderBytecode spirv;     // Vulkan
    ShaderBytecode dxil;      // Direct3D 12
    ShaderBytecode metallib;  // Metal (precompiled library)
};

// Pick the variant matching the device's supported shader formats (as reported by
// SDL_GetGPUShaderFormats), returning the chosen bytecode together with the single
// format flag it satisfies — or nullopt when no shipped variant is available for the
// device. A variant whose data is null is treated as absent, so a device that supports
// a format we did not generate falls through rather than handing back empty bytecode.
[[nodiscard]] inline std::optional<std::pair<ShaderBytecode, SDL_GPUShaderFormat>>
selectShader(SDL_GPUShaderFormat supported, const ShaderVariants& variants) noexcept {
    if ((supported & SDL_GPU_SHADERFORMAT_SPIRV) && variants.spirv.data != nullptr) {
        return std::make_pair(variants.spirv, SDL_GPU_SHADERFORMAT_SPIRV);
    }
    if ((supported & SDL_GPU_SHADERFORMAT_DXIL) && variants.dxil.data != nullptr) {
        return std::make_pair(variants.dxil, SDL_GPU_SHADERFORMAT_DXIL);
    }
    if ((supported & SDL_GPU_SHADERFORMAT_METALLIB) && variants.metallib.data != nullptr) {
        return std::make_pair(variants.metallib, SDL_GPU_SHADERFORMAT_METALLIB);
    }
    return std::nullopt;
}

}  // namespace retropp
