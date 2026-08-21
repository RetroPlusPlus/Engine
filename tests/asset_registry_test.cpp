// Embedded-asset registry + the runtime embed path. Headless (no GPU): exercises the free
// loadMapPng (GPU-free) and the detail:: registry directly. Crucially, this TU also PROVES the bin2c
// codegen end-to-end on every CI platform: the EmbeddedMapDecodesIdenticalToDisk test below calls
// loadMapPng("tests/fixtures/map16.png", AssetPolicy::Embed), so retropp_autoembed_assets (applied to
// this test target) reads that committed fixture at build time, bakes its bytes into the binary, and
// registers them — and the test asserts decoding those baked bytes equals decoding the file off disk.

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/asset_policy.h"
#include "retropp/asset_registry.h"
#include "retropp/image.h"

namespace {
std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::string fixture(const char* name) { return std::string{RETROPP_FIXTURES_DIR} + "/" + name; }
}  // namespace

// register → find returns the exact bytes + size; a never-registered key returns an empty span.
TEST(AssetRegistry, RegisterFindRoundTrip) {
    static constexpr std::array<std::uint8_t, 4> kBytes{0xDE, 0xAD, 0xBE, 0xEF};
    retropp::detail::registerEmbeddedAsset("test/roundtrip.bin", kBytes.data(), kBytes.size());

    const std::span<const std::uint8_t> found = retropp::detail::findEmbeddedAsset("test/roundtrip.bin");
    ASSERT_EQ(found.size(), kBytes.size());
    EXPECT_TRUE(std::equal(found.begin(), found.end(), kBytes.begin()));

    EXPECT_TRUE(retropp::detail::findEmbeddedAsset("test/never-registered.bin").empty());
}

// assetPath joins the runtime asset root onto a logical (project-root-relative) path — the one correct
// join, so a game never hand-builds a base-path string.
TEST(AssetRegistry, AssetPathJoinsRoot) {
    retropp::setAssetRoot("/tmp/retropp-test-root");
    EXPECT_EQ(retropp::assetPath("examples/assets/world.png").generic_string(),
              "/tmp/retropp-test-root/examples/assets/world.png");
}

// The headline: an Embed-policy load decodes the bytes the BUILD baked (no disk read) and yields exactly
// what decoding the same PNG off disk yields — proving the bin2c codegen round-trips byte-for-byte.
TEST(AssetRegistry, EmbeddedMapDecodesIdenticalToDisk) {
    const retropp::IndexGrid embedded =
        retropp::loadMapPng("tests/fixtures/map16.png", retropp::AssetPolicy::Embed);
    const retropp::IndexGrid onDisk = retropp::loadMapPngFromMemory(readFile(fixture("map16.png")));

    ASSERT_FALSE(embedded.values.empty());  // it really came from the baked bytes, not an empty miss
    EXPECT_EQ(embedded.width, onDisk.width);
    EXPECT_EQ(embedded.height, onDisk.height);
    EXPECT_EQ(embedded.values, onDisk.values);
}

// An Embed-policy load whose bytes were NOT baked (no asset at that project path) falls through to the
// disk read rather than failing — so adding the policy never breaks an un-baked call. Here the disk read
// of a non-existent file then throws (the standard missing-file error), proving the fall-through path.
TEST(AssetRegistry, UnbakedEmbedFallsThroughToDisk) {
    // A literal whose file the build never baked → Embed misses the registry → falls through to the disk
    // read under the (empty) asset root → the standard missing-file throw.
    retropp::setAssetRoot("/tmp/retropp-nonexistent-root");
    EXPECT_THROW((void)retropp::loadMapPng("does_not_exist_embed.png", retropp::AssetPolicy::Embed),
                 std::runtime_error);
}
