#pragma once

#include <cstddef>
#include <string_view>

namespace retropp {

// A path that MUST be a compile-time string literal, because a build-time source scan finds it (to
// compile a custom shader, or to embed an asset). A runtime variable, std::string, std::string_view,
// std::filesystem::path, or any computed/concatenated path is invisible to that scan — nothing would
// be compiled or embedded for it — so the program would build and then throw at runtime. This type
// turns that latent runtime failure into a COMPILE error: a non-literal simply does not construct one.
//
// The APIs that genuinely load at runtime take std::filesystem::path instead (so a path resolved on
// the user's machine still works); LiteralPath is only for the path arguments a build step must read
// out of the source verbatim.
//
// Why a non-literal cannot construct one:
//   * The constructor takes `const char (&)[N]` — a reference to a character array. A `const char*`,
//     `std::string`, `std::string_view`, or `std::filesystem::path` does not bind to an array
//     reference, so the overload is not viable → compile error (no consteval needed for these).
//   * A runtime `char buf[N]` array *would* bind to the array reference; `consteval` closes that hole.
//     The body stores a pointer into the array, which is a constant expression only for a string
//     literal (static storage duration). For a runtime automatic array the pointer is not constant, so
//     the consteval evaluation is ill-formed → compile error.
//
// A string literal keeps its natural call syntax — `f("path/to/thing.ext")` constructs a LiteralPath
// implicitly — and the literal text still appears verbatim in the source, so the build-time scan sees
// it unchanged. The stored pointer has the string literal's static lifetime (the whole program run),
// so it stays valid wherever the LiteralPath is later read.
class LiteralPath {
public:
    template <std::size_t N>
    consteval LiteralPath(const char (&literal)[N]) noexcept  // NOLINT(google-explicit-constructor)
        : value_(literal), size_(N - 1) {}                    // N - 1 drops the trailing NUL

    [[nodiscard]] constexpr const char*      c_str() const noexcept { return value_; }
    [[nodiscard]] constexpr std::string_view view()  const noexcept { return {value_, size_}; }

private:
    const char* value_;
    std::size_t size_;
};

}  // namespace retropp
