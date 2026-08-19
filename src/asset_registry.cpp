#include "retropp/asset_registry.h"

#include <SDL3/SDL.h>

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

void warnEmbedNotBaked(std::string_view kind, std::string_view path) {
    // One line per path: the first fallback carries the whole diagnosis, and a loader called in a loop
    // must not bury it. Function-local static for the same init-order reason the tables use one.
    static std::unordered_map<std::string, bool> warned;
    if (!warned.insert_or_assign(std::string(path), true).second) return;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "retropp: %.*s '%.*s' is declared Embed but the build baked nothing for it — reading it "
                "from disk instead. The bytes are meant to ship inside the binary; this read will fail "
                "wherever the file is absent. Either the target was not run through the build scan, or "
                "it is a static library whose generated registry the linker discarded.",
                static_cast<int>(kind.size()), kind.data(),
                static_cast<int>(path.size()), path.data());
}

}  // namespace detail
}  // namespace retropp
