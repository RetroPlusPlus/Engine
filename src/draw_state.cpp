#include "retropp/draw_state.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <variant>

namespace retropp {

namespace {

std::string quoted(std::string_view s) { return "\"" + std::string(s) + "\""; }

std::string describeCollision(const LayerKeyCollision& c) {
    switch (c.kind) {
        case LayerKeyCollision::Kind::DuplicateZ:
            return "layerDrawOrder: duplicate z=" + std::to_string(c.z) +
                   " between layers key=" + quoted(c.first) + " and key=" + quoted(c.second) +
                   " — z must be unique within a frame";
        case LayerKeyCollision::Kind::DuplicateKey:
            return "layerDrawOrder: duplicate key=" + quoted(c.first) +
                   " — layer key must be unique within a frame";
        case LayerKeyCollision::Kind::EmptyKey:
            return "layerDrawOrder: empty layer key at z=" + std::to_string(c.z) +
                   " — every layer needs a non-empty reconciliation key";
    }
    return "layerDrawOrder: layer key collision";  // unreachable; silences -Wreturn-type
}

}  // namespace

std::vector<std::size_t> layerDrawOrder(std::span<const DrawLayer> layers,
                                        LayerKeyCollisionPolicy policy) {
    if (const auto collision = findLayerKeyCollision(layers)) {
        const std::string msg = describeCollision(*collision);
        if (policy == LayerKeyCollisionPolicy::Throw) {
            throw std::invalid_argument(msg);
        }
        // WarnAndResolve: keep a shipped game running — log loudly, then resolve below.
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "%s", msg.c_str());
    }

    std::vector<std::size_t> order(layers.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    // Stable sort by z ALONE — z is the only depth key; the reconciliation key has no ordering role.
    // Stability preserves submission order for equal z (only reachable on the WarnAndResolve path, where
    // a duplicate-z collision was tolerated), keeping the resolved order deterministic.
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return layers[a].z < layers[b].z; });
    return order;
}

std::optional<SpriteKeyCollision> findSpriteKeyCollision(std::span<const DrawLayer> layers) {
    std::unordered_set<std::string_view> seen;
    for (const DrawLayer& layer : layers) {
        if (contentKind(layer.content) != LayerContentKind::Sprites) continue;
        for (const Sprite& s : std::get<SpriteContent>(layer.content).sprites) {
            const std::string_view k = s.key;
            if (k.empty()) {
                return SpriteKeyCollision{SpriteKeyCollision::Kind::EmptyKey, k, k};
            }
            if (!seen.insert(k).second) {
                return SpriteKeyCollision{SpriteKeyCollision::Kind::DuplicateKey, k, k};
            }
        }
    }
    return std::nullopt;
}

void validateSpriteKeys(std::span<const DrawLayer> layers, LayerKeyCollisionPolicy policy) {
    const auto collision = findSpriteKeyCollision(layers);
    if (!collision) return;
    std::string msg;
    switch (collision->kind) {
        case SpriteKeyCollision::Kind::DuplicateKey:
            msg = "validateSpriteKeys: duplicate sprite key=" + quoted(collision->first) +
                  " — sprite keys must be unique within a frame (one interpolation slot per key)";
            break;
        case SpriteKeyCollision::Kind::EmptyKey:
            msg = "validateSpriteKeys: empty sprite key — every sprite needs a non-empty reconciliation key";
            break;
    }
    if (policy == LayerKeyCollisionPolicy::Throw) {
        throw std::invalid_argument(msg);
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "%s", msg.c_str());
}

}  // namespace retropp
