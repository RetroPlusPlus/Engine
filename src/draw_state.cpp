#include "gbcpp/draw_state.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <string>

namespace gbcpp {

namespace {

std::string describeCollision(const LayerKeyCollision& c) {
    const auto idv = [](LayerId id) { return std::to_string(static_cast<std::uint32_t>(id)); };
    switch (c.kind) {
        case LayerKeyCollision::Kind::DuplicateZ:
            return "layerDrawOrder: duplicate z=" + std::to_string(c.z) +
                   " between layers id=" + idv(c.first) + " and id=" + idv(c.second) +
                   " — z must be unique within a frame";
        case LayerKeyCollision::Kind::DuplicateId:
            return "layerDrawOrder: duplicate id=" + idv(c.first) +
                   " — layer identity must be unique within a frame";
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
    // Stable sort by z, ties by id; stability preserves submission order for full ties
    // (only reachable on the WarnAndResolve path, where a collision was tolerated).
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (layers[a].z != layers[b].z) return layers[a].z < layers[b].z;
        return static_cast<std::uint32_t>(layers[a].id) < static_cast<std::uint32_t>(layers[b].id);
    });
    return order;
}

}  // namespace gbcpp
