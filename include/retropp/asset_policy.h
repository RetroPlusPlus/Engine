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
//   per-call argument  >  EngineConfig::defaultAssetPolicy  >  per-type default.
// Per-type defaults: loadAtlas → LoadFromPath (atlases are the copyright surface); loadMapPng → Embed
// (map PNGs are bespoke index data, build-time design inputs).
enum class AssetPolicy : std::uint8_t { Embed, LoadFromPath };

// Resolve the effective policy for one asset given the per-call argument (nullopt = not specified) and
// the loader's per-type default. The middle tier — EngineConfig::defaultAssetPolicy — is supplied by the
// caller (the loaders read it from the fanned-out runtime default; the build reads the scanned config
// literal), so this helper stays free of any engine-config dependency and is trivially unit-testable.
[[nodiscard]] constexpr AssetPolicy resolveAssetPolicy(std::optional<AssetPolicy> perCall,
                                                       std::optional<AssetPolicy> engineConfigDefault,
                                                       AssetPolicy perTypeDefault) noexcept {
    if (perCall)             return *perCall;
    if (engineConfigDefault) return *engineConfigDefault;
    return perTypeDefault;
}

}  // namespace retropp
