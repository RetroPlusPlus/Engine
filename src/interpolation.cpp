#include "retropp/interpolation.h"

#include <string>
#include <variant>

namespace retropp {

namespace {
// Commit one object's motion into its slot by key: mount on first sight (prev == cur, marked changed
// since it has no prior upload), otherwise rotate prev <- cur and store the new cur, flagging whether it
// actually moved.
template <class Map, class Motion>
void commit(Map& map, const std::string& key, const Motion& m) {
    auto [it, inserted] = map.try_emplace(key);
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

// Drop slots whose key was not seen this tick (a despawn).
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
        if (const std::string_view lk = layer.key; !lk.empty()) {
            std::string key(lk);
            commit(layers_, key, LayerMotion{layer.scroll, layer.alpha, layer.transform});
            seenLayers_.insert(std::move(key));
        }
        if (contentKind(layer.content) != LayerContentKind::Sprites) continue;
        for (const Sprite& s : std::get<SpriteContent>(layer.content).sprites) {
            if (const std::string_view sk = s.key; !sk.empty()) {
                std::string key(sk);
                commit(sprites_, key, SpriteMotion{s.x, s.y, s.alpha, s.transform});
                seenSprites_.insert(std::move(key));
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

        if (const std::string_view lk = layer.key; !lk.empty()) {
            if (const auto it = layers_.find(std::string(lk)); it != layers_.end()) {
                layer.scroll    = lerpScroll(it->second.prev.scroll, layer.scroll, alpha);
                layer.alpha     = lerpF(it->second.prev.alpha, layer.alpha, alpha);
                layer.transform = lerpTransform(it->second.prev.transform, layer.transform, alpha);
            }  // unmatched (spawn / empty key) snaps to the submission, already in place.
        }

        if (contentKind(layer.content) != LayerContentKind::Sprites) continue;

        const auto& src = std::get<SpriteContent>(submission.layers[i].content).sprites;
        if (spriteScratch_.size() <= spriteLayer) spriteScratch_.emplace_back();
        std::vector<Sprite>& dst = spriteScratch_[spriteLayer];
        dst.assign(src.begin(), src.end());
        for (Sprite& s : dst) {
            const std::string_view sk = s.key;
            if (sk.empty()) continue;
            if (const auto it = sprites_.find(std::string(sk)); it != sprites_.end()) {
                s.x         = lerpRound(it->second.prev.x, s.x, alpha);
                s.y         = lerpRound(it->second.prev.y, s.y, alpha);
                s.alpha     = lerpF(it->second.prev.alpha, s.alpha, alpha);
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

std::optional<Vec2> Interpolator::interpolatedLayerScroll(std::string_view key, float alpha) const {
    const auto it = layers_.find(std::string(key));
    if (it == layers_.end()) return std::nullopt;
    const LayerMotion& p = it->second.prev;
    const LayerMotion& c = it->second.cur;
    return Vec2{lerpF(static_cast<float>(p.scroll.x), static_cast<float>(c.scroll.x), alpha),
                lerpF(static_cast<float>(p.scroll.y), static_cast<float>(c.scroll.y), alpha)};
}

std::optional<Vec2> Interpolator::interpolatedSpritePos(std::string_view key, float alpha) const {
    const auto it = sprites_.find(std::string(key));
    if (it == sprites_.end()) return std::nullopt;
    const SpriteMotion& p = it->second.prev;
    const SpriteMotion& c = it->second.cur;
    return Vec2{lerpF(static_cast<float>(p.x), static_cast<float>(c.x), alpha),
                lerpF(static_cast<float>(p.y), static_cast<float>(c.y), alpha)};
}

std::optional<LayerMotion> Interpolator::layerPrev(std::string_view key) const {
    const auto it = layers_.find(std::string(key));
    if (it == layers_.end()) return std::nullopt;
    return it->second.prev;
}
std::optional<LayerMotion> Interpolator::layerCur(std::string_view key) const {
    const auto it = layers_.find(std::string(key));
    if (it == layers_.end()) return std::nullopt;
    return it->second.cur;
}
std::optional<SpriteMotion> Interpolator::spritePrev(std::string_view key) const {
    const auto it = sprites_.find(std::string(key));
    if (it == sprites_.end()) return std::nullopt;
    return it->second.prev;
}
std::optional<SpriteMotion> Interpolator::spriteCur(std::string_view key) const {
    const auto it = sprites_.find(std::string(key));
    if (it == sprites_.end()) return std::nullopt;
    return it->second.cur;
}
bool Interpolator::layerChanged(std::string_view key) const {
    const auto it = layers_.find(std::string(key));
    return it != layers_.end() && it->second.changed;
}
bool Interpolator::spriteChanged(std::string_view key) const {
    const auto it = sprites_.find(std::string(key));
    return it != sprites_.end() && it->second.changed;
}

}  // namespace retropp
