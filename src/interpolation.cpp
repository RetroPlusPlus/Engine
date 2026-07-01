#include "retropp/interpolation.h"

#include <variant>

namespace retropp {

namespace {
[[nodiscard]] std::uint32_t raw(LayerId id) noexcept { return static_cast<std::uint32_t>(id); }
[[nodiscard]] std::uint32_t raw(SpriteId id) noexcept { return static_cast<std::uint32_t>(id); }

// Commit one object's motion into its slot: mount on first sight (prev == cur, marked changed since it has
// no prior upload), otherwise rotate prev <- cur and store the new cur, flagging whether it actually moved.
template <class Map, class Motion>
void commit(Map& map, std::uint32_t id, const Motion& m) {
    auto [it, inserted] = map.try_emplace(id);
    if (inserted) {
        it->second.prev    = m;
        it->second.cur     = m;
        it->second.changed = true;
    } else {
        it->second.prev    = it->second.cur;
        it->second.changed = !(it->second.cur == m);
        it->second.cur     = m;
    }
}

// Drop slots whose id was not seen this tick (a despawn).
template <class Map, class Set>
void unmountGone(Map& map, const Set& seen) {
    for (auto it = map.begin(); it != map.end();) {
        if (seen.count(it->first) != 0) {
            ++it;
        } else {
            it = map.erase(it);
        }
    }
}
}  // namespace

void Interpolator::reconcile(const FrameDrawState& submission) {
    seenLayers_.clear();
    seenSprites_.clear();

    for (const DrawLayer& layer : submission.layers) {
        if (const std::uint32_t id = raw(layer.id); id != 0u) {
            commit(layers_, id, LayerMotion{layer.scroll, layer.alpha, layer.transform});
            seenLayers_.insert(id);
        }
        if (contentKind(layer.content) != LayerContentKind::Sprites) continue;
        for (const Sprite& s : std::get<SpriteContent>(layer.content).sprites) {
            if (const std::uint32_t id = raw(s.id); id != 0u) {
                commit(sprites_, id, SpriteMotion{s.x, s.y, s.transform});
                seenSprites_.insert(id);
            }
        }
    }

    unmountGone(layers_, seenLayers_);
    unmountGone(sprites_, seenSprites_);
}

const FrameDrawState& Interpolator::interpolate(const FrameDrawState& submission, float alpha) {
    // Discrete frame fields and the layer list copy straight across; the continuous per-object fields are
    // then overwritten with the eased value. Tile content keeps the submission's cell span (discrete);
    // sprite content is repointed at an interpolated copy in spriteScratch_.
    scratch_.layers.assign(submission.layers.begin(), submission.layers.end());
    scratch_.blend       = submission.blend;
    scratch_.postEffects = submission.postEffects;
    scratch_.regions     = submission.regions;

    std::size_t spriteLayer = 0;
    for (std::size_t i = 0; i < scratch_.layers.size(); ++i) {
        DrawLayer& layer = scratch_.layers[i];

        if (const auto it = layers_.find(raw(layer.id)); it != layers_.end() && raw(layer.id) != 0u) {
            layer.scroll    = lerpScroll(it->second.prev.scroll, layer.scroll, alpha);
            layer.alpha     = lerpF(it->second.prev.alpha, layer.alpha, alpha);
            layer.transform = lerpTransform(it->second.prev.transform, layer.transform, alpha);
        }  // unmatched (spawn / id 0) snaps to the submission, already in place.

        if (contentKind(layer.content) != LayerContentKind::Sprites) continue;

        const auto& src = std::get<SpriteContent>(submission.layers[i].content).sprites;
        if (spriteScratch_.size() <= spriteLayer) spriteScratch_.emplace_back();
        std::vector<Sprite>& dst = spriteScratch_[spriteLayer];
        dst.assign(src.begin(), src.end());
        for (Sprite& s : dst) {
            if (const auto it = sprites_.find(raw(s.id)); it != sprites_.end() && raw(s.id) != 0u) {
                s.x         = lerpRound(it->second.prev.x, s.x, alpha);
                s.y         = lerpRound(it->second.prev.y, s.y, alpha);
                s.transform = lerpTransform(it->second.prev.transform, s.transform, alpha);
            }
        }
        layer.content = SpriteContent{std::span<const Sprite>(dst.data(), dst.size())};
        ++spriteLayer;
    }

    return scratch_;
}

void Interpolator::clear() {
    layers_.clear();
    sprites_.clear();
    seenLayers_.clear();
    seenSprites_.clear();
    scratch_.layers.clear();
    scratch_.postEffects.clear();
    scratch_.regions.clear();
    spriteScratch_.clear();
}

std::optional<LayerMotion> Interpolator::layerPrev(LayerId id) const {
    const auto it = layers_.find(raw(id));
    if (it == layers_.end()) return std::nullopt;
    return it->second.prev;
}
std::optional<LayerMotion> Interpolator::layerCur(LayerId id) const {
    const auto it = layers_.find(raw(id));
    if (it == layers_.end()) return std::nullopt;
    return it->second.cur;
}
std::optional<SpriteMotion> Interpolator::spritePrev(SpriteId id) const {
    const auto it = sprites_.find(raw(id));
    if (it == sprites_.end()) return std::nullopt;
    return it->second.prev;
}
std::optional<SpriteMotion> Interpolator::spriteCur(SpriteId id) const {
    const auto it = sprites_.find(raw(id));
    if (it == sprites_.end()) return std::nullopt;
    return it->second.cur;
}
bool Interpolator::layerChanged(LayerId id) const {
    const auto it = layers_.find(raw(id));
    return it != layers_.end() && it->second.changed;
}
bool Interpolator::spriteChanged(SpriteId id) const {
    const auto it = sprites_.find(raw(id));
    return it != sprites_.end() && it->second.changed;
}

}  // namespace retropp
