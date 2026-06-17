// Asset embed policy resolution (ENG-2.M.b). resolveAssetPolicy is the single pure function that both
// the runtime loaders and the build-time embed codegen apply, so the precedence is tested once here:
//   per-call argument  >  EngineConfig::defaultAssetPolicy  >  per-type default.
// Per-type defaults in the field: loadMapPng = Embed, loadAtlas = LoadFromPath — exercised below as the
// two perTypeDefault values.

#include <optional>

#include <gtest/gtest.h>

#include "retropp/asset_policy.h"

using retropp::AssetPolicy;
using retropp::resolveAssetPolicy;

// A present per-call argument wins over both lower tiers, in either direction.
TEST(AssetPolicy, PerCallOverridesEverything) {
    EXPECT_EQ(resolveAssetPolicy(AssetPolicy::Embed, AssetPolicy::LoadFromPath, AssetPolicy::LoadFromPath),
              AssetPolicy::Embed);
    EXPECT_EQ(resolveAssetPolicy(AssetPolicy::LoadFromPath, AssetPolicy::Embed, AssetPolicy::Embed),
              AssetPolicy::LoadFromPath);
}

// With no per-call argument, the EngineConfig default decides — over the per-type default.
TEST(AssetPolicy, ConfigDefaultAppliesWhenNoPerCall) {
    EXPECT_EQ(resolveAssetPolicy(std::nullopt, AssetPolicy::Embed, AssetPolicy::LoadFromPath),
              AssetPolicy::Embed);
    EXPECT_EQ(resolveAssetPolicy(std::nullopt, AssetPolicy::LoadFromPath, AssetPolicy::Embed),
              AssetPolicy::LoadFromPath);
}

// With neither per-call nor config default, the loader's per-type default decides — Embed for the
// loadMapPng tier, LoadFromPath for the loadAtlas tier.
TEST(AssetPolicy, PerTypeDefaultIsTheFallback) {
    EXPECT_EQ(resolveAssetPolicy(std::nullopt, std::nullopt, AssetPolicy::Embed),
              AssetPolicy::Embed);          // loadMapPng's tier
    EXPECT_EQ(resolveAssetPolicy(std::nullopt, std::nullopt, AssetPolicy::LoadFromPath),
              AssetPolicy::LoadFromPath);   // loadAtlas's tier
}

// The resolver is constexpr — the build-time codegen relies on resolving the same way at configure time.
TEST(AssetPolicy, ResolvesAtCompileTime) {
    static_assert(resolveAssetPolicy(std::nullopt, std::nullopt, AssetPolicy::Embed) == AssetPolicy::Embed);
    static_assert(resolveAssetPolicy(std::nullopt, AssetPolicy::Embed, AssetPolicy::LoadFromPath) ==
                  AssetPolicy::Embed);
    static_assert(resolveAssetPolicy(AssetPolicy::LoadFromPath, AssetPolicy::Embed, AssetPolicy::Embed) ==
                  AssetPolicy::LoadFromPath);
    SUCCEED();
}
