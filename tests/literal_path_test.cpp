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

// The path literals below (in CODE) deliberately avoid the shader extension: a real shader-path string
// literal in code is compiled as a shader by the build scan (retropp_autocompile_shaders), by design.
// LiteralPath is path-type-agnostic; .png stands in for any asset path.
//
// COMMENT-STRIPPING REGRESSION GUARD: the build scan strips comments before matching shader-path
// literals, so the shader paths named in this comment — "game/shaders/line_comment_guard.frag.hlsl" —
// and in the block comment just below must NOT be picked up. If comment-stripping ever regresses, the
// build fails trying to compile these non-existent shaders. That failure IS the guard.
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
