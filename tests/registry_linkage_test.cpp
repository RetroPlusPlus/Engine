// The registries a static library carries must survive into the program that links it.
//
// Each build scan emits a generated translation unit whose whole content is a static initializer. Nothing
// references it, and an archive member is pulled into a link only to satisfy an undefined symbol — so
// without the anchor CMake attaches (retropp_anchor_registry), the linker discards the registry and every
// registration in it. The lookups then miss and the loaders read from disk instead, which succeeds
// wherever the source tree is present and fails in a shipped artifact.
//
// tests/registry_linkage/ is a static library whose sources carry one registration call per registry
// kind. It is linked into this executable, so these three assertions read the state of a real archive
// link. Each path is registered by that library alone: no other target bakes them, so a pass cannot come
// from somewhere else. Removing the anchor from any one scan turns its assertion red and leaves the other
// two green, which is what makes the three separable.

#include <gtest/gtest.h>

#include <cstdint>
#include <span>

#include "retropp/asset_registry.h"
#include "retropp/routine_registry.h"
#include "retropp/shader_registry.h"

namespace retropp {
namespace {

constexpr std::string_view kRoutine = "tests/fixtures/linkage/probe.asm";
constexpr std::string_view kAsset   = "tests/fixtures/linkage/probe.png";
constexpr std::string_view kShader  = "tests/shaders/linkage_probe.frag.hlsl";

TEST(RegistryLinkage, RoutineBytecodeSurvivesAStaticLibraryLink) {
    const std::span<const std::uint8_t> baked = detail::findEmbeddedRoutine(kRoutine);
    ASSERT_FALSE(baked.empty()) << "the routine registry was discarded from the archive";
    // probe.asm is `ld a, $2A` + `ret` — the exact bytes the constexpr SM83 assembler produces at build
    // time. Asserting them rather than the span's size proves the program holds the BAKED bytecode, not
    // some other span that happens to be registered under this path.
    ASSERT_EQ(baked.size(), 3u);
    EXPECT_EQ(baked[0], 0x3Eu);  // ld a, n8
    EXPECT_EQ(baked[1], 0x2Au);  // n8 = $2A
    EXPECT_EQ(baked[2], 0xC9u);  // ret
}

TEST(RegistryLinkage, AssetBytesSurviveAStaticLibraryLink) {
    const std::span<const std::uint8_t> baked = detail::findEmbeddedAsset(kAsset);
    ASSERT_FALSE(baked.empty()) << "the asset registry was discarded from the archive";
    // The PNG signature: the registered span is the file's own bytes, not an empty or placeholder record.
    ASSERT_GE(baked.size(), 8u);
    EXPECT_EQ(baked[0], 0x89u);
    EXPECT_EQ(baked[1], 'P');
    EXPECT_EQ(baked[2], 'N');
    EXPECT_EQ(baked[3], 'G');
}

TEST(RegistryLinkage, ShaderVariantsSurviveAStaticLibraryLink) {
    EXPECT_NE(detail::findShaderVariants(kShader), nullptr)
        << "the shader registry was discarded from the archive";
}

TEST(RegistryLinkage, AnUnregisteredPathStillResolvesToNothing) {
    // The three assertions above are only meaningful if a miss is actually reachable — a lookup that
    // returned something for any path would pass them without the registries being linked at all.
    EXPECT_TRUE(detail::findEmbeddedRoutine("tests/fixtures/linkage/absent.asm").empty());
    EXPECT_TRUE(detail::findEmbeddedAsset("tests/fixtures/linkage/absent.png").empty());
    EXPECT_EQ(detail::findShaderVariants("tests/shaders/absent.frag.hlsl"), nullptr);
}

}  // namespace
}  // namespace retropp
