#include "retropp/interpolation.h"

#include <string>
#include <variant>

namespace retropp {

namespace {
// Commit one object's motion into its slot by key. Mount on first sight (prev == cur, marked changed since
// it has no prior upload). A motion that differs from the one held rotates prev <- cur, stores the new cur,
// and records the tick and span it changed on — that pair is what the ease runs between, and it must be the
// two values the object actually held, not the two most recent submissions. A motion that matches the one
// held leaves the pair alone while the ease is still running, then collapses it once the object's cadence
// has elapsed, so a world that has stopped reads settled.
template <class Map, class Motion>
void commit(Map& map, const std::string& key, const Motion& m, std::uint64_t tick, std::uint32_t span,
            std::uint32_t cadence) {
    auto [it, inserted] = map.try_emplace(key);
    auto& slot          = it->second;
    slot.cadence        = cadence;
    if (inserted || !(slot.cur == m)) {
        slot.prev          = inserted ? m : slot.cur;
        slot.cur           = m;
        slot.changed       = true;
        slot.changedAtTick = tick;
        slot.spanAtChange  = span;
    } else {
        slot.changed = false;
        if (tick - slot.changedAtTick >= cadence) slot.prev = slot.cur;  // the ease finished — settle
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

void Interpolator::reconcile(const FrameDrawState& submission, const FrameTiming& timing) {
    seenLayers_.clear();
    seenSprites_.clear();
    tick_ += timing.commitSpan;

    for (const DrawLayer& layer : submission.layers) {
        // Sprites advance with the layer they live in, so one resolution serves the layer and its contents.
        const std::uint32_t cadence = resolveCadence(layer.advancesEvery, submission.advancesEvery);
        if (const std::string_view lk = layer.key; !lk.empty()) {
            std::string key(lk);
            commit(layers_, key, LayerMotion{layer.scroll, layer.alpha, layer.transform}, tick_,
                   timing.commitSpan, cadence);
            seenLayers_.insert(std::move(key));
        }
        if (contentKind(layer.content) != LayerContentKind::Sprites) continue;
        for (const Sprite& s : std::get<SpriteContent>(layer.content).sprites) {
            if (const std::string_view sk = s.key; !sk.empty()) {
                std::string key(sk);
                commit(sprites_, key, SpriteMotion{s.x, s.y, s.alpha, s.transform, s.pivot, s.origin},
                       tick_, timing.commitSpan, cadence);
                seenSprites_.insert(std::move(key));
            }
        }
    }

    unmountGone(layers_, seenLayers_);
    unmountGone(sprites_, seenSprites_);
}

const FrameDrawState& Interpolator::interpolate(const FrameDrawState& submission,
                                                const FrameTiming&    timing) {
    // Discrete frame fields and the layer list copy straight across; the continuous per-object fields are
    // then overwritten with the eased value, each at its own object's factor. Tile content keeps the
    // submission's cell span (discrete); sprite content is repointed at an interpolated copy in
    // spriteScratch_.
    scratch_.layers.assign(submission.layers.begin(), submission.layers.end());
    scratch_.blend         = submission.blend;
    scratch_.postEffects   = submission.postEffects;
    scratch_.regions       = submission.regions;
    scratch_.advancesEvery = submission.advancesEvery;

    std::size_t spriteLayer = 0;
    for (std::size_t i = 0; i < scratch_.layers.size(); ++i) {
        DrawLayer& layer = scratch_.layers[i];

        if (const std::string_view lk = layer.key; !lk.empty()) {
            if (const auto it = layers_.find(std::string(lk)); it != layers_.end()) {
                const float f   = factorFor(it->second, timing);
                layer.scroll    = lerpScroll(it->second.prev.scroll, layer.scroll, f);
                layer.alpha     = lerpF(it->second.prev.alpha, layer.alpha, f);
                layer.transform = lerpTransform(it->second.prev.transform, layer.transform, f);
            }  // unmatched (a spawn with no history) snaps to the submission, already in place.
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
                const float f = factorFor(it->second, timing);
                s.x         = lerpRound(it->second.prev.x, s.x, f);
                s.y         = lerpRound(it->second.prev.y, s.y, f);
                s.alpha     = lerpF(it->second.prev.alpha, s.alpha, f);
                s.transform = lerpTransform(it->second.prev.transform, s.transform, f);
                s.pivot     = lerpPoint(it->second.prev.pivot, s.pivot, f);
                s.origin    = lerpPoint(it->second.prev.origin, s.origin, f);
            }
        }
        layer.content = SpriteContent{std::span<const Sprite>(dst.data(), dst.size())};
        ++spriteLayer;
    }

    return scratch_;
}

void Interpolator::clear() {
    tick_ = 0;
    layers_.clear();
    sprites_.clear();
    seenLayers_.clear();
    seenSprites_.clear();
    scratch_.layers.clear();
    scratch_.postEffects.clear();
    scratch_.regions.clear();
    spriteScratch_.clear();
}

std::optional<Vec2> Interpolator::interpolatedLayerScroll(std::string_view   key,
                                                          const FrameTiming& timing) const {
    const auto it = layers_.find(std::string(key));
    if (it == layers_.end()) return std::nullopt;
    const float        f = factorFor(it->second, timing);
    const LayerMotion& p = it->second.prev;
    const LayerMotion& c = it->second.cur;
    return Vec2{lerpF(static_cast<float>(p.scroll.x), static_cast<float>(c.scroll.x), f),
                lerpF(static_cast<float>(p.scroll.y), static_cast<float>(c.scroll.y), f)};
}

std::optional<Vec2> Interpolator::interpolatedSpritePos(std::string_view   key,
                                                        const FrameTiming& timing) const {
    const auto it = sprites_.find(std::string(key));
    if (it == sprites_.end()) return std::nullopt;
    const float         f = factorFor(it->second, timing);
    const SpriteMotion& p = it->second.prev;
    const SpriteMotion& c = it->second.cur;
    return Vec2{lerpF(static_cast<float>(p.x), static_cast<float>(c.x), f),
                lerpF(static_cast<float>(p.y), static_cast<float>(c.y), f)};
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

bool Interpolator::allSettled() const noexcept {
    for (const auto& [key, slot] : layers_)  if (!(slot.prev == slot.cur)) return false;
    for (const auto& [key, slot] : sprites_) if (!(slot.prev == slot.cur)) return false;
    return true;
}

}  // namespace retropp
