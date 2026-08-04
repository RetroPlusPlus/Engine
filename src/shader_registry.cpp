#include "retropp/shader_registry.h"

#include <string>
#include <unordered_map>

namespace retropp::detail {

namespace {
struct Entry {
    const ShaderVariants* variants = nullptr;
    EffectPacker          packer   = nullptr;
    const ShaderVariants* batched  = nullptr;  // instanced-additive region variant; null = not additive
    const ShaderVariants* gather   = nullptr;  // union-shape gather variant; null = does not gather
    const ShaderVariants* sprite   = nullptr;  // sprite-inline variant; null = off the sprite path
    const ShaderVariants* spriteBelow = nullptr;  // scene-facing sprite-inline variant; null = no below-custom
    const ShaderVariants* emission = nullptr;  // emission() extract variant; null = stock brightpass extract
    const ShaderVariants* emissionRect = nullptr;  // emission() over the below rect; null = stock rect brightpass
    bool emissionConsumer          = false;    // carries // @retropp:emission — runs the emission chain
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
                            const ShaderVariants* batched, const ShaderVariants* gather,
                            const ShaderVariants* sprite, const ShaderVariants* spriteBelow,
                            const ShaderVariants* emission, const ShaderVariants* emissionRect,
                            bool emissionConsumer) {
    table().insert_or_assign(std::string(path),
                             Entry{variants, packer, batched, gather, sprite, spriteBelow, emission,
                                   emissionRect, emissionConsumer});
}

const ShaderVariants* findShaderVariants(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it == table().end() ? nullptr : it->second.variants;
}

const ShaderVariants* findBatchedShaderVariants(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it == table().end() ? nullptr : it->second.batched;
}

const ShaderVariants* findGatherShaderVariants(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it == table().end() ? nullptr : it->second.gather;
}

const ShaderVariants* findSpriteShaderVariants(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it == table().end() ? nullptr : it->second.sprite;
}

const ShaderVariants* findSpriteBelowShaderVariants(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it == table().end() ? nullptr : it->second.spriteBelow;
}

const ShaderVariants* findEmissionShaderVariants(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it == table().end() ? nullptr : it->second.emission;
}

const ShaderVariants* findEmissionRectShaderVariants(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it == table().end() ? nullptr : it->second.emissionRect;
}

bool isEmissionConsumer(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it != table().end() && it->second.emissionConsumer;
}

EffectPacker findEffectPacker(std::string_view path) {
    const auto it = table().find(std::string(path));
    return it == table().end() ? nullptr : it->second.packer;
}

}  // namespace retropp::detail
