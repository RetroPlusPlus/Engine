#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "retropp/shader_format.h"

// Build-time shader auto-compilation registry. A game registers a custom shader stage by
// PATH — `renderer.registerPostProcessStage("path/to/my.frag.hlsl")` — and writes NO build rule and NO
// boilerplate: a CMake source scan (retropp_autocompile_shaders) finds every `.hlsl` path referenced in
// the target's sources, compiles each to this platform's GPU bytecode at BUILD time, reflects the
// shader's OWN cbuffer, and emits a generated translation unit whose static initializers register each
// path → (its compiled ShaderVariants, its generated packer) here. At runtime the registration call
// resolves the path against this table. The bytecode is embedded in the executable (no runtime shader
// compiler, nothing loaded from disk).
//
// This is engine plumbing — games call Renderer::registerPostProcessStage(path, …); they do not touch
// these functions directly. The generated registry TU is the only caller of registerShaderVariants.

namespace retropp {

struct ScreenSpaceEffect;  // draw_state.h — the packer reads the effect's inline param fields

// Fills a custom shader's cbuffer from a ScreenSpaceEffect's inline param fields, returning the byte
// size written. GENERATED per shader (gen_effect_fields.cmake) and compiled in the consumer's TU, where
// ScreenSpaceEffect's full param layout is visible — so the renderer obtains bytes without ever reading
// the param members itself (keeps renderer.cpp layout-agnostic). The destination buffer is engine-owned
// and at least kMaxCustomEffectUniformBytes (renderer.cpp).
using EffectPacker = std::uint32_t (*)(const ScreenSpaceEffect&, std::byte*);

namespace detail {

// Record that `path` (the exact string used in the registering code) resolves to `variants` (a pointer
// to a constexpr ShaderVariants in a generated header, valid for the program lifetime), `packer` (the
// generated cbuffer packer for that shader, or nullptr for a parameterless shader), `batched` (the
// instanced-additive region variant when the shader carries a `// retropp: additive` declaration, else
// nullptr), and `gather` (the union-shape gather variant, compiled for every custom shader EXCEPT
// additive- or `// retropp: no-gather`-declared ones). Both extra variants reflect the SAME
// cbuffer, so they reuse `packer`. Called from the auto-generated per-target registry TU's static
// initializers, before main().
void registerShaderVariants(std::string_view path, const ShaderVariants* variants, EffectPacker packer,
                            const ShaderVariants* batched = nullptr,
                            const ShaderVariants* gather = nullptr);

// The ShaderVariants registered for `path`, or nullptr if no shader was compiled for that path (the path
// was never referenced in a scanned source, so the build did not compile it). Renderer surfaces the
// nullptr as a clear registration error.
[[nodiscard]] const ShaderVariants* findShaderVariants(std::string_view path);

// The BATCHED (instanced-additive region) ShaderVariants for `path`, or nullptr if the shader carries no
// `// retropp: additive` declaration (so no batched variant was compiled). The renderer builds the batched
// pipeline only when this is non-null — the nullptr IS the "stage is not additive" flag.
[[nodiscard]] const ShaderVariants* findBatchedShaderVariants(std::string_view path);

// The GATHER (union-shape replace-region) ShaderVariants for `path`, or nullptr if the shader is additive-
// or `// retropp: no-gather`-declared (so no gather variant was compiled). The renderer builds the gather
// pipelines only when this is non-null — the nullptr IS the "stage does not gather" flag (unroutable).
[[nodiscard]] const ShaderVariants* findGatherShaderVariants(std::string_view path);

// The generated cbuffer packer registered for `path`, or nullptr (unregistered path, or a parameterless
// shader). Resolved alongside the variants at registration time.
[[nodiscard]] EffectPacker findEffectPacker(std::string_view path);

}  // namespace detail
}  // namespace retropp
