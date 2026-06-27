// Routine delivery runtime state (see retropp/routine_registry.h). Pure data: the embedded-
// bytecode registry and the fanned-out engine-config default policy. NO dependency on Vm or the VM
// backend — registering an embedded routine never force-links the VM, so the lean-binary property holds
// (the VM core arrives only when a game actually calls a register door). A LoadFromPath routine resolves
// against the engine's single assetRoot() (asset_registry.h) — there is no separate routine root.
#include "retropp/routine_registry.h"

#include <string>
#include <unordered_map>

namespace retropp {

namespace {

struct EmbeddedRoutine {
    const std::uint8_t* bytes;
    std::size_t         size;
};

// Function-local static (the asset_registry pattern): constructed on first use, before any pre-main
// registration TU runs, so static-init registrations land safely regardless of TU init order.
std::unordered_map<std::string, EmbeddedRoutine>& registry() {
    static std::unordered_map<std::string, EmbeddedRoutine> reg;
    return reg;
}

}  // namespace

namespace detail {

void registerEmbeddedRoutine(std::string_view path, const std::uint8_t* bytes, std::size_t size) {
    registry()[std::string(path)] = EmbeddedRoutine{bytes, size};
}

std::span<const std::uint8_t> findEmbeddedRoutine(std::string_view path) {
    const auto& reg = registry();
    const auto  it  = reg.find(std::string(path));
    if (it == reg.end()) {
        return {};
    }
    return {it->second.bytes, it->second.size};
}

}  // namespace detail
}  // namespace retropp
