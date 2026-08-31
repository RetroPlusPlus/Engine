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
// This is platform-internal — games call Renderer::registerPostProcessStage(path, …); they do not touch
// these functions directly. The generated registry TU is the only caller of registerShaderVariants.

namespace retropp {

struct ScreenSpaceEffect;  // draw_state.h — the packer reads the effect's inline param fields

// Fills a custom shader's cbuffer from a ScreenSpaceEffect's inline param fields, returning the byte
// size written. GENERATED per shader (gen_effect_fields.cmake) and compiled in the consumer's TU, where
// ScreenSpaceEffect's full param layout is visible — so the renderer obtains bytes without ever reading
// the param members itself (keeps renderer.cpp layout-agnostic). The destination buffer is platform-owned
// and at least kMaxCustomEffectUniformBytes (renderer.cpp).
using EffectPacker = std::uint32_t (*)(const ScreenSpaceEffect&, std::byte*);

namespace detail {

// Record that `path` (the exact string used in the registering code) resolves to `variants` (a pointer
// to a constexpr ShaderVariants in a generated header, valid for the program lifetime), `packer` (the
// generated cbuffer packer for that shader, or nullptr for a parameterless shader), `batched` (the
// instanced-additive region variant when the shader carries a `// @retropp:additive` declaration, else
// nullptr), `gather` (the union-shape gather variant, compiled for every custom shader EXCEPT additive-
// or `// @retropp:no-gather`-declared ones), `sprite` (the sprite-inline variant, compiled for every custom
// shader EXCEPT `// @retropp:no-sprite`-declared or int / uint-param ones), and `spriteBelow` (the
// scene-facing sprite-inline variant — the same body over the below sprite fragment, compiled under the same
// eligibility as `sprite`), `emission` (the <ns>_emission EXTRACT variant — the shader's `emission()` body run
// in the extract pass — when an emission-declared shader defines one, else nullptr), and `emissionConsumer`
// (whether the shader carries a `// @retropp:emission` declaration; when true the renderer runs the
// extract → blur → stage chain for it). All extra variants reflect the SAME cbuffer, so they reuse `packer`.
// Called from the auto-generated per-target registry TU's static initializers, before main().
void registerShaderVariants(std::string_view path, const ShaderVariants* variants, EffectPacker packer,
                            const ShaderVariants* batched = nullptr,
                            const ShaderVariants* gather = nullptr,
                            const ShaderVariants* sprite = nullptr,
                            const ShaderVariants* spriteBelow = nullptr,
                            const ShaderVariants* emission = nullptr,
                            const ShaderVariants* emissionRect = nullptr,
                            bool emissionConsumer = false);

// The ShaderVariants registered for `path`, or nullptr if no shader was compiled for that path (the path
// was never referenced in a scanned source, so the build did not compile it). Renderer surfaces the
// nullptr as a clear registration error.
[[nodiscard]] const ShaderVariants* findShaderVariants(std::string_view path);

// The BATCHED (instanced-additive region) ShaderVariants for `path`, or nullptr if the shader carries no
// `// @retropp:additive` declaration (so no batched variant was compiled). The renderer builds the batched
// pipeline only when this is non-null — the nullptr IS the "stage is not additive" flag.
[[nodiscard]] const ShaderVariants* findBatchedShaderVariants(std::string_view path);

// The GATHER (union-shape replace-region) ShaderVariants for `path`, or nullptr if the shader is additive-
// or `// @retropp:no-gather`-declared (so no gather variant was compiled). The renderer builds the gather
// pipelines only when this is non-null — the nullptr IS the "stage does not gather" flag (unroutable).
[[nodiscard]] const ShaderVariants* findGatherShaderVariants(std::string_view path);

// The SPRITE-INLINE ShaderVariants for `path` — the sprite fragment with this shader's body injected, run
// per sprite pixel — or nullptr if the shader is `// @retropp:no-sprite`-declared or carries an int / uint
// cbuffer param (so no sprite variant was compiled). The renderer builds the sprite-inline pipeline only when
// this is non-null — the nullptr IS the "stage can't run on a sprite" flag (the sprite skips that effect).
[[nodiscard]] const ShaderVariants* findSpriteShaderVariants(std::string_view path);

// The SPRITE-BELOW-INLINE ShaderVariants for `path` — the below sprite fragment with this shader's body
// injected, its sampleSource reading the SCENE — or nullptr under the same exclusions as the sprite variant.
// The renderer builds the below-custom pipeline only when this is non-null; the nullptr IS the "no scene-read
// sprite variant" flag (a Below-scope Custom effect on a sprite skips that effect).
[[nodiscard]] const ShaderVariants* findSpriteBelowShaderVariants(std::string_view path);

// The EMISSION EXTRACT ShaderVariants for `path` — the shader's `emission()` body compiled as the extract
// entry — or nullptr when the shader defines no such body (an emission consumer whose demand defaults to the
// stock brightpass) or is not an emission consumer at all. The renderer builds the custom extract pipeline
// only when this is non-null; the nullptr means "extract with the stock brightpass at `.threshold`".
[[nodiscard]] const ShaderVariants* findEmissionShaderVariants(std::string_view path);

// The EMISSION-RECT ShaderVariants for `path` — the shader's `emission()` body injected into the rect-instanced
// below extract fragment (it authors a below lens's field over the field's rect, reading the SCENE through
// sampleSource) — or nullptr when the shader defines no `emission()` body or is not an emission consumer. The
// renderer builds the below custom extract pipeline only when this is non-null; the nullptr means a Below-scope
// Custom lens fills its field through the stock rect brightpass at `.threshold`.
[[nodiscard]] const ShaderVariants* findEmissionRectShaderVariants(std::string_view path);

// Whether `path` is an emission CONSUMER — carries a `// @retropp:emission` declaration. When true the
// renderer runs the extract → blur → stage chain for the stage and its fullscreen variants were compiled
// with the emission binding (sampleEmission). Independent of findEmissionShaderVariants: a consumer with no
// `emission()` body is a consumer (true) with a null extract variant (stock brightpass).
[[nodiscard]] bool isEmissionConsumer(std::string_view path);

// The generated cbuffer packer registered for `path`, or nullptr (unregistered path, or a parameterless
// shader). Resolved alongside the variants at registration time.
[[nodiscard]] EffectPacker findEffectPacker(std::string_view path);

}  // namespace detail
}  // namespace retropp
