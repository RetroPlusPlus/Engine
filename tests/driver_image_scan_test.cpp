// A driver's images are subject to the same Embed contract as every other input, and the build has to
// find them somewhere other than the call site.
//
// `AudioLibrary::registerDriver` takes a `DriverPathBinding`, and that binding is normally its own
// variable — so the registerDriver statement carries a variable name and no path. The paths live in the
// `DriverImagePath` initializers inside the binding, each with its own `.policy`, and a binding routinely
// mixes them: an Embed boot image beside a LoadFromPath one that must never end up inside the binary.
//
// The declarations under test are in tests/registry_linkage/, whose binding carries all three cases. What
// is asserted here is what the build did with them — an image resolving to Embed has bytes in the
// registry its extension selects, and the LoadFromPath one has none anywhere.

#include <gtest/gtest.h>

#include <cstdint>
#include <span>

#include "retropp/asset_registry.h"
#include "retropp/routine_registry.h"

namespace retropp {
namespace {

// No `.policy` at all. `DriverImagePath::policy` is optional and AudioSystem resolves an unset one against
// Embed at host(), so the build has to agree or the two disagree about the same image.
constexpr std::string_view kUnsetPolicyAsm = "tests/fixtures/linkage/driver_boot.asm";
constexpr std::string_view kEmbedBinary    = "tests/fixtures/linkage/driver_embedded.bin";
constexpr std::string_view kLoadFromPath   = "tests/fixtures/linkage/driver_shipped.bin";

TEST(DriverImageScan, AnAsmImageWithNoPolicyIsAssembledAndBaked) {
    const std::span<const std::uint8_t> baked = detail::findEmbeddedRoutine(kUnsetPolicyAsm);
    ASSERT_FALSE(baked.empty()) << "an unset policy must resolve to Embed, as it does at host()";
    // driver_boot.asm is `ld a, $5B` + `ret`. Asserting the assembled bytes proves the image went through
    // the routine path — an `.asm` driver image is assembled at build time, not shipped as text.
    ASSERT_EQ(baked.size(), 3u);
    EXPECT_EQ(baked[0], 0x3Eu);  // ld a, n8
    EXPECT_EQ(baked[1], 0x5Bu);  // n8 = $5B
    EXPECT_EQ(baked[2], 0xC9u);  // ret
}

TEST(DriverImageScan, ANonAsmEmbedImageIsBakedVerbatim) {
    const std::span<const std::uint8_t> baked = detail::findEmbeddedAsset(kEmbedBinary);
    ASSERT_FALSE(baked.empty()) << "a raw driver image declared Embed must be baked";
    // Raw bytes, not assembled: the file's own contents reach the binary unchanged.
    ASSERT_EQ(baked.size(), 8u);
    EXPECT_EQ(baked[0], 0xE1u);
    EXPECT_EQ(baked[7], 0xE8u);
}

TEST(DriverImageScan, ALoadFromPathImageIsNeverBaked) {
    // The one that matters most. A driver image can be content a game has no right to ship inside its
    // binary, so LoadFromPath has to survive a binding whose *other* images are being baked — a scan that
    // resolved one policy per statement would bake this alongside them.
    EXPECT_TRUE(detail::findEmbeddedRoutine(kLoadFromPath).empty());
    EXPECT_TRUE(detail::findEmbeddedAsset(kLoadFromPath).empty());
}

TEST(DriverImageScan, EachImageLandsInTheRegistryItsExtensionSelects) {
    // host() reads an `.asm` image from the routine registry and any other from the asset registry, so an
    // image baked into the wrong one is invisible at runtime even though the bytes are in the binary.
    EXPECT_TRUE(detail::findEmbeddedAsset(kUnsetPolicyAsm).empty());
    EXPECT_TRUE(detail::findEmbeddedRoutine(kEmbedBinary).empty());
}

}  // namespace
}  // namespace retropp
