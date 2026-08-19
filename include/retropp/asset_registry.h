#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include "retropp/literal_path.h"   // LiteralPath

// Asset runtime state: the embedded-asset registry (bin2c-populated, mirrors
// shader_registry) and the engine-owned asset path resolution. The loaders (Renderer::loadAtlas,
// loadMapPng) read both; games never touch the detail functions. assetRoot / assetPath ARE public: they
// are the built-in path helper that removes the need
// to hand-build "basePath/assets/name" strings (the classic pathing mistake) — call assetPath() when
// reading an asset from disk and the engine resolves it the one correct way.

namespace retropp {

// ── Built-in asset path resolution (engine-owned) ────────────────────────────────────────────────
// A logical asset path is RELATIVE TO THE PROJECT ROOT (e.g. "examples/assets/world.png"). The same
// string addresses the asset in both contexts: the build resolves it against the project source root to
// bake it; the runtime resolves it against assetRoot. assetRoot is the runtime base that holds the
// project-relative asset tree — EngineConfig::setActive resolves it to an ABSOLUTE path once (so the
// executable/base-dir join happens in exactly one place), defaulting to the executable directory and
// overridable (the project root during development, the extracted-asset directory for a shipped game).
[[nodiscard]] const std::filesystem::path& assetRoot() noexcept;
void setAssetRoot(std::filesystem::path absoluteRoot);

// The on-disk path of a logical (project-root-relative) asset: assetRoot() / logical. Call this when
// reading an asset from disk rather than constructing the path by hand — the loaders use it internally
// for LoadFromPath assets, so a game never hand-builds a base-path join.
[[nodiscard]] std::filesystem::path assetPath(LiteralPath logical);

namespace detail {

// Record that a logical asset path resolves to embedded bytes (a constexpr array in a generated header,
// valid for the program lifetime). Called from the auto-generated per-target registry TU's static
// initializers (retropp_autoembed_assets), before main().
void registerEmbeddedAsset(std::string_view path, const std::uint8_t* bytes, std::size_t size);

// The embedded bytes registered for `path`, or an empty span if none were baked for it (the path was
// not embedded — either policy was LoadFromPath, or it was never scanned). A caller that resolved the
// policy to Embed and got an empty span falls back to the runtime disk read and reports it through
// warnEmbedNotBaked below.
[[nodiscard]] std::span<const std::uint8_t> findEmbeddedAsset(std::string_view path);

// Report that `path` resolved to the Embed policy but had nothing baked for it, so the call is reading
// from disk instead. `kind` names what was expected ("asset", "routine") and appears in the message.
//
// Embed's promise is that the bytes ship inside the binary and nothing is read at runtime; a fallback
// means the build did not bake this path, which is a build fault rather than a policy the code chose.
// Two causes account for nearly all of them: the target was never run through the scan, or the target is
// a static library whose registry the linker discarded (retropp_anchor_registry in CMakeLists.txt). Both
// otherwise pass unnoticed on a machine that has the source tree, because the disk read succeeds there
// and fails only in a shipped artifact. Warned once per path — a loader in a loop reports one line.
void warnEmbedNotBaked(std::string_view kind, std::string_view path);

}  // namespace detail
}  // namespace retropp
