#include "retropp/asset_registry.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace retropp {

namespace {
// The runtime asset root: EngineConfig::setActive resolves it to an ABSOLUTE path once and stores it
// here; until then it is empty (assetPath then yields the logical path relative to the cwd). A
// function-local static so there is no global-init-order dependency.
std::filesystem::path& assetRootStorage() {
    static std::filesystem::path root;
    return root;
}
}  // namespace

const std::filesystem::path& assetRoot() noexcept { return assetRootStorage(); }

void setAssetRoot(std::filesystem::path absoluteRoot) { assetRootStorage() = std::move(absoluteRoot); }

std::filesystem::path assetPath(LiteralPath logical) {
    // assetRoot() / logical. If the logical path is itself absolute, operator/ yields it unchanged; if
    // assetRoot() is empty (no setActive yet), the result is the logical path relative to the cwd.
    return assetRootStorage() / logical.c_str();
}

namespace detail {

namespace {
// The embedded-asset table: a logical (project-root-relative) path → the program-lifetime byte array
// the build baked for it (a constexpr array in a generated header). Mirrors shader_registry's table:
// a function-local static so the generated registry TU's pre-main() static initializers never touch a
// not-yet-constructed global.
struct Bytes {
    const std::uint8_t* data = nullptr;
    std::size_t         size = 0;
};
std::unordered_map<std::string, Bytes>& table() {
    static std::unordered_map<std::string, Bytes> t;
    return t;
}
}  // namespace

void registerEmbeddedAsset(std::string_view path, const std::uint8_t* bytes, std::size_t size) {
    table().insert_or_assign(std::string(path), Bytes{bytes, size});
}

std::span<const std::uint8_t> findEmbeddedAsset(std::string_view path) {
    const auto it = table().find(std::string(path));
    if (it == table().end()) return {};
    return {it->second.data, it->second.size};
}

}  // namespace detail
}  // namespace retropp
