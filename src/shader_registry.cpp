#include "retropp/shader_registry.h"

#include <string>
#include <unordered_map>

namespace retropp::detail {

namespace {
struct Entry {
    const ShaderVariants* variants = nullptr;
    EffectPacker          packer   = nullptr;
    const ShaderVariants* batched  = nullptr;  // instanced-additive region variant; null = not additive
};

// A function-local static (constructed on first use) so the static initializers in the generated
// registry TU — which run before main() in unspecified order — never touch a not-yet-constructed
// global map. Keyed by the path string exactly as written in the registering code.
std::unordered_map<std::string, Entry>& table() {
    static std::unordered_map<std::string, Entry> t;
    return t;
}
}  // namespace

void registerShaderVariants(std::string_view path, const ShaderVariants* variants, EffectPacker packer,
                            const ShaderVariants* batched) {
    table().insert_or_assign(std::string(path), Entry{variants, packer, batched});
}

const ShaderVariants* findShaderVariants(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it == table().end() ? nullptr : it->second.variants;
}

const ShaderVariants* findBatchedShaderVariants(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it == table().end() ? nullptr : it->second.batched;
}

EffectPacker findEffectPacker(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it == table().end() ? nullptr : it->second.packer;
}

}  // namespace retropp::detail
