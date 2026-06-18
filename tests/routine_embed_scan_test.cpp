// ENG-4.B Step 4 — the register* build scan (retropp_autoembed_routines), proven end to end. Because the
// registerAudio call below names a project-root-relative .asm with AssetPolicy::Embed, the build scanned
// this source, assembled that .asm to SM83 bytecode AT COMPILE TIME, and recorded the bytes in the
// routine registry — so findEmbeddedRoutine returns them here, with NO runtime .asm read. This is the
// producer the AudioSystem/Vm Embed path consumes; this test is its paper trail.
#include "retropp/audio_library.h"
#include "retropp/routine_registry.h"

#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace retropp {
namespace {

TEST(RoutineEmbedScan, EmbedBakesCorrectBytecodeIntoTheBinary) {
    // The path is project-root-relative (what the scan resolves against CMAKE_SOURCE_DIR) and passed as a
    // string LITERAL (LiteralPath binds only a literal — what the scan can see). The returned id is unused
    // — the point is the build-time side effect the call site triggers: the .asm is baked into this binary.
    (void)AudioLibrary::instance().registerAudio("tests/fixtures/routines/add_ab.asm", AudioType::Sfx,
                                                 Isa::Sm83, AssetPolicy::Embed);

    const std::span<const std::uint8_t> baked =
        detail::findEmbeddedRoutine("tests/fixtures/routines/add_ab.asm");
    ASSERT_FALSE(baked.empty()) << "the scan did not bake the Embed routine into the binary";

    // add_ab.asm is `add a, b` (0x80) then `ret` (0xC9) — proves the compile-time assemble is correct,
    // not merely that some bytes were recorded.
    const std::vector<std::uint8_t> expected{0x80, 0xC9};
    EXPECT_EQ(std::vector<std::uint8_t>(baked.begin(), baked.end()), expected);
}

}  // namespace
}  // namespace retropp
