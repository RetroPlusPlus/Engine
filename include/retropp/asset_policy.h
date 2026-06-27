#pragma once

#include <cstdint>
#include <optional>

namespace retropp {

// Whether an ingestible asset (an atlas image, a map PNG) is baked into the executable at build time
// or loaded from disk at runtime.
//
//   Embed        — the asset's bytes are compiled into the binary (build-time bin2c) and decoded at
//                  runtime from memory. The source file never ships. Use for self-contained binaries,
//                  art the developer doesn't want altered, and build-time-only design inputs (a map PNG).
//   LoadFromPath — the asset ships beside the binary (or is extracted there) and is read from disk at
//                  runtime, relative to EngineConfig::assetRoot. Use for moddable assets and — crucially —
//                  for copyright-derived assets that may never be baked into a shipped binary.
//
// The policy for a given asset is resolved (build-time AND runtime, identically) by precedence:
//   per-call argument  >  per-type default.
// Per-type defaults: loadAtlas → LoadFromPath (atlases are the copyright surface); loadMapPng → Embed
// (map PNGs are bespoke index data, build-time design inputs). There is NO process-global default tier —
// the per-type defaults ARE the defaults, deviated from only by the explicit per-call argument, which
// reads at the call site (no action-at-a-distance).
enum class AssetPolicy : std::uint8_t { Embed, LoadFromPath };

// Resolve the effective policy for one asset given the per-call argument (nullopt = not specified) and
// the loader's per-type default. Free of any engine-config dependency and trivially unit-testable; the
// build-time embed scan applies the same two-tier rule.
[[nodiscard]] constexpr AssetPolicy resolveAssetPolicy(std::optional<AssetPolicy> perCall,
                                                       AssetPolicy perTypeDefault) noexcept {
    return perCall ? *perCall : perTypeDefault;
}

}  // namespace retropp
