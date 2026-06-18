#include "retropp/literal_path.h"

#include <string>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

namespace retropp {

// LiteralPath makes "this path must be a compile-time string literal" a property of the type, so a
// path consumed by a build-time source scan (custom-shader compile, asset embed) can never be a
// runtime value that the scan would miss. The enforcement is two-layered: non-array types are not
// even a viable constructor argument; a runtime char[] array is rejected by the consteval body.

// ── Compile-time enforcement guards ──────────────────────────────────────────────────────────────
// Robustly portable: these test overload VIABILITY (the array-reference parameter does not bind to a
// pointer / std::string / std::string_view), independent of consteval. A runtime `char[]` array DOES
// bind to the array reference and is rejected by consteval instead — that corner is the manual
// cross-compiler de-risk check in the plan, not an is_constructible assertion (is_constructible over a
// consteval ctor with a non-constant operand is the exact portability question being de-risked).
static_assert(!std::is_constructible_v<LiteralPath, const char*>,
              "a runtime const char* must not construct a LiteralPath");
static_assert(!std::is_constructible_v<LiteralPath, std::string>,
              "a std::string must not construct a LiteralPath");
static_assert(!std::is_constructible_v<LiteralPath, std::string_view>,
              "a std::string_view must not construct a LiteralPath");

// Positive: a real string literal constructs one, fully at compile time, and round-trips its text.
static_assert(LiteralPath{"x.png"}.view() == std::string_view{"x.png"},
              "a string literal must construct a LiteralPath whose view is the literal text");

// LiteralPath is path-type-agnostic; the .png literals in the TESTS below stand in for any asset path.
//
// CALL-KEYING REGRESSION GUARD: the shader scan (retropp_autocompile_shaders) is call-keyed — it
// compiles a shader only from a registerPostProcessStage(...) call, never from a bare "*.hlsl" string
// literal in code. The literal just below is NOT in such a call and names a shader that does not exist;
// the call-keyed scan must ignore it. If the scan ever regresses to bare-literal matching, the build
// fails trying to compile this non-existent shader. That failure IS the guard. (A path inside an actual
// registration call IS compiled, by design — see examples/custom_shader_demo.cpp.)
[[maybe_unused]] constexpr const char* kBareLiteralNotARegistration =
    "game/shaders/bare_literal_guard.frag.hlsl";
//
// COMMENT-STRIPPING REGRESSION GUARD: the scan strips comments before matching, so the shader paths
// named in this comment — "game/shaders/line_comment_guard.frag.hlsl" — and in the block comment just
// below must NOT be picked up either. If comment-stripping ever regresses, the build fails trying to
// compile these non-existent shaders. That failure IS the guard.
/* "game/shaders/block_comment_guard.frag.hlsl" — same: a shader path in a block comment must be stripped. */

TEST(LiteralPath, RoundTripsLiteralText) {
    constexpr LiteralPath p{"game/assets/world.png"};
    EXPECT_EQ(p.view(), "game/assets/world.png");
    EXPECT_STREQ(p.c_str(), "game/assets/world.png");
}

TEST(LiteralPath, ViewSizeExcludesTrailingNul) {
    constexpr LiteralPath p{"maps/r12.png"};  // 12 characters, plus the NUL the array carries
    EXPECT_EQ(p.view().size(), 12u);
    EXPECT_EQ(p.c_str()[p.view().size()], '\0');  // c_str stays NUL-terminated (it is the literal)
}

TEST(LiteralPath, DistinctLiteralsReadBackDistinct) {
    constexpr LiteralPath a{"a.png"};
    constexpr LiteralPath b{"world.png"};
    EXPECT_NE(a.view(), b.view());
}

TEST(LiteralPath, UsableInConstantExpressions) {
    constexpr LiteralPath p{"menu.png"};
    static_assert(p.view().size() == 8u);  // construction + view() are usable at compile time
    EXPECT_EQ(p.view(), "menu.png");
}

}  // namespace retropp
