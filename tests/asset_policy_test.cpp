// Asset embed policy resolution. resolveAssetPolicy is the single pure function that both the runtime
// loaders and the build-time embed codegen apply, so the precedence is tested once here — two tiers:
//   per-call argument  >  per-type default.
// There is NO process-global default tier. Per-type defaults in the field: loadMapPng = Embed,
// loadAtlas = LoadFromPath — exercised below as the two perTypeDefault values.

#include <optional>

#include <gtest/gtest.h>

#include "retropp/asset_policy.h"

using retropp::AssetPolicy;
using retropp::resolveAssetPolicy;

// A present per-call argument wins over the per-type default, in either direction.
TEST(AssetPolicy, PerCallOverridesPerType) {
    EXPECT_EQ(resolveAssetPolicy(AssetPolicy::Embed, AssetPolicy::LoadFromPath), AssetPolicy::Embed);
    EXPECT_EQ(resolveAssetPolicy(AssetPolicy::LoadFromPath, AssetPolicy::Embed), AssetPolicy::LoadFromPath);
}

// With no per-call argument, the loader's per-type default decides — Embed for the loadMapPng tier,
// LoadFromPath for the loadAtlas tier.
TEST(AssetPolicy, PerTypeDefaultIsTheFallback) {
    EXPECT_EQ(resolveAssetPolicy(std::nullopt, AssetPolicy::Embed), AssetPolicy::Embed);          // loadMapPng
    EXPECT_EQ(resolveAssetPolicy(std::nullopt, AssetPolicy::LoadFromPath), AssetPolicy::LoadFromPath);  // loadAtlas
}

// The resolver is constexpr — the build-time codegen relies on resolving the same way at configure time.
TEST(AssetPolicy, ResolvesAtCompileTime) {
    static_assert(resolveAssetPolicy(std::nullopt, AssetPolicy::Embed) == AssetPolicy::Embed);
    static_assert(resolveAssetPolicy(std::nullopt, AssetPolicy::LoadFromPath) == AssetPolicy::LoadFromPath);
    static_assert(resolveAssetPolicy(AssetPolicy::LoadFromPath, AssetPolicy::Embed) ==
                  AssetPolicy::LoadFromPath);
    SUCCEED();
}
